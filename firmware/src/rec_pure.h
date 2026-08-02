#pragma once
// rec_pure.h — Owner: A1. The parts of recorder.cpp that are pure byte/string math:
// AVI header + index layout, segment path formatting, retention decision.
//
// NO Arduino, NO FreeRTOS, NO SD here on purpose — this header compiles under
// [env:native] so /test/firmware/test_recorder can assert the exact byte layout
// without hardware. recorder.cpp includes it and does nothing but I/O around it.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "config.h"

// ---------------------------------------------------------------------------
// little-endian writers
// ---------------------------------------------------------------------------
static inline void wr16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static inline void wr32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24;
}
static inline void wrcc(uint8_t* p, const char* cc) { memcpy(p, cc, 4); }
static inline uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------------------------------------------------------------------------
// AVI (MJPEG, single video stream, idx1). Fixed 224-byte prefix, no JUNK pad.
//
//   0   RIFF <size> 'AVI '
//   12  LIST 192 'hdrl'
//   24    avih 56
//   88    LIST 116 'strl'
//   100     strh 56
//   164     strf 40  (BITMAPINFOHEADER, biCompression = MJPG)
//   212  LIST <size> 'movi'          <- frame chunks start at 224
//   ...  '00dc' <len> <jpeg> [pad]
//   ...  idx1 <n*16>
// ---------------------------------------------------------------------------
#define AVI_HDR_BYTES        224
#define AVI_OFF_RIFFSIZE       4
#define AVI_OFF_USPF          32
#define AVI_OFF_TOTALFRAMES   48
#define AVI_OFF_WIDTH         64
#define AVI_OFF_HEIGHT        68
#define AVI_OFF_STRH_RATE    132
#define AVI_OFF_STRH_LENGTH  140
#define AVI_OFF_MOVISIZE     216
#define AVI_IDX_ENTRY_BYTES   16
#define AVI_MOVI_FIRST_OFF     4   // idx1 offsets are relative to the 'movi' FOURCC

// Writes the 224-byte prefix with zeroed patch fields. Returns AVI_HDR_BYTES.
static inline size_t aviBuildHeader(uint8_t* h, uint16_t w, uint16_t hgt, uint8_t fps) {
  memset(h, 0, AVI_HDR_BYTES);
  if (fps == 0) fps = 1;

  wrcc(h + 0, "RIFF");  wr32(h + 4, 0);            wrcc(h + 8, "AVI ");
  wrcc(h + 12, "LIST"); wr32(h + 16, 192);         wrcc(h + 20, "hdrl");

  wrcc(h + 24, "avih"); wr32(h + 28, 56);
  wr32(h + 32, 1000000u / fps);   // dwMicroSecPerFrame
  wr32(h + 44, 0x10);             // dwFlags = AVIF_HASINDEX
  wr32(h + 48, 0);                // dwTotalFrames (patched)
  wr32(h + 56, 1);                // dwStreams
  wr32(h + 64, w);                // dwWidth
  wr32(h + 68, hgt);              // dwHeight

  wrcc(h + 88, "LIST"); wr32(h + 92, 116);         wrcc(h + 96, "strl");

  wrcc(h + 100, "strh"); wr32(h + 104, 56);
  wrcc(h + 108, "vids");
  wrcc(h + 112, "MJPG");
  wr32(h + 128, 1);               // dwScale
  wr32(h + 132, fps);             // dwRate
  wr32(h + 140, 0);               // dwLength (patched)
  wr32(h + 148, 0xFFFFFFFF);      // dwQuality = -1
  wr16(h + 158, 0); wr16(h + 156, 0);
  wr16(h + 160, w); wr16(h + 162, hgt);   // rcFrame right/bottom

  wrcc(h + 164, "strf"); wr32(h + 168, 40);
  wr32(h + 172, 40);              // biSize
  wr32(h + 176, w);
  wr32(h + 180, hgt);
  wr16(h + 184, 1);               // biPlanes
  wr16(h + 186, 24);              // biBitCount
  wrcc(h + 188, "MJPG");          // biCompression
  wr32(h + 192, (uint32_t)w * hgt * 3);

  wrcc(h + 212, "LIST"); wr32(h + 216, 4);         wrcc(h + 220, "movi");
  return AVI_HDR_BYTES;
}

// Fill in every size field once the segment is closed. idxBytes = frames*16 (0 = no index).
static inline void aviPatchHeader(uint8_t* h, uint32_t frames, uint32_t moviBytes, uint32_t idxBytes) {
  uint32_t fileSize = AVI_HDR_BYTES + moviBytes + (idxBytes ? 8 + idxBytes : 0);
  wr32(h + AVI_OFF_RIFFSIZE, fileSize - 8);
  wr32(h + AVI_OFF_TOTALFRAMES, frames);
  wr32(h + AVI_OFF_STRH_LENGTH, frames);
  wr32(h + AVI_OFF_MOVISIZE, 4 + moviBytes);
}

// One idx1 entry. offInMovi = byte offset of the '00dc' chunk header from the 'movi' FOURCC.
static inline void aviIndexEntry(uint8_t* e, uint32_t offInMovi, uint32_t size) {
  wrcc(e + 0, "00dc");
  wr32(e + 4, 0x10);              // AVIIF_KEYFRAME — every MJPEG frame is one
  wr32(e + 8, offInMovi);
  wr32(e + 12, size);
}

// Bytes a frame adds to the movi list (chunk header + payload + even pad).
static inline uint32_t aviChunkBytes(uint32_t jpegLen) { return 8 + jpegLen + (jpegLen & 1); }

// ---------------------------------------------------------------------------
// Segment paths:  /rec/YYYYMMDD/HHMMSS.avi   (local time = epoch + tz)
//
// Own civil-date math in BOTH directions, no <time.h> calls: gmtime_r is absent
// on MinGW (the native test host on Windows) and timegm is absent elsewhere.
// Hinnant's civil_from_days / days_from_civil, ~12 lines, identical on every
// target. This is what makes `pio test -e native` portable.
// ---------------------------------------------------------------------------
static inline void civilFromDays(int64_t z, int* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp  = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp + (mp < 10 ? 3 : (unsigned)-9);
  *y = (int)((int64_t)yoe + era * 400 + (*m <= 2));
}

// Splits local-time epoch seconds into Y/M/D h:m:s.
static inline void civilSplit(uint32_t epoch, int tzOffsetMin,
                              int* y, unsigned* mo, unsigned* d,
                              unsigned* h, unsigned* mi, unsigned* s) {
  int64_t local = (int64_t)epoch + (int64_t)tzOffsetMin * 60;
  int64_t days  = local / 86400;
  int64_t rem   = local % 86400;
  if (rem < 0) { rem += 86400; days--; }            // floor division, pre-1970 safe
  civilFromDays(days, y, mo, d);
  *h  = (unsigned)(rem / 3600);
  *mi = (unsigned)((rem / 60) % 60);
  *s  = (unsigned)(rem % 60);
}

static inline void segDayStr(char out[9], uint32_t epoch, int tzOffsetMin) {
  int y; unsigned mo, d, h, mi, s;
  civilSplit(epoch, tzOffsetMin, &y, &mo, &d, &h, &mi, &s);
  snprintf(out, 9, "%04d%02u%02u", y, mo, d);
}

static inline void segPath(char* out, size_t n, uint32_t epoch, int tzOffsetMin) {
  int y; unsigned mo, d, h, mi, s;
  civilSplit(epoch, tzOffsetMin, &y, &mo, &d, &h, &mi, &s);
  snprintf(out, n, "%s/%04d%02u%02u/%02u%02u%02u.avi", REC_DIR, y, mo, d, h, mi, s);
}

// Inverse of segPath, for /api/recordings "start". day="YYYYMMDD", name="HHMMSS.avi".
static inline int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int)doe - 719468;
}

static inline bool allDigits(const char* s, int n) {
  for (int i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

static inline uint32_t segEpoch(const char* day, const char* name, int tzOffsetMin) {
  // Length alone is not enough: "22.avi" is 6 chars and would parse as garbage.
  if (!day || !name || strlen(day) < 8 || strlen(name) < 6) return 0;
  if (!allDigits(day, 8) || !allDigits(name, 6)) return 0;
  int y = (day[0]-'0')*1000 + (day[1]-'0')*100 + (day[2]-'0')*10 + (day[3]-'0');
  int mo = (day[4]-'0')*10 + (day[5]-'0');
  int d  = (day[6]-'0')*10 + (day[7]-'0');
  int hh = (name[0]-'0')*10 + (name[1]-'0');
  int mm = (name[2]-'0')*10 + (name[3]-'0');
  int ss = (name[4]-'0')*10 + (name[5]-'0');
  int64_t local = daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 + hh*3600 + mm*60 + ss;
  return (uint32_t)(local - (int64_t)tzOffsetMin * 60);
}

// ---------------------------------------------------------------------------
// Retention: which day directory gets deleted next.
// Returns an index into days[], or -1 for "nothing to delete".
// days[] are "YYYYMMDD" strings (strcmp == chronological order, by construction).
// The day we are recording into is never a candidate — deleting it would delete
// the segment currently open.
// ---------------------------------------------------------------------------
static inline int retentionPickDay(uint64_t freeBytes, uint64_t minFreeBytes,
                                   const char* const* days, int nDays,
                                   const char* currentDay) {
  if (freeBytes >= minFreeBytes) return -1;
  int best = -1;
  for (int i = 0; i < nDays; i++) {
    if (!days[i] || !days[i][0]) continue;
    if (currentDay && currentDay[0] && strcmp(days[i], currentDay) == 0) continue;
    if (best < 0 || strcmp(days[i], days[best]) < 0) best = i;
  }
  return best;
}
