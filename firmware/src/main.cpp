// main.cpp — Owner: A0. Boot, NVS, Wi-Fi/NTP, task spawn. Nothing else lives here.
// Task/core/priority layout is HANDOVER.md section 4 and is not negotiable.
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>

#include "config.h"
#include "storage.h"
#include "camera.h"
#include "recorder.h"
#include "uploader.h"
#include "ai.h"
#include "webserver.h"
#include "display.h"

static TaskHandle_t hCam = nullptr, hSd = nullptr;

// USB-serial alternative to the SoftAP captive portal: the flashing web page (webflash/)
// can send one JSON line over the same port it just flashed through, no second Wi-Fi
// network to join. Markers are grep-able on purpose so a browser-side reader can parse
// them without guessing at prose.
static bool trySerialProvision() {
  Serial.println(">>>PROVISION_READY");
  Serial.setTimeout(SERIAL_PROVISION_WINDOW_MS);
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) { Serial.println(">>>PROVISION_TIMEOUT"); return false; }

  JsonDocument doc;
  DeserializationError je = deserializeJson(doc, line);
  if (je || !doc.is<JsonObject>()) {
    Serial.println(">>>PROVISION_ERR:invalid JSON");
    return false;
  }
  String saved, errMsg;
  bool rebootReq = false;
  if (!settingsApplyJson(doc.as<JsonObject>(), true, saved, rebootReq, errMsg)) {
    Serial.println(">>>PROVISION_ERR:" + errMsg);
    return false;
  }
  Serial.println(">>>PROVISION_OK");
  return true;
}

static void startSetupMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP_SSID);
  Serial.printf("[boot] setup mode: AP %s at %s\n", SETUP_AP_SSID, WiFi.softAPIP().toString().c_str());
  webserverBegin(true);
}

static bool startStationMode() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                  // sleep costs us stream latency, not worth the mA
  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[boot] wifi failed; running offline (recording still runs)");
    return false;
  }
  Serial.printf("[boot] wifi ok: http://%s\n", WiFi.localIP().toString().c_str());
  MDNS.begin(MDNS_HOST);
  configTime(settings.tzOffsetMin * 60, 0, NTP_SERVER);   // netTask re-syncs every NTP_RESYNC_MS
  webserverBegin(false);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] deskbuddy");

  settingsLoad();
  if (!settingsValid() && trySerialProvision()) {
    // saved to NVS, settingsValid() now true — fall straight into station mode below
  }
  if (!settingsValid()) startSetupMode();
  else                  startStationMode();

  // Before uiTask exists, so the first frame drawn is already the URL screen.
  displayForceStatus(BOOT_STATUS_SCREEN_MS);   // "URL on screen at every boot"

  // Core 0 (PRO_CPU) — realtime. Recording never yields to anything.
  xTaskCreatePinnedToCore(camTask, "cam", CAM_TASK_STACK, nullptr, CAM_TASK_PRIO, &hCam, CORE_REALTIME);
  xTaskCreatePinnedToCore(sdTask,  "sd",  SD_TASK_STACK,  nullptr, SD_TASK_PRIO,  &hSd,  CORE_REALTIME);

  // Core 1 (APP_CPU) — services. All best-effort.
  xTaskCreatePinnedToCore(netTask,    "net",    NET_TASK_STACK,    nullptr, NET_TASK_PRIO,    nullptr, CORE_SERVICES);
  xTaskCreatePinnedToCore(uploadTask, "upload", UPLOAD_TASK_STACK, nullptr, UPLOAD_TASK_PRIO, nullptr, CORE_SERVICES);
  xTaskCreatePinnedToCore(aiTask,     "ai",     AI_TASK_STACK,     nullptr, AI_TASK_PRIO,     nullptr, CORE_SERVICES);
  xTaskCreatePinnedToCore(uiTask,     "ui",     UI_TASK_STACK,     nullptr, UI_TASK_PRIO,     nullptr, CORE_SERVICES);

  // Watchdog on camTask + sdTask only (HANDOVER 4): stall > WDT_TIMEOUT_S -> panic -> restart.
  // Each of those tasks must call esp_task_wdt_reset() every loop.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdt = { .timeout_ms = WDT_TIMEOUT_S * 1000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_reconfigure(&wdt);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(hCam);
  esp_task_wdt_add(hSd);
}

void loop() { vTaskDelete(NULL); }   // everything runs in the six pinned tasks
