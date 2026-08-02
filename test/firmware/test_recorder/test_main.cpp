// Native unit tests for the pure half of recorder.cpp (firmware/src/rec_pure.h).
// No Arduino, no SD, no camera — byte layout, path math, retention decision only.
//
//   cd firmware && pio test -e native
//
// NOTE (A1 -> A0): firmware/platformio.ini has no `test_dir`, so PlatformIO looks
// in firmware/test. Add `test_dir = ../test/firmware` to [platformio] for this to
// be discovered. platformio.ini is A0's file, so it is not edited here.

#include <unity.h>
#include <string.h>
#include "rec_pure.h"

void setUp() {}       // Unity links against these; test_services/test_display define them too.
void tearDown() {}

// ---------------------------------------------------------------------------
// AVI header layout
// ---------------------------------------------------------------------------
static void test_header_shape(void) {
  uint8_t h[AVI_HDR_BYTES];
  TEST_ASSERT_EQUAL_UINT32(224, aviBuildHeader(h, 800, 600, 10));

  TEST_ASSERT_EQUAL_MEMORY("RIFF", h + 0,   4);
  TEST_ASSERT_EQUAL_MEMORY("AVI ", h + 8,   4);
  TEST_ASSERT_EQUAL_MEMORY("LIST", h + 12,  4);
  TEST_ASSERT_EQUAL_MEMORY("hdrl", h + 20,  4);
  TEST_ASSERT_EQUAL_MEMORY("avih", h + 24,  4);
  TEST_ASSERT_EQUAL_MEMORY("LIST", h + 88,  4);
  TEST_ASSERT_EQUAL_MEMORY("strl", h + 96,  4);
  TEST_ASSERT_EQUAL_MEMORY("strh", h + 100, 4);
  TEST_ASSERT_EQUAL_MEMORY("vids", h + 108, 4);
  TEST_ASSERT_EQUAL_MEMORY("MJPG", h + 112, 4);
  TEST_ASSERT_EQUAL_MEMORY("strf", h + 164, 4);
  TEST_ASSERT_EQUAL_MEMORY("MJPG", h + 188, 4);   // biCompression
  TEST_ASSERT_EQUAL_MEMORY("LIST", h + 212, 4);
  TEST_ASSERT_EQUAL_MEMORY("movi", h + 220, 4);

  // The two list sizes are structural constants — if they drift, VLC gives up.
  TEST_ASSERT_EQUAL_UINT32(192, rd32(h + 16));   // hdrl
  TEST_ASSERT_EQUAL_UINT32(116, rd32(h + 92));   // strl
  TEST_ASSERT_EQUAL_UINT32(56,  rd32(h + 28));   // avih size
  TEST_ASSERT_EQUAL_UINT32(56,  rd32(h + 104));  // strh size
  TEST_ASSERT_EQUAL_UINT32(40,  rd32(h + 168));  // strf size
}

static void test_header_values(void) {
  uint8_t h[AVI_HDR_BYTES];
  aviBuildHeader(h, 800, 600, 10);

  TEST_ASSERT_EQUAL_UINT32(100000, rd32(h + AVI_OFF_USPF));      // 1e6 / 10 fps
  TEST_ASSERT_EQUAL_UINT32(0x10,   rd32(h + 44));                // AVIF_HASINDEX
  TEST_ASSERT_EQUAL_UINT32(1,      rd32(h + 56));                // one stream
  TEST_ASSERT_EQUAL_UINT32(800,    rd32(h + AVI_OFF_WIDTH));
  TEST_ASSERT_EQUAL_UINT32(600,    rd32(h + AVI_OFF_HEIGHT));
  TEST_ASSERT_EQUAL_UINT32(1,      rd32(h + 128));               // dwScale
  TEST_ASSERT_EQUAL_UINT32(10,     rd32(h + AVI_OFF_STRH_RATE));
  TEST_ASSERT_EQUAL_UINT32(40,     rd32(h + 172));               // biSize
  TEST_ASSERT_EQUAL_UINT32(800*600*3, rd32(h + 192));            // biSizeImage

  aviBuildHeader(h, 640, 480, 0);                                // fps 0 must not divide by zero
  TEST_ASSERT_EQUAL_UINT32(1000000, rd32(h + AVI_OFF_USPF));
}

static void test_patch(void) {
  uint8_t h[AVI_HDR_BYTES];
  aviBuildHeader(h, 800, 600, 10);

  const uint32_t frames = 3000, movi = 60000000, idxBytes = frames * AVI_IDX_ENTRY_BYTES;
  aviPatchHeader(h, frames, movi, idxBytes);

  uint32_t fileSize = AVI_HDR_BYTES + movi + 8 + idxBytes;
  TEST_ASSERT_EQUAL_UINT32(fileSize - 8, rd32(h + AVI_OFF_RIFFSIZE));
  TEST_ASSERT_EQUAL_UINT32(frames,       rd32(h + AVI_OFF_TOTALFRAMES));
  TEST_ASSERT_EQUAL_UINT32(frames,       rd32(h + AVI_OFF_STRH_LENGTH));
  TEST_ASSERT_EQUAL_UINT32(4 + movi,     rd32(h + AVI_OFF_MOVISIZE));

  aviPatchHeader(h, 0, 0, 0);                                    // empty segment, no idx1
  TEST_ASSERT_EQUAL_UINT32(AVI_HDR_BYTES - 8, rd32(h + AVI_OFF_RIFFSIZE));
  TEST_ASSERT_EQUAL_UINT32(4, rd32(h + AVI_OFF_MOVISIZE));
}

static void test_chunk_padding(void) {
  TEST_ASSERT_EQUAL_UINT32(8 + 100,     aviChunkBytes(100));     // even payload
  TEST_ASSERT_EQUAL_UINT32(8 + 101 + 1, aviChunkBytes(101));     // odd payload gets a pad byte
}

// The one that actually matters: every idx1 offset must land on a '00dc' header
// inside the movi list, or VLC seeks into garbage.
static void test_index_offsets_resolve(void) {
  static uint8_t file[4096];
  const uint32_t lens[4] = { 10, 11, 64, 3 };                    // mixed odd/even on purpose
  uint8_t idx[4 * AVI_IDX_ENTRY_BYTES];

  aviBuildHeader(file, 320, 240, 10);
  uint32_t movi = 0;
  for (int i = 0; i < 4; i++) {
    uint8_t* p = file + AVI_HDR_BYTES + movi;
    memcpy(p, "00dc", 4);
    wr32(p + 4, lens[i]);
    memset(p + 8, 0xAB, lens[i]);
    if (lens[i] & 1) p[8 + lens[i]] = 0;
    aviIndexEntry(idx + i * AVI_IDX_ENTRY_BYTES, movi + AVI_MOVI_FIRST_OFF, lens[i]);
    movi += aviChunkBytes(lens[i]);
  }

  // 'movi' FOURCC sits at 220, so file offset = 220 + entry offset.
  for (int i = 0; i < 4; i++) {
    const uint8_t* e = idx + i * AVI_IDX_ENTRY_BYTES;
    TEST_ASSERT_EQUAL_MEMORY("00dc", e, 4);
    TEST_ASSERT_EQUAL_UINT32(0x10, rd32(e + 4));                 // keyframe flag
    TEST_ASSERT_EQUAL_UINT32(lens[i], rd32(e + 12));
    const uint8_t* target = file + 220 + rd32(e + 8);
    TEST_ASSERT_EQUAL_MEMORY("00dc", target, 4);
    TEST_ASSERT_EQUAL_UINT32(lens[i], rd32(target + 4));
  }
  TEST_ASSERT_EQUAL_UINT32(AVI_MOVI_FIRST_OFF, rd32(idx + 8));   // first chunk is at movi+4
}

// ---------------------------------------------------------------------------
// Segment paths
// ---------------------------------------------------------------------------
static void test_seg_path(void) {
  char p[64], d[9];
  const uint32_t t = 1700000000u;              // Tue 2023-11-14 22:13:20 UTC

  segPath(p, sizeof(p), t, 0);
  TEST_ASSERT_EQUAL_STRING(REC_DIR "/20231114/221320.avi", p);
  segDayStr(d, t, 0);
  TEST_ASSERT_EQUAL_STRING("20231114", d);

  segPath(p, sizeof(p), t, 330);               // +05:30 rolls over midnight
  TEST_ASSERT_EQUAL_STRING(REC_DIR "/20231115/034320.avi", p);
  segDayStr(d, t, 330);
  TEST_ASSERT_EQUAL_STRING("20231115", d);

  segPath(p, sizeof(p), t, -480);              // -08:00 stays on the 14th
  TEST_ASSERT_EQUAL_STRING(REC_DIR "/20231114/141320.avi", p);
}

static void test_seg_epoch_roundtrip(void) {
  TEST_ASSERT_EQUAL_UINT32(1700000000u, segEpoch("20231114", "221320.avi", 0));
  TEST_ASSERT_EQUAL_UINT32(1700000000u, segEpoch("20231115", "034320.avi", 330));
  TEST_ASSERT_EQUAL_UINT32(0u,          segEpoch("2023", "221320.avi", 0));   // junk -> 0
  TEST_ASSERT_EQUAL_UINT32(0u,          segEpoch("20231114", "22.avi", 0));

  for (uint32_t t = 1700000000u; t < 1700000000u + 3 * 86400u; t += 3617) {
    char p[64], day[9];
    segPath(p, sizeof(p), t, 330);
    segDayStr(day, t, 330);
    TEST_ASSERT_EQUAL_UINT32(t, segEpoch(day, p + strlen(REC_DIR) + 10, 330));
  }
}

// ---------------------------------------------------------------------------
// Retention
// ---------------------------------------------------------------------------
static void test_retention(void) {
  const char* days[] = { "20260801", "20260730", "20260802" };
  const uint64_t min = 512ull * 1024 * 1024;

  // Plenty of room -> never delete.
  TEST_ASSERT_EQUAL_INT(-1, retentionPickDay(min, min, days, 3, "20260802"));
  TEST_ASSERT_EQUAL_INT(-1, retentionPickDay(min * 4, min, days, 3, "20260802"));

  // Low -> oldest day wins regardless of scan order.
  TEST_ASSERT_EQUAL_INT(1, retentionPickDay(min - 1, min, days, 3, "20260802"));

  // The day being recorded into is never a candidate.
  const char* two[] = { "20260802", "20260803" };
  TEST_ASSERT_EQUAL_INT(1, retentionPickDay(0, min, two, 2, "20260802"));

  // Only the current day exists -> nothing safe to delete, caller keeps recording.
  const char* one[] = { "20260802" };
  TEST_ASSERT_EQUAL_INT(-1, retentionPickDay(0, min, one, 1, "20260802"));
  TEST_ASSERT_EQUAL_INT(-1, retentionPickDay(0, min, one, 0, "20260802"));

  // No current day (SD mounted before the clock synced) -> oldest still wins.
  TEST_ASSERT_EQUAL_INT(1, retentionPickDay(0, min, days, 3, ""));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_header_shape);
  RUN_TEST(test_header_values);
  RUN_TEST(test_patch);
  RUN_TEST(test_chunk_padding);
  RUN_TEST(test_index_offsets_resolve);
  RUN_TEST(test_seg_path);
  RUN_TEST(test_seg_epoch_roundtrip);
  RUN_TEST(test_retention);
  return UNITY_END();
}
