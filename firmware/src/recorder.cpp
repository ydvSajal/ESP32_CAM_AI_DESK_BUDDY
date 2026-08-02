#include "recorder.h"
#include "camera.h"
#include "config.h"
#include "storage.h"
#include "rec_pure.h"

#include <SD_MMC.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <time.h>

QueueHandle_t recSegmentQueue = nullptr;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static SemaphoreHandle_t sdMux = nullptr;

static bool     mounted = false;
static File     avi;
static bool     segOpen = false;
static uint8_t  hdr[AVI_HDR_BYTES];
static uint8_t* idx = nullptr;          // idx1 payload, built in PSRAM
static uint32_t idxCount = 0;           // idx1 entries (capped, see REC_IDX_MAX_FRAMES)
static uint32_t frameCount = 0;         // frames actually written (drives dwTotalFrames)
static uint32_t moviBytes = 0;
static char     curPath[REC_PATH_MAX] = "";
static char     curDay[9] = "";
static uint32_t segStart = 0;           // epoch seconds
static uint32_t segsToday = 0;
static uint32_t ringCursor = 0;

static uint64_t cachedTotal = 0, cachedUsed = 0, cachedFree = 0;
static uint32_t usageAt = 0;

static char pendingPurge[9] = "";       // day dir being deleted, "" = none

// SD write load, 0..100. Derived from what sdTask already tracks: how many ring slots
// it had to drain in one pass (a deep backlog means the card is behind the camera) and
// whether cameraDropped() moved (the ring actually overflowed = pegged).
// ponytail: an EMA over two existing counters, no new instrumentation and no timing
// code in the write path. Ceiling: if a card ever stalls in bursts shorter than one
// sdTask pass, measure write() latency here instead.
static volatile uint8_t writeLoad = 0;
static uint32_t         lastDropped = 0;

uint8_t recorderWriteLoadPct() { return writeLoad; }

// docs/API.md caps video.fps at 20, so this is the worst-case index size:
// 300 s * 20 fps * 16 B = 96 KB of PSRAM, allocated once.
#define REC_IDX_MAX_FRAMES ((uint32_t)SEGMENT_SECONDS * 20)

#define SDLOCK()   xSemaphoreTake(sdMux, portMAX_DELAY)
#define SDUNLOCK() xSemaphoreGive(sdMux)

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static const char* baseName(const char* p) {          // Arduino 2.x name() is a full path
  const char* s = strrchr(p, '/');
  return s ? s + 1 : p;
}
static bool endsWith(const char* s, const char* suf) {
  size_t ls = strlen(s), lf = strlen(suf);
  return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}
static void sidecar(char* out, size_t n, const char* aviPath, const char* ext) {
  snprintf(out, n, "%s%s", aviPath, ext);
}

bool sdMounted()                   { return mounted; }
bool recordingActive()             { return segOpen; }
const char* recorderCurrentFile()  { return segOpen ? curPath : ""; }
uint32_t recorderSegmentsToday()   { return segsToday; }
uint32_t recorderSegmentElapsed() {
  if (!segOpen) return 0;
  uint32_t now = (uint32_t)time(nullptr);
  return now > segStart ? now - segStart : 0;
}

static void refreshUsage(bool force) {
  if (!mounted) { cachedTotal = cachedUsed = cachedFree = 0; return; }
  if (!force && usageAt && millis() - usageAt < SD_USAGE_REFRESH_MS) return;
  cachedTotal = SD_MMC.totalBytes();
  cachedUsed  = SD_MMC.usedBytes();
  cachedFree  = cachedTotal > cachedUsed ? cachedTotal - cachedUsed : 0;
  uint32_t now = millis();
  usageAt = now ? now : 1;                  // 0 means "never sampled"
}

void recorderUsage(uint64_t* t, uint64_t* u, uint64_t* f) {
  if (t) *t = cachedTotal / (1024ull * 1024);
  if (u) *u = cachedUsed  / (1024ull * 1024);
  if (f) *f = cachedFree  / (1024ull * 1024);
}

// ---------------------------------------------------------------------------
// Mount
// ---------------------------------------------------------------------------
static bool sdMount() {
  SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0);
  if (!SD_MMC.begin(SD_MOUNT_POINT, true)) {       // true = 1-bit mode (config.h wiring)
    Serial.println("[rec] SD mount failed");
    mounted = false;
    return false;
  }
  mounted = true;
  SD_MMC.mkdir(REC_DIR);
  refreshUsage(true);
  Serial.printf("[rec] SD mounted, %llu MB free\n", cachedFree / (1024ull * 1024));
  return true;
}

// ---------------------------------------------------------------------------
// Segment open / write / close
// ---------------------------------------------------------------------------
static bool openSegment(uint32_t epoch) {
  char day[9];
  segDayStr(day, epoch, settings.tzOffsetMin);
  if (strcmp(day, curDay) != 0) { strcpy(curDay, day); segsToday = 0; }

  char dir[24];
  snprintf(dir, sizeof(dir), "%s/%s", REC_DIR, day);
  SD_MMC.mkdir(dir);

  segPath(curPath, sizeof(curPath), epoch, settings.tzOffsetMin);

  SDLOCK();
  avi = SD_MMC.open(curPath, FILE_WRITE);
  if (!avi) { SDUNLOCK(); Serial.printf("[rec] open failed %s\n", curPath); return false; }

  aviBuildHeader(hdr, settings.videoWidth, settings.videoHeight, settings.videoFps);
  bool ok = avi.write(hdr, AVI_HDR_BYTES) == AVI_HDR_BYTES;

  // "being written" marker — boot recovery deletes any .avi still wearing it.
  char m[REC_PATH_MAX + 12];
  sidecar(m, sizeof(m), curPath, ".rec");
  File f = SD_MMC.open(m, FILE_WRITE);
  if (f) f.close();
  SDUNLOCK();

  if (!ok) { avi.close(); return false; }

  idxCount = 0; frameCount = 0; moviBytes = 0; segStart = epoch; segOpen = true;
  Serial.printf("[rec] segment %s\n", curPath);
  return true;
}

static bool writeFrame(const Frame& f) {
  if (!segOpen || f.len == 0) return true;

  uint8_t ch[8];
  wrcc(ch, "00dc");
  wr32(ch + 4, (uint32_t)f.len);
  if (avi.write(ch, 8) != 8) return false;
  if (avi.write(f.jpeg, f.len) != f.len) return false;
  if (f.len & 1) { uint8_t z = 0; if (avi.write(&z, 1) != 1) return false; }

  if (idx && idxCount < REC_IDX_MAX_FRAMES) {
    aviIndexEntry(idx + idxCount * AVI_IDX_ENTRY_BYTES,
                  moviBytes + AVI_MOVI_FIRST_OFF, (uint32_t)f.len);
    idxCount++;
  }
  moviBytes += aviChunkBytes((uint32_t)f.len);
  frameCount++;
  return true;
}

static void closeSegment() {
  if (!segOpen) return;
  segOpen = false;

  SDLOCK();
  uint32_t idxBytes = idxCount * AVI_IDX_ENTRY_BYTES;
  if (idx && idxBytes) {
    uint8_t ic[8];
    wrcc(ic, "idx1");
    wr32(ic + 4, idxBytes);
    avi.write(ic, 8);
    avi.write(idx, idxBytes);
  }
  // Patch every size field, then rewrite the whole 224-byte prefix in one seek.
  aviPatchHeader(hdr, frameCount, moviBytes, idxBytes);
  avi.seek(0);
  avi.write(hdr, AVI_HDR_BYTES);
  avi.close();

  char rec[REC_PATH_MAX + 12], pend[REC_PATH_MAX + 12];
  sidecar(rec,  sizeof(rec),  curPath, ".rec");
  sidecar(pend, sizeof(pend), curPath, ".pending");
  SD_MMC.rename(rec, pend);          // atomic-enough: clean close == ".pending"
  SDUNLOCK();

  segsToday++;

  if (recSegmentQueue) {
    RecSegmentMsg m;
    strncpy(m.path, curPath, REC_PATH_MAX - 1);
    m.path[REC_PATH_MAX - 1] = 0;
    xQueueSend(recSegmentQueue, &m, 0);   // full queue is fine: .pending survives a reboot
  }
  Serial.printf("[rec] closed %s (%lu frames)\n", curPath, (unsigned long)frameCount);
}

// ---------------------------------------------------------------------------
// Boot recovery: finish the ".pending" backlog, drop the interrupted segment.
//
// ponytail: an interrupted segment is DELETED, not salvaged. T2 allows "at most
// the last partial segment lost", and rebuilding an index by re-scanning JPEG
// chunk headers is a lot of code for 5 minutes of footage. Ceiling: if that
// footage ever matters, walk the movi chunks and rebuild idx1 here instead.
// ---------------------------------------------------------------------------
static void recoverScan() {
  File root = SD_MMC.open(REC_DIR);
  if (!root) return;
  for (File d = root.openNextFile(); d; d = root.openNextFile()) {
    if (!d.isDirectory()) { d.close(); continue; }
    char dir[24];
    snprintf(dir, sizeof(dir), "%s/%s", REC_DIR, baseName(d.name()));
    d.close();

    File dd = SD_MMC.open(dir);
    if (!dd) continue;
    for (File e = dd.openNextFile(); e; e = dd.openNextFile()) {
      char full[REC_PATH_MAX + 12];
      snprintf(full, sizeof(full), "%s/%s", dir, baseName(e.name()));
      e.close();

      if (endsWith(full, ".rec")) {
        char base[REC_PATH_MAX + 12];
        strncpy(base, full, sizeof(base) - 1); base[sizeof(base) - 1] = 0;
        base[strlen(base) - 4] = 0;                 // strip ".rec"
        Serial.printf("[rec] discarding interrupted %s\n", base);
        SD_MMC.remove(base);
        SD_MMC.remove(full);
      } else if (endsWith(full, ".pending") && recSegmentQueue) {
        RecSegmentMsg m;
        size_t n = strlen(full) - 8;                // strip ".pending"
        if (n >= REC_PATH_MAX) continue;
        memcpy(m.path, full, n); m.path[n] = 0;
        xQueueSend(recSegmentQueue, &m, 0);
      }
    }
    dd.close();
  }
  root.close();
}

void recMarkUploaded(const char* aviPath) {
  if (!mounted || !aviPath || !aviPath[0]) return;
  char p[REC_PATH_MAX + 12];
  sidecar(p, sizeof(p), aviPath, ".pending");
  SDLOCK();
  SD_MMC.remove(p);
  SDUNLOCK();
}

bool recIsUploaded(const char* aviPath) {
  if (!mounted || !aviPath || !aviPath[0]) return false;
  char p[REC_PATH_MAX + 12], r[REC_PATH_MAX + 12];
  sidecar(p, sizeof(p), aviPath, ".pending");
  sidecar(r, sizeof(r), aviPath, ".rec");
  return !SD_MMC.exists(p) && !SD_MMC.exists(r);
}

// ---------------------------------------------------------------------------
// Retention — delete the oldest day, a batch of files per pass so a purge never
// stalls the writer. Recording continues through the whole thing.
// ---------------------------------------------------------------------------
static void purgeStep() {
  char dir[24];
  snprintf(dir, sizeof(dir), "%s/%s", REC_DIR, pendingPurge);

  SDLOCK();
  File d = SD_MMC.open(dir);
  if (!d) { SDUNLOCK(); pendingPurge[0] = 0; return; }
  // Unlinking while the dir handle is open can make FATFS skip an entry; harmless,
  // the next pass picks it up because we only rmdir once a pass deletes nothing.
  int n = 0;
  for (File e = d.openNextFile(); e && n < SD_DELETE_BATCH; e = d.openNextFile()) {
    char full[REC_PATH_MAX + 12];
    snprintf(full, sizeof(full), "%s/%s", dir, baseName(e.name()));
    e.close();
    SD_MMC.remove(full);
    n++;
  }
  d.close();
  if (n == 0) { SD_MMC.rmdir(dir); pendingPurge[0] = 0; Serial.printf("[rec] purged %s\n", dir); }
  SDUNLOCK();

  if (n) refreshUsage(true);
}

static void maybeRetention() {
  if (pendingPurge[0]) { purgeStep(); return; }
  refreshUsage(false);
  if (cachedFree >= (uint64_t)SD_FREE_MIN_MB * 1024 * 1024) return;

  // Collect day names, then let the pure decision function choose.
  static char names[32][9];
  const char* ptrs[32];
  int n = 0;
  SDLOCK();
  File root = SD_MMC.open(REC_DIR);
  if (root) {
    for (File d = root.openNextFile(); d && n < 32; d = root.openNextFile()) {
      if (d.isDirectory()) { strncpy(names[n], baseName(d.name()), 8); names[n][8] = 0; ptrs[n] = names[n]; n++; }
      d.close();
    }
    root.close();
  }
  SDUNLOCK();

  int pick = retentionPickDay(cachedFree, (uint64_t)SD_FREE_MIN_MB * 1024 * 1024, ptrs, n, curDay);
  if (pick < 0) {
    Serial.println("[rec] low space but nothing safe to delete — recording anyway");
    return;
  }
  strcpy(pendingPurge, names[pick]);
  Serial.printf("[rec] retention: purging %s\n", pendingPurge);
}

// ---------------------------------------------------------------------------
// /api/recordings backing
// ---------------------------------------------------------------------------
uint32_t recListDays(RecDay* out, uint32_t max) {
  if (!mounted || !out || !max) return 0;
  uint32_t n = 0;
  SDLOCK();
  File root = SD_MMC.open(REC_DIR);
  if (root) {
    for (File d = root.openNextFile(); d && n < max; d = root.openNextFile()) {
      if (!d.isDirectory()) { d.close(); continue; }
      char dir[24];
      snprintf(dir, sizeof(dir), "%s/%s", REC_DIR, baseName(d.name()));
      strncpy(out[n].day, baseName(d.name()), 8); out[n].day[8] = 0;
      d.close();

      out[n].segments = 0; out[n].bytes = 0;
      File dd = SD_MMC.open(dir);
      if (dd) {
        for (File e = dd.openNextFile(); e; e = dd.openNextFile()) {
          if (endsWith(baseName(e.name()), ".avi")) { out[n].segments++; out[n].bytes += e.size(); }
          e.close();
        }
        dd.close();
      }
      n++;
    }
    root.close();
  }
  SDUNLOCK();

  for (uint32_t i = 1; i < n; i++) {            // newest first; n <= 32ish, insertion sort is fine
    RecDay k = out[i]; int j = (int)i - 1;
    while (j >= 0 && strcmp(out[j].day, k.day) < 0) { out[j + 1] = out[j]; j--; }
    out[j + 1] = k;
  }
  return n;
}

bool recDayExists(const char* day) {
  if (!mounted || !day || !day[0]) return false;
  char dir[24];
  snprintf(dir, sizeof(dir), "%s/%s", REC_DIR, day);
  return SD_MMC.exists(dir);
}

uint32_t recListSegments(const char* day, RecSegment* out, uint32_t max, bool* truncated) {
  if (truncated) *truncated = false;
  if (!mounted || !day || !out || !max) return 0;

  char dir[24];
  snprintf(dir, sizeof(dir), "%s/%s", REC_DIR, day);
  uint32_t n = 0;

  SDLOCK();
  File dd = SD_MMC.open(dir);
  if (dd) {
    for (File e = dd.openNextFile(); e; e = dd.openNextFile()) {
      const char* bn = baseName(e.name());
      if (!endsWith(bn, ".avi")) { e.close(); continue; }
      if (n >= max) { if (truncated) *truncated = true; e.close(); break; }
      strncpy(out[n].name, bn, sizeof(out[n].name) - 1);
      out[n].name[sizeof(out[n].name) - 1] = 0;
      out[n].bytes = e.size();
      out[n].start = segEpoch(day, bn, settings.tzOffsetMin);
      e.close();

      char full[REC_PATH_MAX];
      snprintf(full, sizeof(full), "%s/%s", dir, out[n].name);
      bool isCurrent = segOpen && strcmp(full, curPath) == 0;
      out[n].durationS = isCurrent ? recorderSegmentElapsed() : SEGMENT_SECONDS;

      char p[REC_PATH_MAX + 12], r[REC_PATH_MAX + 12];
      sidecar(p, sizeof(p), full, ".pending");
      sidecar(r, sizeof(r), full, ".rec");
      out[n].uploaded = !SD_MMC.exists(p) && !SD_MMC.exists(r);
      n++;
    }
    dd.close();
  }
  SDUNLOCK();
  return n;
}

// ---------------------------------------------------------------------------
// sdTask
//
// ponytail: ONE mutex (sdMux) around mount / open / close / listing / purge, and
// per-frame writes deliberately left outside it — ESP-IDF's FATFS is reentrant
// per volume and taking a mutex 10x/s to guard against a once-a-minute listing
// is the wrong trade. Ceiling: if concurrent /api/recordings ever corrupts a
// write, move the writeFrame() call inside the lock and eat the contention.
//
// ponytail: segment rotation is close-then-open inline, not pre-open. The ring
// buffer holds RING_FRAME_COUNT frames (~2.4 s at 10 fps), which is longer than
// a close (idx1 write + one 224-byte seek) takes, so the T2 "gap < 2 s" criterion
// is met by buffering rather than by juggling two open files. Ceiling: if a slow
// card ever pushes close past ~2 s, pre-open the next file before closing.
// ---------------------------------------------------------------------------
extern "C" void sdTask(void* pv) {
  (void)pv;
  esp_task_wdt_reset();

  sdMux = xSemaphoreCreateMutex();
  recSegmentQueue = xQueueCreate(REC_UPLOAD_QUEUE_LEN, sizeof(RecSegmentMsg));
  idx = (uint8_t*)heap_caps_malloc(REC_IDX_MAX_FRAMES * AVI_IDX_ENTRY_BYTES, MALLOC_CAP_SPIRAM);
  if (!idx) Serial.println("[rec] no PSRAM for idx1 — files will play but not seek");

  bool recovered = false;

  for (;;) {
    esp_task_wdt_reset();

    if (!mounted) {
      if (!sdMount()) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
      recovered = false;
    }
    if (!recovered) { recoverScan(); recovered = true; }

    uint32_t now = (uint32_t)time(nullptr);

    // ringCursor deliberately survives rotation: the new segment picks up at the
    // exact frame the old one stopped at, so nothing is duplicated or dropped.
    if (!segOpen && !openSegment(now)) {
      mounted = false; vTaskDelay(pdMS_TO_TICKS(1000)); continue;
    }

    // Drain the ring. Bounded by RING_FRAME_COUNT, so the watchdog is never at risk.
    Frame f;
    bool writeFailed = false;
    uint32_t drained = 0;
    while (ringBorrowNext(&ringCursor, &f)) {
      if (!writeFrame(f)) writeFailed = true;
      ringReleaseId(f.id);
      drained++;
      if (writeFailed) break;
    }

    // Write load: backlog as a fraction of the ring, pegged to 100 if we lost frames.
    uint32_t dropped = cameraDropped();
    uint32_t pct = drained * 100u / RING_FRAME_COUNT;
    if (dropped != lastDropped) { lastDropped = dropped; pct = 100; }
    if (pct > 100) pct = 100;
    writeLoad = (uint8_t)((writeLoad * 3u + pct) / 4u);     // EMA, ~4 passes to settle

    if (writeFailed) {
      Serial.printf("[rec] write error on %s — remounting\n", curPath);
      segOpen = false;
      avi.close();
      SD_MMC.end();
      mounted = false;
      continue;
    }

    // Rotate. If NTP lands mid-segment the clock jumps and this closes early —
    // one short segment after a resync, which is correct behaviour, not a bug.
    if (now - segStart >= SEGMENT_SECONDS) { closeSegment(); continue; }   // reopen next pass

    maybeRetention();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
