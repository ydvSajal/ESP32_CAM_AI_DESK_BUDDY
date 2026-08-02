// display.cpp — Owner: A4. uiTask on CORE_SERVICES (core 1), prio UI_TASK_PRIO.
// Screens per HANDOVER.md section 7, touch per section 7 + config.h timings.
//
// NOT COMPILE-VERIFIED AGAINST HARDWARE (no board, no toolchain in this
// environment). The pure logic in display.h is covered by native unit tests;
// everything below is careful-but-unproven glue. T6 in TEST_PLAN.md is the check.
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <time.h>

#include "display.h"
#include "config.h"
#include "camera.h"
#include "recorder.h"
#include "uploader.h"
#include "storage.h"

// ---------------------------------------------------------------- panel
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
public:
  LGFX() {
    { auto c = _bus.config();
      c.spi_host    = SPI2_HOST;
      c.spi_mode    = 0;
      c.freq_write  = TFT_SPI_HZ;
      c.pin_sclk    = TFT_PIN_SCLK;
      c.pin_mosi    = TFT_PIN_MOSI;
      c.pin_miso    = -1;
      c.pin_dc      = TFT_PIN_DC;
      c.dma_channel = SPI_DMA_CH_AUTO;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs        = TFT_PIN_CS;
      c.pin_rst       = TFT_PIN_RST;
      c.pin_busy      = -1;
      c.panel_width   = TFT_WIDTH_PX;
      c.panel_height  = TFT_HEIGHT_PX;
      c.memory_width  = TFT_WIDTH_PX;
      c.memory_height = TFT_HEIGHT_PX;
      c.offset_x = 0; c.offset_y = 0;
      c.readable = false;
      c.invert   = true;   // CALIBRATION KNOB: most 240x240 ST7789 modules need this
      c.rgb_order = false; // CALIBRATION KNOB: flip if red/blue are swapped
      c.bus_shared = false;
      _panel.config(c); }
    setPanel(&_panel);
  }
};

static LGFX lcd;

#define BG      TFT_BLACK
#define FG      TFT_WHITE
#define DIM     TFT_DARKGREY
#define ACCENT  TFT_CYAN

// ---------------------------------------------------------------- shared state
static volatile uint32_t g_forceUntil = 0;      // millis() deadline for the boot status screen
static volatile bool     g_forcePending = false;

// AI text is pushed in by aiTask (ai.h has no getter — see A4 report).
// ponytail: fixed buffers + spinlock, copied in/out; a queue only pays off if AI
// starts streaming token-by-token to the panel, which v1 does not.
static portMUX_TYPE g_aiLock = portMUX_INITIALIZER_UNLOCKED;
static const size_t AI_Q_MAX = 96;
static const size_t AI_A_MAX = 320;
static char g_aiQ[AI_Q_MAX] = {0};
static char g_aiA[AI_A_MAX] = {0};

void displayForceStatus(uint32_t ms) {
  g_forceUntil   = millis() + ms;
  g_forcePending = true;
}

void displaySetAiText(const String& question, const String& answer) {
  char q[AI_Q_MAX], a[AI_A_MAX];
  snprintf(q, sizeof(q), "%s", question.c_str());   // String work OUTSIDE the lock
  snprintf(a, sizeof(a), "%s", answer.c_str());
  taskENTER_CRITICAL(&g_aiLock);
  memcpy(g_aiQ, q, sizeof(q));
  memcpy(g_aiA, a, sizeof(a));
  taskEXIT_CRITICAL(&g_aiLock);
}

// ---------------------------------------------------------------- dirty-field draw
// Redraw only what changed. setTextPadding erases exactly the old glyph box in
// the background colour, so no full-screen clear and no flicker.
static const int  CACHE_SLOTS = 14;
static const int  CACHE_LEN   = 64;
static char g_cache[CACHE_SLOTS][CACHE_LEN];

static void cacheClear() { memset(g_cache, 0, sizeof(g_cache)); }

static void field(int slot, const char* val, int x, int y, int w, int size,
                  uint16_t color, textdatum_t datum = textdatum_t::top_left,
                  const lgfx::IFont* font = &fonts::Font0) {
  if (slot < 0 || slot >= CACHE_SLOTS) return;
  if (!strncmp(g_cache[slot], val, CACHE_LEN - 1)) return;
  snprintf(g_cache[slot], CACHE_LEN, "%s", val);
  lcd.setFont(font);
  lcd.setTextDatum(datum);
  lcd.setTextSize(size);
  lcd.setTextColor(color, BG);
  lcd.setTextPadding(w);
  lcd.drawString(val, x, y);
  lcd.setTextPadding(0);
}

// ---------------------------------------------------------------- little widgets
// Wi-Fi bars, top right. Redrawn only when the level changes.
static void drawWifi(int level) {   // -1 = down, 0..4
  static int last = -2;
  if (level == last) return;
  last = level;
  lcd.fillRect(200, 4, 36, 18, BG);
  if (level < 0) {
    lcd.setFont(&fonts::Font0); lcd.setTextSize(2); lcd.setTextColor(TFT_RED, BG);
    lcd.setTextDatum(textdatum_t::top_left);
    lcd.drawString("x", 214, 4);
    return;
  }
  for (int i = 0; i < 4; i++) {
    const int h = 4 + i * 4;
    lcd.fillRect(202 + i * 8, 22 - h, 6, h, i < level ? ACCENT : DIM);
  }
}

static void drawRecDot(bool rec) {
  static int last = -1;
  if ((int)rec == last) return;
  last = rec;
  lcd.fillRect(4, 4, 46, 18, BG);
  if (!rec) return;
  lcd.fillCircle(11, 13, 6, TFT_RED);
  lcd.setFont(&fonts::Font0); lcd.setTextSize(1); lcd.setTextColor(TFT_RED, BG);
  lcd.setTextDatum(textdatum_t::middle_left);
  lcd.drawString("REC", 21, 13);
}

static int wifiLevel() {
  if (WiFi.status() != WL_CONNECTED) return -1;
  const int r = WiFi.RSSI();
  if (r >= -55) return 4;
  if (r >= -65) return 3;
  if (r >= -75) return 2;
  return 1;
}

// ---------------------------------------------------------------- screens
static void screenClock() {
  char buf[CACHE_LEN];
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  const bool haveTime = (tmv.tm_year + 1900) >= 2020;   // NTP not in yet -> dashes, never garbage

  if (haveTime) strftime(buf, sizeof(buf), "%H:%M", &tmv); else snprintf(buf, sizeof(buf), "--:--");
  field(0, buf, 120, 104, 200, 1, FG, textdatum_t::middle_center, &fonts::Font7);

  if (haveTime) strftime(buf, sizeof(buf), "%a %d %b %Y", &tmv); else snprintf(buf, sizeof(buf), "no time yet");
  field(1, buf, 120, 168, 236, 2, DIM, textdatum_t::middle_center);
}

static void screenStatus() {
  char buf[CACHE_LEN];
  const bool up = (WiFi.status() == WL_CONNECTED);

  field(0, "STATUS", 120, 34, 236, 2, ACCENT, textdatum_t::middle_center);

  snprintf(buf, sizeof(buf), up ? "http://%s" : "%s", up ? WiFi.localIP().toString().c_str() : "no wifi");
  field(1, buf, 8, 66, 224, 2, FG);

  snprintf(buf, sizeof(buf), "http://%s.local", MDNS_HOST);
  field(2, up ? buf : "-", 8, 90, 224, 1, DIM);

  snprintf(buf, sizeof(buf), "SSID %s", up ? WiFi.SSID().c_str() : "-");
  field(3, buf, 8, 112, 224, 1, DIM);

  uint64_t tot = 0, used = 0, freeMb = 0;
  if (sdMounted()) recorderUsage(&tot, &used, &freeMb);
  const int pct = pctUsed(used, tot);
  if (pct < 0) snprintf(buf, sizeof(buf), "SD  --");
  else         snprintf(buf, sizeof(buf), "SD  %d%% used", pct);
  field(4, buf, 8, 140, 224, 2, FG);

  snprintf(buf, sizeof(buf), "Queue  %u", (unsigned)uploadQueueDepth());
  field(5, buf, 8, 168, 224, 2, FG);

  fmtUptime((uint32_t)(millis() / 1000u), buf, sizeof(buf));
  field(6, buf, 8, 200, 224, 1, DIM);
}

static void screenAi() {
  char q[AI_Q_MAX], a[AI_A_MAX];
  taskENTER_CRITICAL(&g_aiLock);          // copy, then draw with nothing held
  memcpy(q, g_aiQ, sizeof(q));
  memcpy(a, g_aiA, sizeof(a));
  taskEXIT_CRITICAL(&g_aiLock);

  field(0, "AI", 120, 20, 236, 2, ACCENT, textdatum_t::middle_center);

  if (!q[0] && !a[0]) {
    field(1, "no question yet", 120, 120, 236, 1, DIM, textdatum_t::middle_center);
    return;
  }

  // size 2 default font = 12 px wide glyphs -> 19 cols across 236 px.
  static const int COLS = 19, Q_LINES = 2, A_LINES = 7;
  char lines[Q_LINES > A_LINES ? Q_LINES : A_LINES][CACHE_LEN];

  int n = textWrap(q, COLS, Q_LINES, &lines[0][0], CACHE_LEN);
  for (int i = 0; i < Q_LINES; i++)
    field(1 + i, i < n ? lines[i] : "", 8, 46 + i * 18, 228, 1, ACCENT);

  n = textWrap(a, COLS, A_LINES, &lines[0][0], CACHE_LEN);
  for (int i = 0; i < A_LINES; i++)
    field(3 + i, i < n ? lines[i] : "", 8, 92 + i * 18, 228, 1, FG);
}

static void screenCam() {
  char buf[CACHE_LEN];
  field(0, "CAM", 120, 20, 236, 2, ACCENT, textdatum_t::middle_center);

  snprintf(buf, sizeof(buf), "Today  %u seg", (unsigned)recorderSegmentsToday());
  field(1, buf, 8, 70, 224, 2, FG);

  snprintf(buf, sizeof(buf), "FPS    %.1f", cameraReady() ? cameraFps() : 0.0f);
  field(2, buf, 8, 106, 224, 2, FG);

  const uint32_t up = uploadLastOk();
  if (!up) snprintf(buf, sizeof(buf), "Upload never");
  else {
    time_t t = (time_t)up;
    struct tm tmv; localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "Upload %H:%M:%S", &tmv);
  }
  field(3, buf, 8, 142, 224, 2, FG);

  field(4, sdMounted() ? (recordingActive() ? "recording" : "idle") : "no SD card",
        8, 180, 224, 1, DIM);
}

// ---------------------------------------------------------------- task
extern "C" void uiTask(void* pv) {
  (void)pv;
  lcd.init();
  lcd.setRotation(TFT_ROTATION);
  lcd.fillScreen(BG);
#if TFT_PIN_BL >= 0
  pinMode(TFT_PIN_BL, OUTPUT);
  digitalWrite(TFT_PIN_BL, HIGH);
#endif

  TouchSm  touch; touchReset(touch);
  const TouchCfg tcfg = touchMakeCfg(TOUCH_DELTA_PCT, TOUCH_BASELINE_TAU_MS,
                                     TOUCH_POLL_MS, TOUCH_HOLD_MS, TOUCH_DEBOUNCE_MS);

  int      screen     = SCREEN_CLOCK;
  bool     needClear  = true;
  uint32_t lastDraw   = 0;

  for (;;) {
    const uint32_t now = millis();

    // ---- touch ----
    switch (touchFeed(touch, tcfg, now, touchRead(TOUCH_PIN))) {
      case TOUCH_TAP:
        g_forcePending = false;                 // a deliberate touch beats the boot screen
        screen = screenNext(screen, SCREEN_COUNT);
        needClear = true;
        break;
      case TOUCH_HOLD:
        g_forcePending = false;
        if (screen != SCREEN_STATUS) { screen = SCREEN_STATUS; needClear = true; }
        break;
      default: break;
    }

    // ---- boot status screen: URL visible for BOOT_STATUS_SCREEN_MS at every boot ----
    if (g_forcePending) {
      if ((int32_t)(now - g_forceUntil) < 0) {
        if (screen != SCREEN_STATUS) { screen = SCREEN_STATUS; needClear = true; }
      } else {
        g_forcePending = false;
        screen = SCREEN_CLOCK;
        needClear = true;
      }
    }

    // ---- redraw ----
    if (needClear || (uint32_t)(now - lastDraw) >= DISPLAY_REDRAW_MS) {
      if (needClear) { lcd.fillScreen(BG); cacheClear(); needClear = false; }
      switch (screen) {
        case SCREEN_STATUS: screenStatus(); break;
        case SCREEN_AI:     screenAi();     break;
        case SCREEN_CAM:    screenCam();    break;
        default:            screenClock();  break;
      }
      drawWifi(wifiLevel());
      drawRecDot(recordingActive());
      lastDraw = now;
    }

    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
  }
}
