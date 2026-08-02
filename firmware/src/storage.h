#pragma once
// storage.h — NVS settings. Owner: A0. Callable from any core/task.
// Secrets live here and ONLY here; they are never logged or compiled in.
#include <Arduino.h>

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
