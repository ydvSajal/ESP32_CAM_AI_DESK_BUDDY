#pragma once
// recorder.h — Owner: A1. Runs on CORE_REALTIME (core 0), sdTask prio 4.
// Ring buffer -> AVI segment on SD, rotate every SEGMENT_SECONDS, retention =
// delete oldest day. Implement per HANDOVER.md section 4.
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

extern "C" void sdTask(void* pv);

bool sdMounted();
bool recordingActive();
const char* recorderCurrentFile();   // "" when idle
uint32_t recorderSegmentElapsed();   // seconds into the current segment
uint32_t recorderSegmentsToday();
void recorderUsage(uint64_t* totalMb, uint64_t* usedMb, uint64_t* freeMb);

// 0..100 — how hard sdTask is working to keep up with the camera, derived from the
// ring-buffer backlog it had to drain last pass plus any newly dropped frames. This is
// the "recording always wins" gate: uploadTask must stand down above
// UPLOAD_MAX_SD_LOAD_PCT. Cheap read of one volatile, safe from any task.
uint8_t recorderWriteLoadPct();

// ---------------------------------------------------------------------------
// Segment -> uploader handoff (A2 owns the draining end).
//
// sdTask pushes one message per CLOSED segment. uploadTask drains it:
//     RecSegmentMsg m;
//     if (xQueueReceive(recSegmentQueue, &m, 0) == pdTRUE) { upload(m.path); ... }
//
// Persistence: every open segment gets a zero-byte "<path>.rec" sidecar, renamed
// to "<path>.pending" when the segment closes cleanly. After a successful upload
// A2 MUST call recMarkUploaded(path) — that deletes the sidecar. On boot sdTask
// re-queues every ".pending" it finds and deletes every ".avi" still marked
// ".rec" (interrupted mid-write). That is the whole persistence story: two
// zero-byte files, no manifest to corrupt.
//
// ponytail: sidecars, not a manifest — a manifest is one more thing that can be
// half-written by a power cut. Ceiling: if the queue ever needs ordering or
// retry counts, put a JSON manifest per day and keep the sidecars as the truth.
// The queue is lossy by design: if it is full the segment still has its
// ".pending" sidecar and gets picked up on the next boot scan. Recording never
// blocks on the uploader.
// ---------------------------------------------------------------------------
#define REC_PATH_MAX 48                      // "/rec/20260802/003000.avi" + slack

struct RecSegmentMsg { char path[REC_PATH_MAX]; };

extern QueueHandle_t recSegmentQueue;        // items are RecSegmentMsg, created in sdTask

void recMarkUploaded(const char* aviPath);   // deletes the ".pending" sidecar
bool recIsUploaded(const char* aviPath);     // no sidecar == uploaded (or never queued)

// ---------------------------------------------------------------------------
// GET /api/recordings backing (A2, core 1). Both do a directory scan under the
// SD mutex — call them from a request handler, not from a tight loop.
// ---------------------------------------------------------------------------
struct RecDay     { char day[9];   uint32_t segments; uint64_t bytes; };
struct RecSegment { char name[16]; uint64_t bytes; uint32_t start; uint32_t durationS; bool uploaded; };

uint32_t recListDays(RecDay* out, uint32_t max);                    // newest first
bool     recDayExists(const char* day);                             // for the 404
uint32_t recListSegments(const char* day, RecSegment* out, uint32_t max, bool* truncated);
