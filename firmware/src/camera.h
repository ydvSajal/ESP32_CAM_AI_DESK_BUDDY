#pragma once
// camera.h — Owner: A1. Runs on CORE_REALTIME (core 0), camTask prio 5.
// camTask grabs frames into the PSRAM ring buffer. Consumers: sdTask, netTask.
// Implement per HANDOVER.md section 4.
#include <Arduino.h>

extern "C" void camTask(void* pv);

bool cameraInit();          // esp32-camera init from config.h pins + settings geometry
bool cameraReady();

// ---------------------------------------------------------------------------
// PSRAM ring buffer — RING_FRAME_COUNT fixed-size slots, allocated once at boot.
//
// camTask (producer) copies each JPEG out of the driver framebuffer into a free
// slot and returns the fb immediately, so the camera never waits on a consumer.
// Every slot carries a refcount. Borrowing takes the mutex only long enough to
// bump that count; the mutex is NEVER held across an SD write or a socket write.
// A borrowed slot is simply not reusable, so a slow HTTP client costs frames
// (dropped, counted) and never a stall: recording always wins.
//
// One borrow per task at a time when using the no-arg ringRelease() form.
// ---------------------------------------------------------------------------
struct Frame { const uint8_t* jpeg; size_t len; uint32_t ts; uint32_t id; };

bool  ringBorrowLatest(Frame* out);   // newest published frame; caller must release
void  ringRelease();                  // releases whatever THIS task borrowed
void  ringReleaseId(uint32_t frameId);// explicit form (cross-task safe)

// Sequential consumer seam used by sdTask: hands back the oldest frame newer
// than *cursor and advances it. Frames recycled before we got to them are
// counted in cameraDropped(). Returns false when nothing new is available.
bool  ringBorrowNext(uint32_t* cursor, Frame* out);

// ---------------------------------------------------------------------------
// Public API for A2 (core 1: /api/snapshot, /api/stream). Thread-safe.
//   uint8_t* b; size_t n; uint32_t id;
//   if (cameraGetLatestJpeg(&b, &n, &id)) { send(b, n); cameraReleaseJpeg(id); }
// Release promptly — an unreleased frame permanently shrinks the ring.
// ---------------------------------------------------------------------------
bool cameraGetLatestJpeg(uint8_t** buf, size_t* len, uint32_t* frameId);
void cameraReleaseJpeg(uint32_t frameId);

float    cameraFps();          // measured, for /api/status
uint32_t cameraDropped();
uint32_t cameraLastFrameMs();  // millis() of the last published frame, 0 = none yet
