// uploader.cpp — A2. uploadTask on CORE_SERVICES prio 2.
// Drains recorder.h's recSegmentQueue into Google Drive with the v3 resumable protocol.
// Best-effort by construction: it never holds an SD handle across a network wait, never
// allocates more than one chunk, and stands down whenever heap or Wi-Fi say so.
#include "uploader.h"
#include "config.h"
#include "storage.h"
#include "recorder.h"
#include "webserver.h"
#include "ai.h"          // redactSecret() — T8

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#if ENABLE_UPLOAD

// ---------------------------------------------------------------- state

static char     g_current[REC_PATH_MAX] = "";
static uint8_t  g_pct = 0;
static uint32_t g_lastOk = 0;
static String   g_lastError;
static uint32_t g_backoffUntil = 0;
static uint32_t g_attempt = 0;

static String   g_accessToken;
static uint32_t g_tokenExpiryMs = 0;      // millis() deadline

static String   g_folderDay;              // cache: one day, one id — we only ever write today
static String   g_folderId;
static String   g_rootFolderId;

static uint8_t* g_chunk = nullptr;        // single PSRAM chunk buffer, allocated once

uint32_t uploadQueueDepth() {
  uint32_t q = recSegmentQueue ? uxQueueMessagesWaiting(recSegmentQueue) : 0;
  return q + (g_current[0] ? 1 : 0);
}
const char* uploadCurrentFile() { return g_current; }
uint8_t uploadProgressPct() { return g_pct; }
uint32_t uploadLastOk() { return g_lastOk; }
const char* uploadLastError() { return g_lastError.c_str(); }

// T8: every string that can reach a log, /api/status or /ws goes through here first.
static String safe(const String& s) {
  std::string t = s.c_str();
  t = redactSecret(t, settings.driveRefreshToken.c_str());
  t = redactSecret(t, settings.driveClientSecret.c_str());
  t = redactSecret(t, g_accessToken.c_str());
  return String(t.c_str());
}

static void event(const char* ev, const char* file, uint8_t pct, const char* driveId, const char* err) {
  JsonDocument p;
  p["event"] = ev;
  p["file"] = file;
  p["progress_pct"] = pct;
  p["queued"] = uploadQueueDepth();
  if (driveId) p["drive_file_id"] = driveId; else p["drive_file_id"] = nullptr;
  if (err) p["error"] = err; else p["error"] = nullptr;
  String s;
  serializeJson(p, s);
  wsBroadcast("upload_event", s);
}

static void fail(const char* file, const String& why) {
  g_lastError = safe(why);
  event("failed", file, g_pct, nullptr, g_lastError.c_str());
  g_backoffUntil = millis() + backoffMs(g_attempt++, UPLOAD_BACKOFF_BASE_MS, UPLOAD_BACKOFF_MAX_MS);
  Serial.printf("[up] %s: %s (retry in %lus)\n", file, g_lastError.c_str(),
                (unsigned long)((g_backoffUntil - millis()) / 1000));
}

// ---------------------------------------------------------------- http plumbing

// ponytail: setInsecure() — no CA bundle, no pinning. The threat here is a LAN MITM against
// a hobby device whose only secret in flight is an access token. Ceiling: ship the GTS root
// and call setCACert() if this ever leaves a home network.
static void newClient(WiFiClientSecure& c) {
  c.setInsecure();
  c.setTimeout(UPLOAD_HTTP_TIMEOUT_MS / 1000);
}

static bool refreshAccessToken() {
  if (settings.driveRefreshToken.isEmpty() || settings.driveClientId.isEmpty() ||
      settings.driveClientSecret.isEmpty())
    return false;

  WiFiClientSecure c;
  newClient(c);
  HTTPClient http;
  http.setTimeout(UPLOAD_HTTP_TIMEOUT_MS);
  if (!http.begin(c, "https://oauth2.googleapis.com/token")) return false;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "grant_type=refresh_token&client_id=" + settings.driveClientId +
                "&client_secret=" + settings.driveClientSecret +
                "&refresh_token=" + settings.driveRefreshToken;
  int code = http.POST(body);
  String resp = (code > 0) ? http.getString() : String("");
  http.end();
  body = "";                                   // do not leave creds sitting in the heap

  if (code != 200) {
    // NEVER quote the response: a Google error body can echo the client_id/secret.
    g_lastError = String("drive auth failed (") + code + ")";
    return false;
  }
  JsonDocument d;
  if (deserializeJson(d, resp)) { g_lastError = "drive auth: bad json"; return false; }
  g_accessToken = d["access_token"].as<const char*>() ? d["access_token"].as<const char*>() : "";
  uint32_t ttl = d["expires_in"] | 3600;
  g_tokenExpiryMs = millis() + (ttl > 120 ? (ttl - 60) * 1000 : 60000);   // refresh a minute early
  return g_accessToken.length() > 0;
}

static bool ensureToken() {
  if (g_accessToken.length() && (int32_t)(g_tokenExpiryMs - millis()) > 0) return true;
  return refreshAccessToken();
}

static void authHeader(HTTPClient& http) {
  http.addHeader("Authorization", "Bearer " + g_accessToken);
}

// Find a folder by name under `parent` ("root" for the drive root). "" when absent.
static String findFolder(const String& name, const String& parent) {
  WiFiClientSecure c;
  newClient(c);
  HTTPClient http;
  http.setTimeout(UPLOAD_HTTP_TIMEOUT_MS);
  String q = "name='" + name + "' and '" + parent +
             "' in parents and mimeType='application/vnd.google-apps.folder' and trashed=false";
  q.replace(" ", "%20");
  q.replace("'", "%27");
  String url = "https://www.googleapis.com/drive/v3/files?q=" + q + "&fields=files(id)&pageSize=1";
  if (!http.begin(c, url)) return "";
  authHeader(http);
  int code = http.GET();
  String resp = (code == 200) ? http.getString() : String("");
  http.end();
  if (code != 200) return "";
  JsonDocument d;
  if (deserializeJson(d, resp)) return "";
  JsonArray f = d["files"];
  if (f.isNull() || f.size() == 0) return "";
  const char* id = f[0]["id"];
  return id ? String(id) : String("");
}

static String createFolder(const String& name, const String& parent) {
  WiFiClientSecure c;
  newClient(c);
  HTTPClient http;
  http.setTimeout(UPLOAD_HTTP_TIMEOUT_MS);
  if (!http.begin(c, "https://www.googleapis.com/drive/v3/files?fields=id")) return "";
  authHeader(http);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"name\":\"" + name + "\",\"mimeType\":\"application/vnd.google-apps.folder\"," +
                "\"parents\":[\"" + parent + "\"]}";
  int code = http.POST(body);
  String resp = (code == 200 || code == 201) ? http.getString() : String("");
  http.end();
  if (resp.isEmpty()) return "";
  JsonDocument d;
  if (deserializeJson(d, resp)) return "";
  const char* id = d["id"];
  return id ? String(id) : String("");
}

static String ensureFolder(const String& name, const String& parent) {
  String id = findFolder(name, parent);
  if (id.isEmpty()) id = createFolder(name, parent);
  return id;
}

// DeskBuddy/YYYYMMDD, created on demand, ids cached (root forever, day until it rolls over).
static String folderForDay(const String& day) {
  if (day == g_folderDay && g_folderId.length()) return g_folderId;
  if (g_rootFolderId.isEmpty()) {
    String parent = settings.driveFolderId.length() ? settings.driveFolderId : String("root");
    g_rootFolderId = ensureFolder(DRIVE_ROOT_FOLDER, parent);
    if (g_rootFolderId.isEmpty()) return "";
  }
  String id = ensureFolder(day, g_rootFolderId);
  if (id.length()) { g_folderDay = day; g_folderId = id; }
  return id;
}

// ---------------------------------------------------------------- resumable upload

// POST the metadata, get back the session URI in the Location header.
static String startSession(const String& folderId, const String& name, uint64_t total) {
  WiFiClientSecure c;
  newClient(c);
  HTTPClient http;
  http.setTimeout(UPLOAD_HTTP_TIMEOUT_MS);
  const char* keys[] = {"Location"};
  if (!http.begin(c, "https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable&fields=id"))
    return "";
  http.collectHeaders(keys, 1);
  authHeader(http);
  http.addHeader("Content-Type", "application/json; charset=UTF-8");
  http.addHeader("X-Upload-Content-Type", "video/x-msvideo");
  http.addHeader("X-Upload-Content-Length", String((unsigned long long)total));
  String body = "{\"name\":\"" + name + "\",\"parents\":[\"" + folderId + "\"]}";
  int code = http.POST(body);
  String loc = http.header("Location");
  http.end();
  if (code != 200 && code != 201) return "";
  return loc;
}

// Send one chunk. Returns the HTTP code; `nextOff` is updated on 308, `driveId` on 200/201.
static int putChunk(const String& session, uint8_t* data, uint32_t len, uint64_t off,
                    uint64_t total, uint64_t* nextOff, String* driveId) {
  WiFiClientSecure c;
  newClient(c);
  HTTPClient http;
  http.setTimeout(UPLOAD_HTTP_TIMEOUT_MS);
  const char* keys[] = {"Range"};
  if (!http.begin(c, session)) return -1;
  http.collectHeaders(keys, 1);
  http.addHeader("Content-Type", "video/x-msvideo");
  http.addHeader("Content-Range", String(contentRange(off, len, total).c_str()));
  int code = http.sendRequest("PUT", data, len);
  if (code == 308) {
    String range = http.header("Range");
    *nextOff = range.length() ? nextOffsetFromRange(range.c_str()) : off + len;
  } else if (code == 200 || code == 201) {
    JsonDocument d;
    String resp = http.getString();
    if (!deserializeJson(d, resp) && d["id"].is<const char*>()) *driveId = d["id"].as<const char*>();
  }
  http.end();
  return code;
}

// Ask Drive how much of an interrupted session it already has (0 length PUT, bytes */total).
static bool querySession(const String& session, uint64_t total, uint64_t* off, bool* complete) {
  WiFiClientSecure c;
  newClient(c);
  HTTPClient http;
  http.setTimeout(UPLOAD_HTTP_TIMEOUT_MS);
  const char* keys[] = {"Range"};
  if (!http.begin(c, session)) return false;
  http.collectHeaders(keys, 1);
  http.addHeader("Content-Range", String("bytes */") + String((unsigned long long)total));
  int code = http.sendRequest("PUT", (uint8_t*)nullptr, 0);
  String range = http.header("Range");
  http.end();
  if (code == 200 || code == 201) { *complete = true; return true; }
  if (code != 308) return false;
  *off = range.length() ? nextOffsetFromRange(range.c_str()) : 0;
  return true;
}

// Recording ALWAYS wins: bail out of any long loop the moment the device looks stressed.
// recorderWriteLoadPct() is the one that matters — heap and Wi-Fi say the upload *can*
// run, SD load says whether it *may*. Checked inside the chunk loop too, so a card that
// falls behind mid-upload suspends the transfer at the next chunk boundary; the
// ".pending" sidecar means it simply resumes later.
static bool healthy() {
  return WiFi.status() == WL_CONNECTED && ESP.getFreeHeap() >= UPLOAD_MIN_HEAP &&
         sdMounted() && recorderWriteLoadPct() <= UPLOAD_MAX_SD_LOAD_PCT;
}

// Returns true when Drive has confirmed the whole file.
static bool uploadOne(const char* path) {
  if (recIsUploaded(path)) return true;              // reboot-safe: sidecar already gone

  File f = SD_MMC.open(path, FILE_READ);
  if (!f) { fail(path, "cannot open segment"); return false; }
  uint64_t total = f.size();
  if (total == 0) { f.close(); recMarkUploaded(path); return true; }   // nothing to send

  std::string day = driveDayFromPath(path);
  if (day.empty()) { f.close(); fail(path, "unexpected segment path"); return false; }
  String folder = folderForDay(String(day.c_str()));
  if (folder.isEmpty()) { f.close(); fail(path, "cannot create drive folder"); return false; }

  String name = String(driveFileName(path).c_str());
  strncpy(g_current, path, REC_PATH_MAX - 1);
  g_current[REC_PATH_MAX - 1] = '\0';
  g_pct = 0;
  event("started", path, 0, nullptr, nullptr);

  // ponytail: the session URI lives in RAM only. A mid-upload reboot restarts that one
  // segment from byte 0 (no duplicate — the ".pending" sidecar is still there). Wi-Fi drops
  // and provider hiccups DO resume, which is the case T3 actually exercises. Ceiling: persist
  // the URI next to the segment if restarts-during-upload ever become common.
  String session = startSession(folder, name, total);
  if (session.isEmpty()) { f.close(); fail(path, "cannot start resumable session"); return false; }

  uint64_t off = 0;
  String driveId;
  uint32_t lastEventMs = 0;
  uint8_t lastEventPct = 0;
  bool done = false;
  uint8_t authRetries = 0;

  while (off < total) {
    if (!healthy()) { f.close(); fail(path, "device busy or offline"); return false; }

    uint32_t len = (uint32_t)((total - off) < UPLOAD_CHUNK_BYTES ? (total - off) : UPLOAD_CHUNK_BYTES);
    if (!f.seek(off) || f.read(g_chunk, len) != len) {
      f.close(); fail(path, "sd read failed"); return false;
    }

    uint64_t next = off + len;
    int code = putChunk(session, g_chunk, len, off, total, &next, &driveId);

    if (code == 308) {
      off = next;
    } else if (code == 200 || code == 201) {
      off = total; done = true;
    } else if (code == 401 && authRetries++ < 1) {
      if (!refreshAccessToken()) { f.close(); fail(path, "drive re-auth failed"); return false; }
      bool complete = false;
      querySession(session, total, &off, &complete);      // pick up where Drive stopped
      if (complete) { off = total; done = true; }
    } else if (code == 404 || code == 410) {
      f.close(); fail(path, "resumable session expired"); return false;
    } else {
      // Transient (5xx, TLS drop, -1). Ask Drive for its offset and retry on the next pass.
      f.close();
      fail(path, String("drive chunk failed (") + code + ")");
      return false;
    }

    g_pct = (uint8_t)((off * 100) / total);
    uint32_t now = millis();
    if (g_pct - lastEventPct >= UPLOAD_PROGRESS_PCT && now - lastEventMs >= UPLOAD_PROGRESS_MS) {
      lastEventPct = g_pct;
      lastEventMs = now;
      event("progress", path, g_pct, nullptr, nullptr);
    }
    vTaskDelay(pdMS_TO_TICKS(10));      // yield: core 1 has a display and a web server too
  }
  f.close();

  if (!done) { fail(path, "upload ended without drive confirmation"); return false; }

  recMarkUploaded(path);                // ONLY after Drive said 200/201. No duplicates.
  g_lastOk = (uint32_t)time(nullptr);
  g_lastError = "";
  g_attempt = 0;
  g_pct = 100;
  event("done", path, 100, driveId.c_str(), nullptr);
  Serial.printf("[up] done %s\n", path);
  return true;
}

// ---------------------------------------------------------------- task

extern "C" void uploadTask(void* pv) {
  RecSegmentMsg pending{};
  bool havePending = false;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(UPLOAD_POLL_MS));

    if (settings.driveRefreshToken.isEmpty()) continue;         // not configured: stay asleep
    if (!healthy()) continue;                                   // recording always wins
    if ((int32_t)(millis() - g_backoffUntil) < 0) continue;

    if (!havePending) {
      if (!recSegmentQueue) continue;
      if (xQueueReceive(recSegmentQueue, &pending, 0) != pdTRUE) continue;
      havePending = true;
      event("queued", pending.path, 0, nullptr, nullptr);
    }

    if (!g_chunk) {
      g_chunk = (uint8_t*)heap_caps_malloc(UPLOAD_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!g_chunk) { g_backoffUntil = millis() + UPLOAD_BACKOFF_BASE_MS; continue; }
    }
    if (!ensureToken()) {
      wsError("uploader", "drive_auth_failed", safe(g_lastError), false);
      g_backoffUntil = millis() + backoffMs(g_attempt++, UPLOAD_BACKOFF_BASE_MS, UPLOAD_BACKOFF_MAX_MS);
      continue;
    }

    bool ok = uploadOne(pending.path);
    g_current[0] = '\0';
    g_pct = 0;
    if (ok) havePending = false;      // failed items stay pending and retry after the backoff
  }
}

#else   // !ENABLE_UPLOAD

extern "C" void uploadTask(void* pv) { for (;;) vTaskDelay(pdMS_TO_TICKS(UPLOAD_POLL_MS)); }
uint32_t uploadQueueDepth() { return 0; }
const char* uploadCurrentFile() { return ""; }
uint8_t uploadProgressPct() { return 0; }
uint32_t uploadLastOk() { return 0; }
const char* uploadLastError() { return ""; }

#endif
