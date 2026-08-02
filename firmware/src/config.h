#pragma once
// =====================================================================
//  config.h — THE ONLY PLACE HARDWARE IS DEFINED.
//
//  !! UNCONFIRMED DEFAULTS !!
//  The physical board has NOT been verified by the user yet. Everything
//  below is the assumed build target:
//
//    Board   : Seeed XIAO ESP32S3 Sense (ESP32-S3R8, 8MB PSRAM)
//    Camera  : OV2640 DVP (on the Sense expansion board)
//    Mic     : PDM MEMS (on the Sense expansion board)
//    SD      : onboard slot, SDMMC 1-bit
//    Display : ST7789 240x240 SPI TFT (external, wired to free pads)
//    Touch   : one ESP32-S3 native touch GPIO
//
//  If the real board differs, CHANGE ONLY THIS FILE. No pin, size,
//  interval, or resolution may appear anywhere else in the project.
// =====================================================================

// ---------- Camera: OV2640 DVP, XIAO ESP32S3 Sense pinout ----------
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   10
#define CAM_PIN_SIOD   40   // SDA
#define CAM_PIN_SIOC   39   // SCL
#define CAM_PIN_D7     48   // Y9
#define CAM_PIN_D6     11   // Y8
#define CAM_PIN_D5     12   // Y7
#define CAM_PIN_D4     14   // Y6
#define CAM_PIN_D3     16   // Y5
#define CAM_PIN_D2     18   // Y4
#define CAM_PIN_D1     17   // Y3
#define CAM_PIN_D0     15   // Y2
#define CAM_PIN_VSYNC  38
#define CAM_PIN_HREF   47
#define CAM_PIN_PCLK   13
#define CAM_XCLK_HZ    20000000

// ---------- SD card: onboard slot, SDMMC 1-bit ----------
#define SD_PIN_CLK     7
#define SD_PIN_CMD     9
#define SD_PIN_D0      8
#define SD_MOUNT_POINT "/sdcard"

// ---------- PDM microphone (Sense board) ----------
#define MIC_PIN_CLK    42
#define MIC_PIN_DATA   41
#define MIC_SAMPLE_RATE 16000
#define MIC_I2S_PORT   0

// ---------- Display: ST7789 240x240 on the free XIAO pads ----------
// D1..D5 pads. D8/D9/D10 (GPIO 7/8/9) are taken by the SD slot.
#define TFT_PIN_SCLK   2    // pad D1
#define TFT_PIN_MOSI   3    // pad D2
#define TFT_PIN_DC     4    // pad D3
#define TFT_PIN_CS     5    // pad D4
#define TFT_PIN_RST    6    // pad D5
#define TFT_PIN_BL     -1   // backlight tied high
#define TFT_WIDTH_PX   240
#define TFT_HEIGHT_PX  240
#define TFT_SPI_HZ     40000000
#define TFT_ROTATION   0

// ---------- Touch: one native touch GPIO ----------
#define TOUCH_PIN      1    // pad D0 == TOUCH1
#define TOUCH_THRESHOLD 40000   // legacy absolute cut. UNUSED by display.cpp — the
                                // baseline drifts with temperature/humidity, so the UI
                                // triggers on a relative rise instead (below).
// A4: capacitive calibration knobs. Tune these on the real board, nothing else.
#define TOUCH_DELTA_PCT      8.0f   // % rise of touchRead() over baseline = pressed
                                    //   too twitchy -> raise; misses taps -> lower
#define TOUCH_BASELINE_TAU_MS 5000  // baseline EMA time constant while released
                                    //   raise if slow finger-rests get absorbed
#define TOUCH_HOLD_MS  2000     // hold this long -> force status screen
#define TOUCH_DEBOUNCE_MS 250

// ---------- Video defaults (user-overridable via /api/settings) ----------
#define VIDEO_WIDTH    800      // SVGA
#define VIDEO_HEIGHT   600
#define VIDEO_FPS      10
#define VIDEO_QUALITY  12       // esp32-camera JPEG quality, lower = better

// ---------- PSRAM ring buffer ----------
#define RING_FRAME_COUNT   24           // camTask -> sdTask / netTask
#define RING_PSRAM_BUDGET  (4u*1024*1024)  // hard cap on ring allocation
#define CAM_FB_COUNT       2

// ---------- Recording ----------
#define SEGMENT_SECONDS    300          // 5 min per .avi
#define REC_DIR            "/rec"       // /rec/YYYYMMDD/HHMMSS.avi
#define SD_FREE_MIN_MB     512          // below this: delete oldest day
#define SD_USAGE_REFRESH_MS 60000       // usedBytes() walks the FAT — don't ask often
#define SD_DELETE_BATCH    16           // files purged per sdTask pass (keeps gaps small)
#define REC_UPLOAD_QUEUE_LEN 16         // closed segments waiting for uploadTask
#define STREAM_FPS         5            // MJPEG live stream
#define STREAM_MAX_CLIENTS 1

// ---------- Intervals (ms) ----------
#define STATUS_PUSH_MS     5000
#define NTP_RESYNC_MS      3600000
#define DISPLAY_REDRAW_MS  1000         // 1 Hz
#define TOUCH_POLL_MS      50           // 20 Hz
#define UPLOAD_POLL_MS     10000
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define BOOT_STATUS_SCREEN_MS   30000   // URL on screen for 30 s at boot

// ---------- Watchdog ----------
#define WDT_TIMEOUT_S      10           // camTask + sdTask stall -> esp_restart()

// ---------- Task stacks / priorities / cores (HANDOVER section 4) ----------
#define CAM_TASK_STACK     4096
#define SD_TASK_STACK      8192
#define NET_TASK_STACK     8192
#define UPLOAD_TASK_STACK  12288        // TLS needs the same room aiTask gets
#define AI_TASK_STACK      12288         // TLS needs room
#define UI_TASK_STACK      4096

#define CAM_TASK_PRIO      5
#define SD_TASK_PRIO       4
#define NET_TASK_PRIO      3
#define UI_TASK_PRIO       3
#define UPLOAD_TASK_PRIO   2
#define AI_TASK_PRIO       2

#define CORE_REALTIME      0            // PRO_CPU: camTask, sdTask
#define CORE_SERVICES      1            // APP_CPU: net, upload, ai, ui

// ---------- Network / identity ----------
#define SETUP_AP_SSID      "deskbuddy-setup"
#define HTTP_PORT          80
#define NTP_SERVER         "pool.ntp.org"
#define MDNS_HOST          "deskbuddy"

// ---------- Feature flags ----------
#define ENABLE_AUDIO   0    // companion .wav per segment (v1: off until mic verified)
#define ENABLE_UPLOAD  1    // Google Drive resumable upload
#define ENABLE_AI      1    // OpenRouter / Gemini

// =====================================================================
//  A2 (core-1 services) tunables — appended, nothing above was changed.
//  webserver.cpp / uploader.cpp / ai.cpp read ONLY from here.
// =====================================================================

// ---------- Firmware identity (reported by /api/status) ----------
#define FW_VERSION     "1.0.0"
#define DEVICE_NAME    "deskbuddy"

// ---------- Google Drive uploader ----------
#define DRIVE_ROOT_FOLDER      "DeskBuddy"   // DeskBuddy/YYYYMMDD/<segment>.avi
// Drive requires resumable chunks to be a multiple of 256 KiB (except the last one).
#define UPLOAD_CHUNK_BYTES     (256u * 1024u)
#define UPLOAD_MIN_HEAP        60000         // below this free heap: uploader stands down
#define UPLOAD_MAX_SD_LOAD_PCT 50            // recorderWriteLoadPct() above this: uploader stands
                                             // down. HANDOVER 4 "recording always wins" — this is
                                             // the knob that enforces it. Raise if uploads starve
                                             // on a healthy card; lower if T3.4 shows fps dipping.
#define UPLOAD_BACKOFF_BASE_MS 5000          // 5,10,20,40,80,160,300(cap) seconds
#define UPLOAD_BACKOFF_MAX_MS  300000
#define UPLOAD_HTTP_TIMEOUT_MS 20000
#define UPLOAD_PROGRESS_MS     5000          // ws upload_event progress: >=5 s AND >=10 %
#define UPLOAD_PROGRESS_PCT    10
// "uploaded" is recorder.h's ".pending" sidecar, deleted via recMarkUploaded() only after
// Drive confirms. No marker file of our own — one source of truth survives a power cut.

// ---------- AI ----------
#define AI_PROMPT_MAX          2000          // docs/API.md 3.4
#define AI_TIMEOUT_MS          20000         // T5: answer < 20 s
#define AI_ANSWER_MAX          4000          // truncate provider answer; guards heap
#define OPENROUTER_URL         "https://openrouter.ai/api/v1/chat/completions"
#define OPENROUTER_MODEL       "openai/gpt-4o-mini"
#define GEMINI_HOST            "https://generativelanguage.googleapis.com"
#define GEMINI_MODEL           "gemini-2.0-flash"

// ---------- Web server ----------
#define WS_QUEUE_DEPTH         16            // netTask is the ONLY thread that touches AsyncWebSocket
#define RECORDINGS_LIMIT_DEF   200           // docs/API.md 3.5 default `limit`
