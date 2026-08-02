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
