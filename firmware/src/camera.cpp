#include "camera.h"
#include "config.h"
#include "storage.h"

#include <esp_camera.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Ring buffer
//
// ponytail: fixed-size slots (budget / count) instead of a packed byte ring.
// Costs ~4 MB of the 8 MB PSRAM up front and wastes the difference between a
// slot and a real JPEG, buys zero allocation and zero fragmentation at runtime,
// which is what a 24/7 recorder actually needs. Ceiling: if a larger resolution
// makes 4 MB too tight, switch RING_FRAME_COUNT/RING_PSRAM_BUDGET in config.h
// before reaching for a packed ring.
// ---------------------------------------------------------------------------
#define SLOT_BYTES (RING_PSRAM_BUDGET / RING_FRAME_COUNT)

struct Slot {
  uint8_t* buf;
  size_t   len;
  uint32_t ts;      // epoch seconds (docs/API.md X-Timestamp)
  uint32_t id;      // 0 = never published; monotonic otherwise
  int16_t  refs;    // -1 = producer is filling it, >=0 = borrow count
};

static Slot              ring[RING_FRAME_COUNT];
static SemaphoreHandle_t mux = nullptr;
static uint32_t          nextId = 1;

static volatile bool     camOk = false;
static volatile uint32_t droppedFrames = 0;
static volatile uint32_t lastFrameMs = 0;
static volatile float    measuredFps = 0.0f;

// ponytail: 4-entry task->borrow table so the no-arg ringRelease() seam in the
// original header keeps working. Borrowers are sdTask + the async web task, so 4
// is generous. Ceiling: use ringReleaseId()/cameraReleaseJpeg() if a task ever
// needs two frames at once.
#define MAX_BORROWERS 4
static struct { TaskHandle_t task; uint32_t id; } borrowers[MAX_BORROWERS];

#define LOCK()   xSemaphoreTake(mux, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(mux)

static void rememberBorrow(uint32_t id) {          // caller holds the lock
  TaskHandle_t me = xTaskGetCurrentTaskHandle();
  for (int i = 0; i < MAX_BORROWERS; i++)
    if (borrowers[i].task == nullptr || borrowers[i].task == me) {
      borrowers[i].task = me; borrowers[i].id = id; return;
    }
}

static int slotOfId(uint32_t id) {                 // caller holds the lock
  if (!id) return -1;
  for (int i = 0; i < RING_FRAME_COUNT; i++) if (ring[i].id == id) return i;
  return -1;
}

static bool ringAlloc() {
  if (ring[0].buf) return true;
  for (int i = 0; i < RING_FRAME_COUNT; i++) {
    ring[i].buf = (uint8_t*)heap_caps_malloc(SLOT_BYTES, MALLOC_CAP_SPIRAM);
    if (!ring[i].buf) { Serial.printf("[cam] ring alloc failed at slot %d\n", i); return false; }
    ring[i].len = 0; ring[i].ts = 0; ring[i].id = 0; ring[i].refs = 0;
  }
  if (!mux) mux = xSemaphoreCreateMutex();
  return mux != nullptr;
}

// ---------------------------------------------------------------------------
// Camera init
// ---------------------------------------------------------------------------
static framesize_t framesizeFor(uint16_t w, uint16_t h) {
  if (w <= 320)  return FRAMESIZE_QVGA;
  if (w <= 640)  return FRAMESIZE_VGA;
  if (w <= 800)  return FRAMESIZE_SVGA;
  if (w <= 1024) return FRAMESIZE_XGA;
  (void)h;       return FRAMESIZE_UXGA;
}

bool cameraInit() {
  if (!ringAlloc()) return false;

  esp_camera_deinit();                 // safe on first call; makes retry a one-liner

  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_pwdn = CAM_PIN_PWDN;  c.pin_reset = CAM_PIN_RESET;
  c.pin_xclk = CAM_PIN_XCLK;
  c.pin_sccb_sda = CAM_PIN_SIOD; c.pin_sccb_scl = CAM_PIN_SIOC;
  c.pin_d7 = CAM_PIN_D7; c.pin_d6 = CAM_PIN_D6; c.pin_d5 = CAM_PIN_D5; c.pin_d4 = CAM_PIN_D4;
  c.pin_d3 = CAM_PIN_D3; c.pin_d2 = CAM_PIN_D2; c.pin_d1 = CAM_PIN_D1; c.pin_d0 = CAM_PIN_D0;
  c.pin_vsync = CAM_PIN_VSYNC; c.pin_href = CAM_PIN_HREF; c.pin_pclk = CAM_PIN_PCLK;
  c.xclk_freq_hz = CAM_XCLK_HZ;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = framesizeFor(settings.videoWidth, settings.videoHeight);
  c.jpeg_quality = settings.videoQuality;
  c.fb_count     = CAM_FB_COUNT;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_LATEST;   // never let a stale fb queue build up

  esp_err_t e = esp_camera_init(&c);
  if (e != ESP_OK) { Serial.printf("[cam] init failed 0x%x\n", e); camOk = false; return false; }

  camOk = true;
  Serial.printf("[cam] init ok %ux%u q%u\n", settings.videoWidth, settings.videoHeight,
                settings.videoQuality);
  return true;
}

bool cameraReady() { return camOk; }

// ---------------------------------------------------------------------------
// Producer
// ---------------------------------------------------------------------------
static void publish(const camera_fb_t* fb) {
  if (fb->len > SLOT_BYTES) { droppedFrames++; return; }   // absurd frame, skip

  LOCK();
  int s = -1;
  uint32_t oldest = UINT32_MAX;
  for (int i = 0; i < RING_FRAME_COUNT; i++) {
    if (ring[i].refs != 0) continue;                        // borrowed or being filled
    if (ring[i].id == 0) { s = i; break; }                  // virgin slot first
    if (ring[i].id < oldest) { oldest = ring[i].id; s = i; }
  }
  if (s >= 0) ring[s].refs = -1;                            // reserve before unlocking
  UNLOCK();

  if (s < 0) { droppedFrames++; return; }                   // every slot held: consumers are slow

  memcpy(ring[s].buf, fb->buf, fb->len);                    // long copy, NO lock held

  LOCK();
  ring[s].len  = fb->len;
  ring[s].ts   = (uint32_t)time(nullptr);
  ring[s].id   = nextId++;
  ring[s].refs = 0;                                         // published
  UNLOCK();

  lastFrameMs = millis();
}

extern "C" void camTask(void* pv) {
  (void)pv;
  esp_task_wdt_reset();
  cameraInit();

  uint32_t lastOk = millis();
  uint32_t winStart = lastOk;
  uint32_t winFrames = 0;
  TickType_t last = xTaskGetTickCount();

  for (;;) {
    esp_task_wdt_reset();

    if (!camOk) {
      Serial.println("[cam] retrying init");
      cameraInit();
      if (!camOk) vTaskDelay(pdMS_TO_TICKS(500));
    } else {
      camera_fb_t* fb = esp_camera_fb_get();
      if (!fb) {
        Serial.println("[cam] fb_get failed");
        camOk = false;                       // force a reinit on the next pass
      } else {
        publish(fb);
        esp_camera_fb_return(fb);
        lastOk = millis();
        winFrames++;
      }
    }

    uint32_t now = millis();
    if (now - winStart >= 1000) {
      measuredFps = winFrames * 1000.0f / (now - winStart);
      winFrames = 0; winStart = now;
    }

    // HANDOVER 4: stall > WDT_TIMEOUT_S = log + restart. 24/7 means auto-recovery.
    // Explicit restart rather than starving the WDT so the reason lands in the log.
    if (now - lastOk > (uint32_t)WDT_TIMEOUT_S * 1000) {
      Serial.printf("[cam] stalled %lu ms — restarting\n", (unsigned long)(now - lastOk));
      Serial.flush();
      esp_restart();
    }

    uint8_t fps = settings.videoFps ? settings.videoFps : VIDEO_FPS;
    vTaskDelayUntil(&last, pdMS_TO_TICKS(1000 / fps));
  }
}

// ---------------------------------------------------------------------------
// Consumers — every one of these holds the mutex for a handful of instructions.
// ---------------------------------------------------------------------------
static void fill(Frame* out, int s) {
  out->jpeg = ring[s].buf; out->len = ring[s].len;
  out->ts   = ring[s].ts;  out->id  = ring[s].id;
}

bool ringBorrowLatest(Frame* out) {
  if (!mux || !out) return false;
  LOCK();
  int best = -1; uint32_t bid = 0;
  for (int i = 0; i < RING_FRAME_COUNT; i++)
    if (ring[i].refs >= 0 && ring[i].id > bid) { bid = ring[i].id; best = i; }
  if (best < 0) { UNLOCK(); return false; }
  ring[best].refs++;
  rememberBorrow(ring[best].id);
  fill(out, best);
  UNLOCK();
  return true;
}

bool ringBorrowNext(uint32_t* cursor, Frame* out) {
  if (!mux || !cursor || !out) return false;
  LOCK();
  int best = -1; uint32_t bid = UINT32_MAX;
  for (int i = 0; i < RING_FRAME_COUNT; i++)
    if (ring[i].refs >= 0 && ring[i].id > *cursor && ring[i].id < bid) { bid = ring[i].id; best = i; }
  if (best < 0) { UNLOCK(); return false; }
  if (*cursor && bid > *cursor + 1) droppedFrames += bid - *cursor - 1;  // recycled under us
  *cursor = bid;
  ring[best].refs++;
  rememberBorrow(bid);
  fill(out, best);
  UNLOCK();
  return true;
}

void ringReleaseId(uint32_t frameId) {
  if (!mux) return;
  LOCK();
  int s = slotOfId(frameId);
  if (s >= 0 && ring[s].refs > 0) ring[s].refs--;
  TaskHandle_t me = xTaskGetCurrentTaskHandle();
  for (int i = 0; i < MAX_BORROWERS; i++)
    if (borrowers[i].task == me && borrowers[i].id == frameId) { borrowers[i].task = nullptr; break; }
  UNLOCK();
}

void ringRelease() {
  if (!mux) return;
  TaskHandle_t me = xTaskGetCurrentTaskHandle();
  uint32_t id = 0;
  LOCK();
  for (int i = 0; i < MAX_BORROWERS; i++)
    if (borrowers[i].task == me) { id = borrowers[i].id; break; }
  UNLOCK();
  if (id) ringReleaseId(id);
}

bool cameraGetLatestJpeg(uint8_t** buf, size_t* len, uint32_t* frameId) {
  Frame f;
  if (!ringBorrowLatest(&f)) return false;
  if (buf)     *buf = (uint8_t*)f.jpeg;
  if (len)     *len = f.len;
  if (frameId) *frameId = f.id;
  return true;
}

void cameraReleaseJpeg(uint32_t frameId) { ringReleaseId(frameId); }

float    cameraFps()         { return measuredFps; }
uint32_t cameraDropped()     { return droppedFrames; }
uint32_t cameraLastFrameMs() { return lastFrameMs; }
