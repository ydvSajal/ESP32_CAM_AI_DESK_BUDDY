// ai.cpp — A2. aiTask on CORE_SERVICES prio 2, queue depth 1.
// One code path, two backends: both are asked to stream SSE and the chunks are either
// forwarded to /ws or accumulated, depending on what the client asked for.
// T8: the key exists in exactly two places — Settings and one HTTP header. It is never
// logged, never put in a URL, never echoed in an error.
#include "ai.h"
#include "config.h"
#include "storage.h"
#include "webserver.h"
#include "display.h"     // A4 wants the Q/A pushed, not polled — aiLastQA() exists too

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_system.h>     // esp_random()

#if ENABLE_AI

static SemaphoreHandle_t g_mtx = nullptr;   // guards req/result/lastQA
static SemaphoreHandle_t g_done = nullptr;  // binary: given when a request finishes
static TaskHandle_t      g_task = nullptr;
static volatile bool     g_busy = false;

static String g_id, g_prompt, g_provider;
static bool   g_stream = false;

static String   g_answer, g_error;
static uint32_t g_elapsed = 0;
static String   g_lastQ, g_lastA;

// ---------------------------------------------------------------- small helpers

bool aiBusy() { return g_busy; }

String aiEffectiveProvider(const String& want) {
  if (want.length()) return want;
  return settings.aiProvider.length() ? settings.aiProvider : String("openrouter");
}

bool aiHasKey(const String& provider) {
  return provider == "gemini" ? settings.geminiKey.length() > 0 : settings.openrouterKey.length() > 0;
}

void aiLastQA(String& question, String& answer) {
  if (!g_mtx) { question = ""; answer = ""; return; }
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  question = g_lastQ;
  answer = g_lastA;
  xSemaphoreGive(g_mtx);
}

// T8 belt-and-braces: nothing leaving this module may contain either key.
static String safe(const String& s) {
  std::string t = s.c_str();
  t = redactSecret(t, settings.openrouterKey.c_str());
  t = redactSecret(t, settings.geminiKey.c_str());
  return String(t.c_str());
}

// ---------------------------------------------------------------- submit / wait

bool aiSubmit(const String& prompt, const String& provider, bool stream, String& id) {
  if (!g_mtx || !g_done || !g_task || g_busy) return false;   // aiTask not up yet, or in flight
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  if (g_busy) { xSemaphoreGive(g_mtx); return false; }     // lost the race: 409
  char buf[8];
  snprintf(buf, sizeof buf, "%04x", (unsigned)(esp_random() & 0xFFFF));
  g_id = buf;
  g_prompt = prompt;
  g_provider = provider;
  g_stream = stream;
  g_answer = "";
  g_error = "";
  g_elapsed = 0;
  g_busy = true;
  id = g_id;
  xSemaphoreGive(g_mtx);

  xSemaphoreTake(g_done, 0);              // clear a stale completion from an abandoned wait
  xTaskNotifyGive(g_task);
  return true;
}

bool aiWaitResult(const String& id, String& answer, String& error, uint32_t& elapsedMs,
                  uint32_t timeoutMs) {
  if (!g_done) return false;
  if (xSemaphoreTake(g_done, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  bool mine = (g_id == id);
  answer = g_answer;
  error = g_error;
  elapsedMs = g_elapsed;
  xSemaphoreGive(g_mtx);
  return mine;
}

// ---------------------------------------------------------------- provider call

static void pushChunk(const String& id, uint32_t seq, const std::string& text) {
  String p = String("{\"id\":\"") + id + "\",\"seq\":" + seq + ",\"text\":\"" +
             jsonEscape(text).c_str() + "\"}";
  wsBroadcast("ai_chunk", p);
}

// Returns false with `err` set. `err` is already user-safe (no key, no provider body).
static bool runProvider(const String& provider, const String& prompt, bool emitChunks,
                        const String& id, String& answer, String& err, uint32_t& chunks) {
  // ponytail: setInsecure() — same call as the uploader, same ceiling. Ship a CA bundle if
  // this device ever answers from outside the LAN.
  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(5);                          // seconds; the deadline loop below is the real bound
  HTTPClient http;
  http.setTimeout(AI_TIMEOUT_MS);
  http.setReuse(false);

  String url;
  if (provider == "gemini")
    url = String(GEMINI_HOST) + "/v1beta/models/" + GEMINI_MODEL + ":streamGenerateContent?alt=sse";
  else
    url = OPENROUTER_URL;

  if (!http.begin(c, url)) { err = "cannot reach provider"; return false; }
  http.addHeader("Content-Type", "application/json");
  if (provider == "gemini") {
    http.addHeader("x-goog-api-key", settings.geminiKey);   // header, never the query string
  } else {
    http.addHeader("Authorization", "Bearer " + settings.openrouterKey);
    http.addHeader("HTTP-Referer", "http://deskbuddy.local");
    http.addHeader("X-Title", "deskbuddy");
  }

  std::string body = aiRequestBody(provider.c_str(), OPENROUTER_MODEL, prompt.c_str());
  int code = http.POST((uint8_t*)body.data(), body.size());

  if (code < 0) {
    err = String("network error: ") + HTTPClient::errorToString(code);
    http.end();
    return false;
  }
  if (code != 200) {
    // The provider's body can contain the key we sent. It is read and dropped, never quoted.
    http.getString();
    http.end();
    err = String("provider returned ") + code;
    return false;
  }

  WiFiClient* s = http.getStreamPtr();
  uint32_t deadline = millis() + AI_TIMEOUT_MS;
  chunks = 0;
  while (millis() < deadline) {
    if (!s->available()) {
      if (!http.connected()) break;
      vTaskDelay(pdMS_TO_TICKS(10));       // core 1 has a web server and a display to run
      continue;
    }
    String line = s->readStringUntil('\n');
    if (line.length() == 0) continue;
    if (line.indexOf("[DONE]") >= 0) break;
    std::string t = sseExtractText(provider.c_str(), line.c_str());
    if (t.empty()) continue;
    if (answer.length() + t.size() <= AI_ANSWER_MAX) answer += t.c_str();
    if (emitChunks) pushChunk(id, chunks, t);
    chunks++;
  }
  http.end();

  if (answer.length() == 0 && chunks == 0) { err = "provider returned no content"; return false; }
  return true;
}

// ---------------------------------------------------------------- task

extern "C" void aiTask(void* pv) {
  g_mtx = xSemaphoreCreateMutex();
  g_done = xSemaphoreCreateBinary();
  g_task = xTaskGetCurrentTaskHandle();

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    xSemaphoreTake(g_mtx, portMAX_DELAY);
    String id = g_id, prompt = g_prompt, provider = g_provider;
    bool emit = g_stream;
    xSemaphoreGive(g_mtx);

    uint32_t t0 = millis();
    String answer, err;
    uint32_t chunks = 0;

    if (WiFi.status() != WL_CONNECTED) {
      err = "no wifi";
    } else if (!aiHasKey(provider)) {
      err = "no api key for provider " + provider;
    } else {
      runProvider(provider, prompt, emit, id, answer, err, chunks);
    }
    err = safe(err);
    uint32_t elapsed = millis() - t0;

    xSemaphoreTake(g_mtx, portMAX_DELAY);
    g_answer = answer;
    g_error = err;
    g_elapsed = elapsed;
    if (err.isEmpty()) { g_lastQ = prompt; g_lastA = answer; }   // A4's display screen
    g_busy = false;
    xSemaphoreGive(g_mtx);
    // A failed request must NOT wipe the last good answer off the display (T5.6.3).
    if (err.isEmpty()) displaySetAiText(prompt, answer);

    // ai_done is sent for every request, streamed or not (docs/API.md 4).
    JsonDocument d;
    d["id"] = id;
    d["provider"] = provider;
    d["chunks"] = chunks;
    d["elapsed_ms"] = elapsed;
    if (err.length()) d["error"] = err; else d["error"] = nullptr;
    String p;
    serializeJson(d, p);
    wsBroadcast("ai_done", p);
    if (err.length()) Serial.printf("[ai] %s failed: %s\n", provider.c_str(), err.c_str());

    xSemaphoreGive(g_done);
  }
}

#else   // !ENABLE_AI

extern "C" void aiTask(void* pv) { for (;;) vTaskDelay(pdMS_TO_TICKS(1000)); }
bool aiBusy() { return false; }
bool aiSubmit(const String&, const String&, bool, String&) { return false; }
bool aiHasKey(const String&) { return false; }
String aiEffectiveProvider(const String& w) { return w; }
bool aiWaitResult(const String&, String&, String&, uint32_t&, uint32_t) { return false; }
void aiLastQA(String& q, String& a) { q = ""; a = ""; }

#endif
