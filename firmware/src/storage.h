#pragma once
// storage.h — NVS settings. Owner: A0. Callable from any core/task.
// Secrets live here and ONLY here; they are never logged or compiled in.
#include <Arduino.h>
#include <ArduinoJson.h>

struct Settings {
  String wifiSsid;
  String wifiPass;
  String devicePin;           // empty = auth disabled
  String openrouterKey;
  String geminiKey;
  String driveRefreshToken;
  String driveClientId;
  String driveClientSecret;
  String driveFolderId;
  String aiProvider;          // "openrouter" | "gemini"
  uint16_t videoWidth;
  uint16_t videoHeight;
  uint8_t  videoFps;
  uint8_t  videoQuality;
  int16_t  tzOffsetMin;
};

// The one live copy. Loaded once in setup(), read by every module.
extern Settings settings;

void settingsLoad();            // NVS -> settings (config.h defaults if unset)
void settingsSave();            // settings -> NVS
bool settingsValid();           // true when wifiSsid is non-empty

// "sk-or-v1-abcdef123456" -> "********3456". Empty stays empty. Used by GET /api/settings.
String maskSecret(const String& s);

// Shared by /api/settings (webserver.cpp), /api/setup (webserver.cpp) and the USB serial
// provisioning line (main.cpp) — one validator, one field-copy, so all three entry points
// agree on what a valid settings payload looks like. Validates everything before writing
// anything; on success, calls settingsSave() itself.
//   o             — the settings object (same shape as docs/API.md 3.8/3.10 body)
//   requireSsid   — true for /api/setup and serial provisioning (first boot needs Wi-Fi)
//   savedCsv      — out: quoted,comma,list of field names actually written (may be empty)
//   rebootRequired— out: true if a changed field only takes effect after reboot
//   errMsg        — out: human-readable reason, set only when this returns false
bool settingsApplyJson(JsonObject o, bool requireSsid, String& savedCsv,
                        bool& rebootRequired, String& errMsg);
