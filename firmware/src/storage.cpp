#include "storage.h"
#include "config.h"
#include <Preferences.h>

Settings settings;

static Preferences prefs;
static const char* NS = "deskbuddy";

// ponytail: one open/close per load/save, no caching layer. NVS keys are <=15 chars (hard NVS limit).
void settingsLoad() {
  prefs.begin(NS, true);
  settings.wifiSsid          = prefs.getString("ssid", "");
  settings.wifiPass          = prefs.getString("pass", "");
  settings.devicePin         = prefs.getString("pin", "");
  settings.openrouterKey     = prefs.getString("orkey", "");
  settings.geminiKey         = prefs.getString("gemkey", "");
  settings.driveRefreshToken = prefs.getString("drvtok", "");
  settings.driveClientId     = prefs.getString("drvcid", "");
  settings.driveClientSecret = prefs.getString("drvsec", "");
  settings.driveFolderId     = prefs.getString("drvfld", "");
  settings.aiProvider        = prefs.getString("aiprov", "openrouter");
  settings.videoWidth        = prefs.getUShort("vidw", VIDEO_WIDTH);
  settings.videoHeight       = prefs.getUShort("vidh", VIDEO_HEIGHT);
  settings.videoFps          = prefs.getUChar("vidfps", VIDEO_FPS);
  settings.videoQuality      = prefs.getUChar("vidq", VIDEO_QUALITY);
  settings.tzOffsetMin       = prefs.getShort("tzmin", 0);
  prefs.end();
}

void settingsSave() {
  prefs.begin(NS, false);
  prefs.putString("ssid",   settings.wifiSsid);
  prefs.putString("pass",   settings.wifiPass);
  prefs.putString("pin",    settings.devicePin);
  prefs.putString("orkey",  settings.openrouterKey);
  prefs.putString("gemkey", settings.geminiKey);
  prefs.putString("drvtok", settings.driveRefreshToken);
  prefs.putString("drvcid", settings.driveClientId);
  prefs.putString("drvsec", settings.driveClientSecret);
  prefs.putString("drvfld", settings.driveFolderId);
  prefs.putString("aiprov", settings.aiProvider);
  prefs.putUShort("vidw",   settings.videoWidth);
  prefs.putUShort("vidh",   settings.videoHeight);
  prefs.putUChar("vidfps",  settings.videoFps);
  prefs.putUChar("vidq",    settings.videoQuality);
  prefs.putShort("tzmin",   settings.tzOffsetMin);
  prefs.end();
}

bool settingsValid() { return settings.wifiSsid.length() > 0; }

// docs/API.md 3.7. Empty stays empty. A secret of 8 characters or fewer is masked
// ENTIRELY — keeping "the last 4" of a 4-6 character PIN would hand over most of it.
// Longer secrets keep their last 4, everything before becomes '*', total capped at 12
// so the response never leaks the real length either.
String maskSecret(const String& s) {
  size_t n = s.length();
  if (n == 0) return "";
  size_t keep  = n > 8 ? 4 : 0;
  size_t stars = n - keep;
  if (stars > 8) stars = 8;
  String out;
  for (size_t i = 0; i < stars; i++) out += '*';
  if (keep) out += s.substring(n - keep);
  return out;
}

static bool videoGeometryOk(int w, int h) {
  return (w == 320 && h == 240) || (w == 640 && h == 480) || (w == 800 && h == 600) ||
         (w == 1024 && h == 768) || (w == 1600 && h == 1200);
}

// docs/API.md 3.8: partial update, validate everything first, write nothing on failure.
bool settingsApplyJson(JsonObject o, bool requireSsid, String& savedCsv,
                        bool& rebootRequired, String& errMsg) {
  if (requireSsid && !o["wifi_ssid"].is<const char*>()) { errMsg = "wifi_ssid required"; return false; }
#define BAD(m) { errMsg = m; return false; }
  if (o["wifi_ssid"].is<const char*>() && String(o["wifi_ssid"].as<const char*>()).length() > 32) BAD("wifi_ssid too long")
  if (o["wifi_pass"].is<const char*>() && String(o["wifi_pass"].as<const char*>()).length() > 63) BAD("wifi_pass too long")
  if (o["device_pin"].is<const char*>()) {
    size_t n = strlen(o["device_pin"].as<const char*>());
    if (n != 0 && (n < 4 || n > 12)) BAD("device_pin must be empty or 4-12 chars")
  }
  if (o["ai_provider"].is<const char*>()) {
    String p = o["ai_provider"].as<const char*>();
    if (p != "openrouter" && p != "gemini") BAD("ai_provider must be openrouter or gemini")
  }
  if (o["video"].is<JsonObject>()) {
    JsonObject v = o["video"];
    int w = v["width"] | (int)settings.videoWidth, h = v["height"] | (int)settings.videoHeight;
    int fps = v["fps"] | (int)settings.videoFps, q = v["quality"] | (int)settings.videoQuality;
    if (!videoGeometryOk(w, h)) BAD("unsupported video geometry")
    if (fps < 1 || fps > 20) BAD("video.fps must be 1-20")
    if (q < 10 || q > 63) BAD("video.quality must be 10-63")
  }
  if (o["tz_offset_min"].is<int>()) {
    int t = o["tz_offset_min"];
    if (t < -720 || t > 840) BAD("tz_offset_min out of range")
  }
#undef BAD

  String saved;
  rebootRequired = false;
#define TAKE(key, field) if (o[key].is<const char*>()) { \
    String nv = o[key].as<const char*>(); if (nv != field) { field = nv; } \
    saved += (saved.length() ? ",\"" : "\""); saved += key; saved += "\""; }

  TAKE("wifi_ssid", settings.wifiSsid)          if (o["wifi_ssid"].is<const char*>()) rebootRequired = true;
  TAKE("wifi_pass", settings.wifiPass)          if (o["wifi_pass"].is<const char*>()) rebootRequired = true;
  TAKE("device_pin", settings.devicePin)
  TAKE("openrouter_key", settings.openrouterKey)
  TAKE("gemini_key", settings.geminiKey)
  TAKE("drive_refresh_token", settings.driveRefreshToken)
  TAKE("drive_client_id", settings.driveClientId)
  TAKE("drive_client_secret", settings.driveClientSecret)
  TAKE("drive_folder_id", settings.driveFolderId)
  TAKE("ai_provider", settings.aiProvider)
#undef TAKE

  if (o["video"].is<JsonObject>()) {
    JsonObject v = o["video"];
    settings.videoWidth = v["width"] | settings.videoWidth;
    settings.videoHeight = v["height"] | settings.videoHeight;
    settings.videoFps = v["fps"] | settings.videoFps;
    settings.videoQuality = v["quality"] | settings.videoQuality;
    saved += (saved.length() ? ",\"video\"" : "\"video\"");
    rebootRequired = true;
  }
  if (o["tz_offset_min"].is<int>()) {
    settings.tzOffsetMin = o["tz_offset_min"].as<int>();
    saved += (saved.length() ? ",\"tz_offset_min\"" : "\"tz_offset_min\"");
  }

  settingsSave();
  savedCsv = saved;
  return true;
}
