// webserver.cpp — A2. netTask on CORE_SERVICES. Implements docs/API.md exactly.
// Nothing here may block core 0: every handler is O(fast) or hands off to another task.
#include "webserver.h"
#include "config.h"
#include "storage.h"
#include "camera.h"
#include "recorder.h"
#include "uploader.h"
#include "ai.h"

#include <WiFi.h>
#include <SD_MMC.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <time.h>

static AsyncWebServer server(HTTP_PORT);
static AsyncWebSocket ws("/ws");
static bool g_setupMode = false;
static uint32_t g_reboots = 0;

// netTask is the ONLY task allowed to call into AsyncWebSocket. Everyone else posts here.
// ponytail: fixed-depth queue of strdup'd frames, dropped when full. Upgrade path if a
// burst ever matters: coalesce by type instead of dropping.
static QueueHandle_t wsQ = nullptr;

// ---------------------------------------------------------------- helpers

static void sendErr(AsyncWebServerRequest* r, int code, const char* err, const char* msg) {
  char buf[224];
  snprintf(buf, sizeof buf, "{\"error\":\"%s\",\"message\":\"%s\"}", err, msg);
  r->send(code, "application/json", buf);
}

// docs/API.md 1: exact string compare; empty PIN in NVS disables auth entirely.
// `allowQueryPin` is ONLY set on the three browser-native URLs (snapshot, stream,
// recordings/file) — <img src> and <a download> cannot send a header. Everything else
// stays header-only so the PIN does not end up in referrers and proxy logs by default.
static bool authed(AsyncWebServerRequest* r, bool allowQueryPin = false) {
  if (settings.devicePin.length() == 0) return true;
  if (r->hasHeader("X-Device-Pin") && r->header("X-Device-Pin") == settings.devicePin) return true;
  if (allowQueryPin && r->hasParam("pin") && r->getParam("pin")->value() == settings.devicePin)
    return true;
  return false;
}
#define REQUIRE_AUTH_Q(r, q) \
  if (!authed(r, q)) { sendErr(r, 401, "unauthorized", "Missing or invalid X-Device-Pin"); return; }
#define REQUIRE_AUTH(r) REQUIRE_AUTH_Q(r, false)

static uint32_t nowEpoch() { return (uint32_t)time(nullptr); }

// ---------------------------------------------------------------- status

String statusJson() {
  JsonDocument d;
  d["device"] = DEVICE_NAME;
  d["fw"] = FW_VERSION;
  d["uptime_s"] = (uint32_t)(millis() / 1000);

  time_t now = time(nullptr);
  d["time"] = (uint32_t)now;
  struct tm utc;
  gmtime_r(&now, &utc);
  char iso[32];
  strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &utc);
  d["time_iso"] = iso;
  d["tz_offset_min"] = settings.tzOffsetMin;
  d["time_synced"] = now > 1600000000;            // NTP has clearly landed
  d["pin_required"] = settings.devicePin.length() > 0;
  d["setup_mode"] = g_setupMode;

  d["heap_free"] = (uint32_t)ESP.getFreeHeap();
  d["heap_min"] = (uint32_t)esp_get_minimum_free_heap_size();
  d["psram_free"] = (uint32_t)ESP.getFreePsram();

  JsonObject w = d["wifi"].to<JsonObject>();
  bool sta = WiFi.status() == WL_CONNECTED;
  w["connected"] = sta;
  w["ssid"] = sta ? WiFi.SSID() : String("");
  w["ip"] = (g_setupMode ? WiFi.softAPIP() : WiFi.localIP()).toString();
  w["rssi"] = sta ? WiFi.RSSI() : 0;

  uint64_t tot = 0, used = 0, freeMb = 0;
  recorderUsage(&tot, &used, &freeMb);
  JsonObject s = d["sd"].to<JsonObject>();
  s["mounted"] = sdMounted();
  s["total_mb"] = tot;
  s["used_mb"] = used;
  s["free_mb"] = freeMb;

  JsonObject rec = d["recording"].to<JsonObject>();
  rec["active"] = recordingActive();
  rec["fps"] = cameraFps();
  rec["current_file"] = recorderCurrentFile();
  rec["segment_elapsed_s"] = recorderSegmentElapsed();
  rec["segments_today"] = recorderSegmentsToday();
  rec["dropped_frames"] = cameraDropped();

  JsonObject up = d["upload"].to<JsonObject>();
#if ENABLE_UPLOAD
  up["enabled"] = true;
  up["queued"] = uploadQueueDepth();
  const char* cur = uploadCurrentFile();
  if (cur && *cur) up["uploading"] = cur; else up["uploading"] = nullptr;
  up["progress_pct"] = uploadProgressPct();
  up["last_ok"] = uploadLastOk();
  const char* uerr = uploadLastError();
  if (uerr && *uerr) up["last_error"] = uerr; else up["last_error"] = nullptr;
#else
  up["enabled"] = false; up["queued"] = 0; up["uploading"] = nullptr;
  up["progress_pct"] = 0; up["last_ok"] = 0; up["last_error"] = nullptr;
#endif

  JsonObject ai = d["ai"].to<JsonObject>();
#if ENABLE_AI
  ai["enabled"] = true;
  ai["provider"] = settings.aiProvider;
  ai["busy"] = aiBusy();
#else
  ai["enabled"] = false; ai["provider"] = ""; ai["busy"] = false;
#endif

  d["reboots"] = g_reboots;

  String out;
  serializeJson(d, out);
  return out;
}

// ---------------------------------------------------------------- websocket

void wsBroadcast(const char* type, const String& payloadJson) {
  if (!wsQ) return;
  String frame = String("{\"type\":\"") + type + "\",\"ts\":" + nowEpoch() +
                 ",\"payload\":" + payloadJson + "}";
  char* copy = strdup(frame.c_str());
  if (!copy) return;
  if (xQueueSend(wsQ, &copy, 0) != pdTRUE) free(copy);   // full: drop, never block a task
}

void wsError(const char* source, const char* code, const String& message, bool fatal) {
  JsonDocument p;
  p["source"] = source;
  p["code"] = code;
  p["message"] = message;      // callers pass an already-redacted string (T8)
  p["fatal"] = fatal;
  String s;
  serializeJson(p, s);
  wsBroadcast("error", s);
}

static void wsDrain() {
  if (!wsQ) return;      // webserverBegin() never ran (booted with no Wi-Fi) — netTask must not
  char* msg;             // hand a null handle to xQueueReceive.
  while (xQueueReceive(wsQ, &msg, 0) == pdTRUE) {
    ws.textAll(msg);
    free(msg);
  }
}

// ---------------------------------------------------------------- snapshot

static void handleSnapshot(AsyncWebServerRequest* r) {
  REQUIRE_AUTH_Q(r, true)      // <img src> cannot set a header (docs/API.md 3.2)
  uint8_t* src = nullptr;
  size_t len = 0;
  uint32_t fid = 0;
  if (!cameraReady() || !cameraGetLatestJpeg(&src, &len, &fid)) {
    sendErr(r, 503, "unavailable", "camera not ready");
    return;
  }
  // Copy out and release the ring slot NOW. The async response outlives this handler, and a
  // slow HTTP client must cost us PSRAM, never a pinned ring slot (camera.h contract).
  uint8_t* copy = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!copy) copy = (uint8_t*)malloc(len);
  uint32_t ts = (uint32_t)time(nullptr);
  if (copy) memcpy(copy, src, len);
  cameraReleaseJpeg(fid);
  if (!copy) { sendErr(r, 503, "unavailable", "out of memory for snapshot"); return; }

  AsyncWebServerResponse* res = r->beginResponse(
      "image/jpeg", len,
      [copy, len](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
        size_t n = len - index;
        if (n > maxLen) n = maxLen;
        memcpy(buf, copy + index, n);
        return n;
      });
  res->addHeader("Cache-Control", "no-store");
  res->addHeader("X-Timestamp", String(ts));
  r->onDisconnect([copy]() { free(copy); });
  r->send(res);
}

// ---------------------------------------------------------------- MJPEG stream

// One client max (STREAM_MAX_CLIENTS). State is per-stream and there is only ever one.
static volatile bool s_busy = false;
static uint8_t* s_buf = nullptr;
static size_t s_cap = 0, s_len = 0, s_sent = 0;
static uint32_t s_lastFrameMs = 0;

static void streamRelease() {
  s_busy = false;
  if (s_buf) { free(s_buf); s_buf = nullptr; s_cap = 0; }
  s_len = s_sent = 0;
}

static size_t streamFill(uint8_t* buf, size_t maxLen, size_t /*index*/) {
  if (s_len == 0) {                                  // need the next frame
    if (millis() - s_lastFrameMs < (uint32_t)(1000 / STREAM_FPS)) return RESPONSE_TRY_AGAIN;
    uint8_t* src = nullptr;
    size_t jlen = 0;
    uint32_t fid = 0;
    if (!cameraReady() || !cameraGetLatestJpeg(&src, &jlen, &fid)) return RESPONSE_TRY_AGAIN;
    size_t need = 160 + jlen + 2;
    if (s_cap < need) {
      uint8_t* nb = (uint8_t*)heap_caps_realloc(s_buf, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!nb) nb = (uint8_t*)realloc(s_buf, need);
      if (!nb) { cameraReleaseJpeg(fid); return RESPONSE_TRY_AGAIN; }
      s_buf = nb; s_cap = need;
    }
    int n = snprintf((char*)s_buf, 160,
                     "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %u\r\n\r\n",
                     (unsigned)jlen, (unsigned)time(nullptr));
    memcpy(s_buf + n, src, jlen);
    cameraReleaseJpeg(fid);                           // shortest possible hold on the ring
    memcpy(s_buf + n + jlen, "\r\n", 2);
    s_len = n + jlen + 2;
    s_sent = 0;
    s_lastFrameMs = millis();
  }
  size_t chunk = s_len - s_sent;
  if (chunk > maxLen) chunk = maxLen;
  memcpy(buf, s_buf + s_sent, chunk);
  s_sent += chunk;
  if (s_sent >= s_len) s_len = 0;   // a slow client simply misses the frames it could not
  return chunk;                     // drain — the recorder is never asked to wait.
}

static void handleStream(AsyncWebServerRequest* r) {
  REQUIRE_AUTH_Q(r, true)      // <img src> cannot set a header (docs/API.md 3.3)
  if (!cameraReady()) { sendErr(r, 503, "unavailable", "camera not ready"); return; }
  if (s_busy) { sendErr(r, 409, "busy", "stream already in use"); return; }
  s_busy = true;
  s_len = s_sent = 0;
  s_lastFrameMs = 0;
  AsyncWebServerResponse* res =
      r->beginChunkedResponse("multipart/x-mixed-replace; boundary=frame", streamFill);
  res->addHeader("Cache-Control", "no-store");
  r->onDisconnect(streamRelease);
  r->send(res);
}

// ---------------------------------------------------------------- recordings

// Both branches use A1's recListDays/recListSegments (recorder.h) — they hold the SD mutex,
// so this file never touches the filesystem for listings.
// ponytail: the body is assembled in one String and the scratch array is stack/heap-sized by
// `limit`. Ceiling ~limit*140 bytes (~28 KB at the default 200). Upgrade path if a full day
// must ever come back in one response: page with `limit` (already in the contract).
static void handleRecordings(AsyncWebServerRequest* r) {
  REQUIRE_AUTH(r)
  if (!sdMounted()) { sendErr(r, 503, "unavailable", "sd not mounted"); return; }

  uint32_t limit = RECORDINGS_LIMIT_DEF;
  if (r->hasParam("limit")) {
    long v = r->getParam("limit")->value().toInt();
    if (v > 0 && v <= 2000) limit = (uint32_t)v;
  }

  if (!r->hasParam("day")) {
    const uint32_t maxDays = 64;
    RecDay* days = (RecDay*)calloc(maxDays, sizeof(RecDay));
    if (!days) { sendErr(r, 500, "internal", "out of memory"); return; }
    uint32_t n = recListDays(days, maxDays);
    if (n > limit) n = limit;
    String out = "{\"days\":[";
    for (uint32_t i = 0; i < n; i++) {
      if (i) out += ',';
      out += "{\"day\":\"" + String(days[i].day) + "\",\"segments\":" + days[i].segments +
             ",\"bytes\":" + String((unsigned long long)days[i].bytes) + "}";
    }
    free(days);
    uint64_t t = 0, u = 0, f = 0;
    recorderUsage(&t, &u, &f);
    out += "],\"sd_free_mb\":" + String((uint32_t)f) + "}";
    r->send(200, "application/json", out);
    return;
  }

  String day = r->getParam("day")->value();
  if (day.length() != 8) { sendErr(r, 400, "bad_request", "day must be YYYYMMDD"); return; }
  if (!recDayExists(day.c_str())) { sendErr(r, 404, "not_found", "unknown day"); return; }

  RecSegment* segs = (RecSegment*)calloc(limit, sizeof(RecSegment));
  if (!segs) { sendErr(r, 500, "internal", "out of memory"); return; }
  bool truncated = false;
  uint32_t n = recListSegments(day.c_str(), segs, limit, &truncated);

  String out;
  out.reserve(n * 140 + 64);
  out = "{\"day\":\"" + day + "\",\"segments\":[";
  for (uint32_t i = 0; i < n; i++) {
    if (i) out += ',';
    String path = String(REC_DIR) + "/" + day + "/" + segs[i].name;
    out += "{\"name\":\"" + String(segs[i].name) + "\",\"path\":\"" + path +
           "\",\"bytes\":" + String((unsigned long long)segs[i].bytes) +
           ",\"start\":" + segs[i].start + ",\"duration_s\":" + segs[i].durationS +
           ",\"uploaded\":" + (segs[i].uploaded ? "true" : "false") + "}";
  }
  free(segs);
  out += "],\"truncated\":" + String(truncated ? "true" : "false") + "}";
  r->send(200, "application/json", out);
}

static void handleRecordingFile(AsyncWebServerRequest* r) {
  REQUIRE_AUTH_Q(r, true)      // <a download> cannot set a header (docs/API.md 3.6)
  if (!sdMounted()) { sendErr(r, 503, "unavailable", "sd not mounted"); return; }
  if (!r->hasParam("path")) { sendErr(r, 400, "bad_request", "path required"); return; }
  String p = r->getParam("path")->value();
  if (!p.startsWith(String(REC_DIR) + "/") || p.indexOf("..") >= 0) {
    sendErr(r, 400, "bad_request", "path must be under /rec and contain no ..");
    return;
  }
  if (!SD_MMC.exists(p)) { sendErr(r, 404, "not_found", "no such recording"); return; }
  // ponytail: AsyncFileResponse handles Range itself in ESPAsyncWebServer 3.x. If a client
  // ever needs multi-range, that is the upgrade point — v1 needs single-range seek only.
  r->send(SD_MMC, p, "video/x-msvideo");
}

// ---------------------------------------------------------------- settings

// Thin HTTP wrapper. All validation and field-copying lives in storage.cpp's
// settingsApplyJson() — shared with the USB serial provisioning path in main.cpp.
static void applySettings(AsyncWebServerRequest* r, JsonVariant& j, bool requireSsid, bool isSetup) {
  if (!j.is<JsonObject>()) { sendErr(r, 400, "bad_request", "body must be a JSON object"); return; }
  JsonObject o = j.as<JsonObject>();

  String saved, errMsg;
  bool rebootReq = false;
  if (!settingsApplyJson(o, requireSsid, saved, rebootReq, errMsg)) {
    sendErr(r, 400, "bad_request", errMsg.c_str());
    return;
  }

  if (isSetup) {
    r->send(200, "application/json", "{\"ok\":true,\"rebooting_in_ms\":1500}");
    // ponytail: delayed restart via a detached one-shot task — the async handler must return
    // before the socket can flush. 1.5 s is the contract value in docs/API.md 3.10.
    xTaskCreate([](void*) { vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); },
                "rst", 2048, nullptr, 1, nullptr);
    return;
  }
  r->send(200, "application/json",
          String("{\"ok\":true,\"saved\":[") + saved + "],\"reboot_required\":" +
              (rebootReq ? "true" : "false") + "}");
}

static void handleGetSettings(AsyncWebServerRequest* r) {
  REQUIRE_AUTH(r)
  JsonDocument d;
  d["wifi_ssid"] = settings.wifiSsid;                       // not a secret
  d["wifi_pass"] = maskSecret(settings.wifiPass);
  d["device_pin"] = maskSecret(settings.devicePin);
  d["openrouter_key"] = maskSecret(settings.openrouterKey);
  d["gemini_key"] = maskSecret(settings.geminiKey);
  d["drive_refresh_token"] = maskSecret(settings.driveRefreshToken);
  d["drive_client_id"] = settings.driveClientId;            // not a secret (docs/API.md 3.7)
  d["drive_client_secret"] = maskSecret(settings.driveClientSecret);
  d["drive_folder_id"] = settings.driveFolderId;            // not a secret
  d["ai_provider"] = settings.aiProvider;
  JsonObject v = d["video"].to<JsonObject>();
  v["width"] = settings.videoWidth;
  v["height"] = settings.videoHeight;
  v["fps"] = settings.videoFps;
  v["quality"] = settings.videoQuality;
  d["tz_offset_min"] = settings.tzOffsetMin;
  String out;
  serializeJson(d, out);
  r->send(200, "application/json", out);
}

// ---------------------------------------------------------------- AI

static void handleAi(AsyncWebServerRequest* r, JsonVariant& j) {
  if (!authed(r)) { sendErr(r, 401, "unauthorized", "Missing or invalid X-Device-Pin"); return; }
#if !ENABLE_AI
  sendErr(r, 503, "unavailable", "ai disabled"); return;
#else
  if (!j.is<JsonObject>()) { sendErr(r, 400, "bad_request", "body must be a JSON object"); return; }
  JsonObject o = j.as<JsonObject>();
  String prompt = o["prompt"].is<const char*>() ? String(o["prompt"].as<const char*>()) : String("");
  if (prompt.length() == 0 || prompt.length() > AI_PROMPT_MAX) {
    sendErr(r, 400, "bad_request", "prompt must be 1-2000 chars"); return;
  }
  String want = o["provider"].is<const char*>() ? String(o["provider"].as<const char*>()) : String("");
  if (want.length() && want != "openrouter" && want != "gemini") {
    sendErr(r, 400, "bad_request", "unknown provider"); return;
  }
  String provider = aiEffectiveProvider(want);
  bool stream = o["stream"] | false;

  if (WiFi.status() != WL_CONNECTED) { sendErr(r, 503, "unavailable", "no wifi"); return; }
  if (!aiHasKey(provider)) {
    String m = "no api key for provider " + provider;
    sendErr(r, 503, "unavailable", m.c_str());
    return;
  }

  String id;
  if (!aiSubmit(prompt, provider, stream, id)) {
    sendErr(r, 409, "busy", "ai request already in flight"); return;
  }
  if (stream) {
    r->send(202, "application/json",
            String("{\"id\":\"") + id + "\",\"provider\":\"" + provider + "\",\"streaming\":true}");
    return;
  }

  // ponytail: blocking mode parks the AsyncTCP task until the provider answers (<= AI_TIMEOUT_MS).
  // Ceiling: HTTP requests queue behind it for that window (WS pushes still go out — they are
  // posted from netTask through lwIP, not through this task). Upgrade path: the PWA should send
  // stream:true, which is fully non-blocking and is what docs/API.md 3.4 exists for.
  String answer, err;
  uint32_t elapsed = 0;
  if (!aiWaitResult(id, answer, err, elapsed, AI_TIMEOUT_MS)) {
    sendErr(r, 502, "internal", "provider timed out"); return;
  }
  if (err.length()) { sendErr(r, 502, "internal", err.c_str()); return; }

  JsonDocument d;
  d["id"] = id;
  d["provider"] = provider;
  d["answer"] = answer;
  d["elapsed_ms"] = elapsed;
  String out;
  serializeJson(d, out);
  r->send(200, "application/json", out);
#endif
}

// ---------------------------------------------------------------- setup portal

// ponytail: the fallback provisioning form, not a product. No CSS framework, no JS build,
// no field the first boot cannot do without. The real UI is A3's PWA.
static const char SETUP_HTML[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>deskbuddy setup</title>
<style>body{font:16px system-ui;margin:0;padding:24px;background:#111;color:#eee}
input,select{width:100%;box-sizing:border-box;padding:10px;margin:4px 0 14px;font:inherit;
background:#222;color:#eee;border:1px solid #444;border-radius:6px}
button{width:100%;padding:14px;font:600 16px system-ui;background:#3b82f6;color:#fff;border:0;border-radius:6px}
label{font-size:13px;color:#9ca3af}h1{font-size:20px}small{color:#6b7280}</style>
<h1>deskbuddy setup</h1>
<form id=f>
<label>Wi-Fi SSID (required)</label><input name=wifi_ssid required maxlength=32>
<label>Wi-Fi password</label><input name=wifi_pass type=password maxlength=63>
<label>Device PIN (4-12 chars, blank = no auth)</label><input name=device_pin maxlength=12>
<label>OpenRouter API key (optional)</label><input name=openrouter_key type=password>
<label>Gemini API key (optional)</label><input name=gemini_key type=password>
<label>AI provider</label><select name=ai_provider><option>openrouter<option>gemini</select>
<label>Timezone offset, minutes (e.g. 330 = IST)</label><input name=tz_offset_min type=number value=0>
<button>Save &amp; reboot</button></form>
<p id=m><small>Keys are stored in NVS on the device only. Drive credentials are added later
from the web app &mdash; see docs/get_drive_token.md.</small></p>
<script>f.onsubmit=async e=>{e.preventDefault();
const b={};for(const[k,v]of new FormData(f))if(v!=='')b[k]=v;
if(b.tz_offset_min!==undefined)b.tz_offset_min=+b.tz_offset_min;
m.textContent='saving...';
const r=await fetch('/api/setup',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify(b)});
m.textContent=r.ok?'saved - rebooting, reconnect to your Wi-Fi':'error: '+await r.text();};
</script>)HTML";

// ---------------------------------------------------------------- wiring

void webserverBegin(bool setupMode) {
  g_setupMode = setupMode;

  {  // boot counter for status.reboots — own key, does not collide with storage.cpp's set
    Preferences p;
    p.begin("deskbuddy", false);
    g_reboots = p.getUInt("reboots", 0) + 1;
    p.putUInt("reboots", g_reboots);
    p.end();
  }

  wsQ = xQueueCreate(WS_QUEUE_DEPTH, sizeof(char*));

  // The PWA lives in flash (LittleFS), served at / — same origin as /api/*, which is the
  // whole point: an HTTPS page cannot call this device's plain-HTTP LAN address.
  // `pio run -t uploadfs` builds the image from webapp/ (data_dir in platformio.ini).
  bool fsOk = LittleFS.begin(false);
  if (!fsOk) Serial.println("[net] LittleFS mount failed — run 'pio run -t uploadfs'");

  // CORS is no longer required (the app is same-origin) but is kept: docs/DEPLOY.md still
  // documents serving the PWA from a static host as an optional path, and the mock server
  // and every T8.7 row depend on it. Four default headers, no code path, no risk.
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, X-Device-Pin");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Max-Age", "600");

  ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient* c, AwsEventType type, void* arg,
                uint8_t*, size_t) {
    if (type != WS_EVT_CONNECT) return;      // client->server frames are ignored in v1
    if (settings.devicePin.length() == 0) return;
    AsyncWebServerRequest* req = (AsyncWebServerRequest*)arg;
    const AsyncWebParameter* p = req ? req->getParam("pin") : nullptr;
    if (!p || p->value() != settings.devicePin) c->close(4401, "unauthorized");
  });
  server.addHandler(&ws);

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r) {   // auth-exempt by contract
    r->send(200, "application/json", statusJson());
  });
  server.on("/api/snapshot", HTTP_GET, handleSnapshot);
  server.on("/api/stream", HTTP_GET, handleStream);
  server.on("/api/recordings", HTTP_GET, handleRecordings);
  server.on("/api/recordings/file", HTTP_GET, handleRecordingFile);
  server.on("/api/settings", HTTP_GET, handleGetSettings);
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* r) {
    REQUIRE_AUTH(r)
    r->send(200, "application/json", "{\"ok\":true,\"rebooting_in_ms\":500}");
    xTaskCreate([](void*) { vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); },
                "rst", 2048, nullptr, 1, nullptr);
  });

  auto* aiH = new AsyncCallbackJsonWebHandler("/api/ai", handleAi);
  aiH->setMethod(HTTP_POST);
  server.addHandler(aiH);

  auto* setH = new AsyncCallbackJsonWebHandler(
      "/api/settings", [](AsyncWebServerRequest* r, JsonVariant& j) {
        if (!authed(r)) { sendErr(r, 401, "unauthorized", "Missing or invalid X-Device-Pin"); return; }
        applySettings(r, j, false, false);
      });
  setH->setMethod(HTTP_POST);
  server.addHandler(setH);

  // /api/setup exists only in SoftAP mode; in station mode it must 404 (docs/API.md 3.10).
  auto* supH = new AsyncCallbackJsonWebHandler(
      "/api/setup", [](AsyncWebServerRequest* r, JsonVariant& j) {
        if (!g_setupMode) { sendErr(r, 404, "not_found", "not in setup mode"); return; }
        applySettings(r, j, true, true);      // auth-exempt: no PIN exists yet
      });
  supH->setMethod(HTTP_POST);
  server.addHandler(supH);

  if (setupMode) {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
      r->send_P(200, "text/html", SETUP_HTML);
    });
    // Captive-portal OS probes -> bounce to the form.
    auto probe = [](AsyncWebServerRequest* r) { r->redirect("/"); };
    server.on("/generate_204", HTTP_GET, probe);
    server.on("/gen_204", HTTP_GET, probe);
    server.on("/hotspot-detect.html", HTTP_GET, probe);
    server.on("/connecttest.txt", HTTP_GET, probe);
    server.on("/ncsi.txt", HTTP_GET, probe);
  } else if (fsOk) {
    // Registered last: serveStatic("/") matches every path, so every /api route above wins.
    // Auth-exempt on purpose — the shell is how the user types the PIN in the first place,
    // and it contains nothing but the same HTML/JS anyone can read in this repo.
    server.serveStatic("/", LittleFS, "/")
        .setDefaultFile("index.html")
        .setCacheControl("no-cache");        // small files, re-validated; never a stale app.js
  }

  server.onNotFound([](AsyncWebServerRequest* r) {
    if (r->method() == HTTP_OPTIONS) { r->send(204); return; }   // preflight, always exempt
    if (g_setupMode && r->method() == HTTP_GET && !r->url().startsWith("/api/")) { r->redirect("/"); return; }
    sendErr(r, 404, "not_found", "unknown path");
  });

  server.begin();
  Serial.printf("[net] http on :%d%s\n", HTTP_PORT, setupMode ? " (setup mode)" : "");
}

extern "C" void netTask(void* pv) {
  uint32_t lastStatus = 0, lastNtp = millis();
  for (;;) {
    wsDrain();
    ws.cleanupClients();

    uint32_t now = millis();
    if (now - lastStatus >= STATUS_PUSH_MS) {
      lastStatus = now;
      if (ws.count()) wsBroadcast("status", statusJson());
      wsDrain();
    }
    if (now - lastNtp >= NTP_RESYNC_MS && WiFi.status() == WL_CONNECTED) {
      lastNtp = now;
      configTime(settings.tzOffsetMin * 60, 0, NTP_SERVER);   // T6: clock drift < 2 s/day
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
