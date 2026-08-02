// Native unit tests for the pure logic in firmware/src/display.h (A4).
// No Arduino, no LovyanGFX, no hardware — everything here is plain types.
//   pio test -e native -f test_display
#include <unity.h>
#include <string.h>
#include "display.h"

// Touch cfg matching config.h at time of writing; the tests exercise the state
// machine, not the tuning, so the numbers are local on purpose.
static const float    DELTA_PCT = 8.0f;
static const uint32_t TAU_MS    = 5000;
static const uint32_t POLL_MS   = 50;
static const uint32_t HOLD_MS   = 2000;
static const uint32_t DEB_MS    = 250;

static const uint32_t IDLE   = 10000;   // representative touchRead() baseline
static const uint32_t PRESS  = 12000;   // +20% -> comfortably over DELTA_PCT

static TouchCfg cfg() { return touchMakeCfg(DELTA_PCT, TAU_MS, POLL_MS, HOLD_MS, DEB_MS); }

// Feed samples from t0 to t0+durMs at POLL_MS, counting emitted events.
struct Counts { int taps, holds; };
static void feedFor(TouchSm& s, const TouchCfg& c, uint32_t& t, uint32_t durMs,
                    uint32_t raw, Counts& n) {
  for (uint32_t e = t + durMs; t < e; t += POLL_MS) {
    switch (touchFeed(s, c, t, raw)) {
      case TOUCH_TAP:  n.taps++;  break;
      case TOUCH_HOLD: n.holds++; break;
      default: break;
    }
  }
}

void setUp() {}
void tearDown() {}

// ------------------------------------------------------------ screen cycling
static void test_screen_cycles_and_wraps() {
  int s = SCREEN_CLOCK;
  s = screenNext(s, SCREEN_COUNT); TEST_ASSERT_EQUAL_INT(SCREEN_STATUS, s);
  s = screenNext(s, SCREEN_COUNT); TEST_ASSERT_EQUAL_INT(SCREEN_AI, s);
  s = screenNext(s, SCREEN_COUNT); TEST_ASSERT_EQUAL_INT(SCREEN_CAM, s);
  s = screenNext(s, SCREEN_COUNT); TEST_ASSERT_EQUAL_INT(SCREEN_CLOCK, s);   // wraps
  TEST_ASSERT_EQUAL_INT(0, screenNext(99, SCREEN_COUNT));                    // junk -> screen 0
  TEST_ASSERT_EQUAL_INT(0, screenNext(0, 0));                                // no divide by zero
}

// ------------------------------------------------------------ touch
static void test_one_tap_advances_exactly_one_screen() {
  TouchSm s; touchReset(s); TouchCfg c = cfg();
  uint32_t t = 0; Counts n = {0, 0};
  feedFor(s, c, t, 1000, IDLE,  n);   // settle baseline
  feedFor(s, c, t,  300, PRESS, n);   // finger down 300 ms
  feedFor(s, c, t, 1000, IDLE,  n);   // and up
  TEST_ASSERT_EQUAL_INT(1, n.taps);
  TEST_ASSERT_EQUAL_INT(0, n.holds);

  int scr = SCREEN_CLOCK;
  for (int i = 0; i < n.taps; i++) scr = screenNext(scr, SCREEN_COUNT);
  TEST_ASSERT_EQUAL_INT(SCREEN_STATUS, scr);      // exactly one advance
}

static void test_contact_bounce_is_one_tap_not_two() {
  TouchSm s; touchReset(s); TouchCfg c = cfg();
  uint32_t t = 0; Counts n = {0, 0};
  feedFor(s, c, t, 1000, IDLE,  n);
  feedFor(s, c, t,  100, PRESS, n);   // press
  feedFor(s, c, t,   50, IDLE,  n);   // bounce release
  feedFor(s, c, t,  100, PRESS, n);   // bounce re-press
  feedFor(s, c, t,   50, IDLE,  n);   // bounce release
  feedFor(s, c, t,  100, PRESS, n);   // bounce re-press
  feedFor(s, c, t, 1000, IDLE,  n);   // final release
  TEST_ASSERT_EQUAL_INT(1, n.taps);
  TEST_ASSERT_EQUAL_INT(0, n.holds);
}

static void test_two_deliberate_taps_are_two_taps() {
  TouchSm s; touchReset(s); TouchCfg c = cfg();
  uint32_t t = 0; Counts n = {0, 0};
  feedFor(s, c, t, 1000, IDLE,  n);
  feedFor(s, c, t,  200, PRESS, n);
  feedFor(s, c, t,  600, IDLE,  n);   // well past TOUCH_DEBOUNCE_MS
  feedFor(s, c, t,  200, PRESS, n);
  feedFor(s, c, t,  600, IDLE,  n);
  TEST_ASSERT_EQUAL_INT(2, n.taps);
}

static void test_hold_fires_once_and_emits_no_tap() {
  TouchSm s; touchReset(s); TouchCfg c = cfg();
  uint32_t t = 0; Counts n = {0, 0};
  feedFor(s, c, t, 1000, IDLE,  n);
  feedFor(s, c, t, 3000, PRESS, n);   // 3 s hold, well past HOLD_MS
  TEST_ASSERT_EQUAL_INT(1, n.holds);  // fires once mid-press, not repeatedly
  TEST_ASSERT_EQUAL_INT(0, n.taps);
  feedFor(s, c, t, 1000, IDLE,  n);   // release
  TEST_ASSERT_EQUAL_INT(0, n.taps);   // release after a hold must NOT also tap
  TEST_ASSERT_EQUAL_INT(1, n.holds);
}

static void test_hold_release_bounce_emits_nothing() {
  TouchSm s; touchReset(s); TouchCfg c = cfg();
  uint32_t t = 0; Counts n = {0, 0};
  feedFor(s, c, t, 1000, IDLE,  n);
  feedFor(s, c, t, 2500, PRESS, n);
  feedFor(s, c, t,   50, IDLE,  n);   // release
  feedFor(s, c, t,   50, PRESS, n);   // bounce
  feedFor(s, c, t, 1000, IDLE,  n);
  TEST_ASSERT_EQUAL_INT(1, n.holds);
  TEST_ASSERT_EQUAL_INT(0, n.taps);
}

static void test_baseline_drift_does_not_false_trigger() {
  TouchSm s; touchReset(s); TouchCfg c = cfg();
  uint32_t t = 0; Counts n = {0, 0};
  // Untouched reading drifts 10000 -> 14000 over ~5 min (humidity/temperature).
  for (uint32_t i = 0; i < 6000; i++, t += POLL_MS) {
    const uint32_t raw = IDLE + (4000u * i) / 6000u;
    switch (touchFeed(s, c, t, raw)) {
      case TOUCH_TAP:  n.taps++;  break;
      case TOUCH_HOLD: n.holds++; break;
      default: break;
    }
  }
  TEST_ASSERT_EQUAL_INT(0, n.taps);
  TEST_ASSERT_EQUAL_INT(0, n.holds);
  // ...and a real tap on top of the drifted baseline still registers.
  feedFor(s, c, t, 300, (uint32_t)(14000 * 1.2f), n);
  feedFor(s, c, t, 600, 14000, n);
  TEST_ASSERT_EQUAL_INT(1, n.taps);
}

// ------------------------------------------------------------ word wrap
static void test_wrap_breaks_on_spaces() {
  char out[4][32];
  int n = textWrap("the quick brown fox jumps", 10, 4, &out[0][0], 32);
  TEST_ASSERT_EQUAL_INT(3, n);
  TEST_ASSERT_EQUAL_STRING("the quick", out[0]);
  TEST_ASSERT_EQUAL_STRING("brown fox", out[1]);
  TEST_ASSERT_EQUAL_STRING("jumps", out[2]);
}

static void test_wrap_truncates_with_ellipsis() {
  char out[2][32];
  int n = textWrap("the quick brown fox jumps over the lazy dog", 10, 2, &out[0][0], 32);
  TEST_ASSERT_EQUAL_INT(2, n);
  TEST_ASSERT_EQUAL_STRING("the quick", out[0]);
  TEST_ASSERT_EQUAL_STRING("brown f...", out[1]);
  TEST_ASSERT_TRUE(strlen(out[1]) <= 10);
}

static void test_wrap_hard_splits_long_word() {
  char out[3][32];
  int n = textWrap("supercalifragilistic", 8, 3, &out[0][0], 32);
  TEST_ASSERT_EQUAL_INT(3, n);
  TEST_ASSERT_EQUAL_STRING("supercal", out[0]);
  TEST_ASSERT_EQUAL_STRING("ifragili", out[1]);
  TEST_ASSERT_EQUAL_STRING("stic", out[2]);
}

static void test_wrap_edges() {
  char out[3][32];
  TEST_ASSERT_EQUAL_INT(0, textWrap("", 10, 3, &out[0][0], 32));
  TEST_ASSERT_EQUAL_INT(0, textWrap(nullptr, 10, 3, &out[0][0], 32)); // null in, no crash
  TEST_ASSERT_EQUAL_INT(0, textWrap("hi", 0, 3, &out[0][0], 32));
  TEST_ASSERT_EQUAL_INT(0, textWrap("hi", 10, 0, &out[0][0], 32));
  TEST_ASSERT_EQUAL_INT(1, textWrap("   hi   ", 10, 3, &out[0][0], 32));
  TEST_ASSERT_EQUAL_STRING("hi", out[0]);
  int n = textWrap("line one\nline two", 20, 3, &out[0][0], 32);      // honours newlines
  TEST_ASSERT_EQUAL_INT(2, n);
  TEST_ASSERT_EQUAL_STRING("line one", out[0]);
  TEST_ASSERT_EQUAL_STRING("line two", out[1]);
}

// ------------------------------------------------------------ formatters
static void test_fmt_uptime() {
  char b[32];
  fmtUptime(0, b, sizeof(b));            TEST_ASSERT_EQUAL_STRING("00:00:00", b);
  fmtUptime(59, b, sizeof(b));           TEST_ASSERT_EQUAL_STRING("00:00:59", b);
  fmtUptime(3661, b, sizeof(b));         TEST_ASSERT_EQUAL_STRING("01:01:01", b);
  fmtUptime(86399, b, sizeof(b));        TEST_ASSERT_EQUAL_STRING("23:59:59", b);
  fmtUptime(86400, b, sizeof(b));        TEST_ASSERT_EQUAL_STRING("1d 00:00", b);
  fmtUptime(3u*86400+4*3600+5*60, b, sizeof(b)); TEST_ASSERT_EQUAL_STRING("3d 04:05", b);
}

static void test_pct_used() {
  TEST_ASSERT_EQUAL_INT(-1,  pctUsed(0, 0));         // SD not mounted -> caller prints "--"
  TEST_ASSERT_EQUAL_INT(0,   pctUsed(0, 1024));
  TEST_ASSERT_EQUAL_INT(50,  pctUsed(512, 1024));
  TEST_ASSERT_EQUAL_INT(33,  pctUsed(1, 3));
  TEST_ASSERT_EQUAL_INT(100, pctUsed(1024, 1024));
  TEST_ASSERT_EQUAL_INT(100, pctUsed(9999, 1024));   // clamped, never 976%
  TEST_ASSERT_EQUAL_INT(50,  pctUsed(64ull*1024*1024*1024, 128ull*1024*1024*1024));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_screen_cycles_and_wraps);
  RUN_TEST(test_one_tap_advances_exactly_one_screen);
  RUN_TEST(test_contact_bounce_is_one_tap_not_two);
  RUN_TEST(test_two_deliberate_taps_are_two_taps);
  RUN_TEST(test_hold_fires_once_and_emits_no_tap);
  RUN_TEST(test_hold_release_bounce_emits_nothing);
  RUN_TEST(test_baseline_drift_does_not_false_trigger);
  RUN_TEST(test_wrap_breaks_on_spaces);
  RUN_TEST(test_wrap_truncates_with_ellipsis);
  RUN_TEST(test_wrap_hard_splits_long_word);
  RUN_TEST(test_wrap_edges);
  RUN_TEST(test_fmt_uptime);
  RUN_TEST(test_pct_used);
  return UNITY_END();
}
