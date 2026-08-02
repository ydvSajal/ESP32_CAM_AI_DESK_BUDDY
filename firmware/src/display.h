#pragma once
// display.h — Owner: A4. Runs on CORE_SERVICES (core 1), uiTask prio 3.
// LovyanGFX ST7789, redraw at DISPLAY_REDRAW_MS, touch poll at TOUCH_POLL_MS.
// Screens + touch behaviour per HANDOVER.md section 7. No LVGL.
//
// Everything below the "pure logic" line is plain C++ on plain types so the
// native unit tests (/test/firmware/test_display) can include this header
// without Arduino, LovyanGFX, or hardware.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

enum Screen { SCREEN_CLOCK, SCREEN_STATUS, SCREEN_AI, SCREEN_CAM, SCREEN_COUNT };

#ifdef ARDUINO
#include <Arduino.h>

extern "C" void uiTask(void* pv);

void displayForceStatus(uint32_t ms);   // boot shows the URL screen for BOOT_STATUS_SCREEN_MS
void displaySetAiText(const String& question, const String& answer);
#endif

// ===================== pure logic (native-testable) =====================

// ---- screen cycling ----
inline int screenNext(int cur, int count) {
  if (count <= 0) return 0;
  if (cur < 0 || cur >= count) return 0;
  return (cur + 1) % count;
}

// ---- touch state machine ----
// Capacitive baseline drifts with temperature/humidity, so no raw threshold:
// track a slow EMA of the released-state reading and trigger on a relative rise.
// (ESP32-S3 touchRead() goes UP when touched — opposite of the S2/classic.)
enum TouchEvent { TOUCH_NONE = 0, TOUCH_TAP, TOUCH_HOLD };

struct TouchCfg {
  float    deltaPct;    // % rise over baseline that counts as pressed
  float    alpha;       // per-sample EMA weight for the baseline
  uint32_t holdMs;
  uint32_t debounceMs;
};

struct TouchSm {
  float    baseline;
  uint32_t downAt;
  uint32_t lastEventAt;
  bool     primed;
  bool     down;
  bool     holdFired;
  bool     fired;       // any event emitted yet (so the first tap isn't eaten)
};

inline TouchCfg touchMakeCfg(float deltaPct, uint32_t baselineTauMs,
                             uint32_t pollMs, uint32_t holdMs, uint32_t debounceMs) {
  TouchCfg c;
  c.deltaPct   = deltaPct;
  c.alpha      = baselineTauMs ? (float)pollMs / (float)baselineTauMs : 1.0f;
  if (c.alpha > 1.0f) c.alpha = 1.0f;
  c.holdMs     = holdMs;
  c.debounceMs = debounceMs;
  return c;
}

inline void touchReset(TouchSm& s) { memset(&s, 0, sizeof(s)); }

// Feed one (timestamp, raw touchRead) sample. Returns the event emitted, if any.
inline TouchEvent touchFeed(TouchSm& s, const TouchCfg& c, uint32_t now, uint32_t raw) {
  const float r = (float)raw;
  if (!s.primed) { s.baseline = r; s.primed = true; return TOUCH_NONE; }

  const bool pressed = r > s.baseline * (1.0f + c.deltaPct / 100.0f);
  if (!pressed) s.baseline += c.alpha * (r - s.baseline);   // baseline only tracks the released state

  if (pressed) {
    if (!s.down) { s.down = true; s.downAt = now; s.holdFired = false; }
    else if (!s.holdFired && (uint32_t)(now - s.downAt) >= c.holdMs) {
      s.holdFired = true; s.fired = true; s.lastEventAt = now;
      return TOUCH_HOLD;                      // fires once, mid-press
    }
    return TOUCH_NONE;
  }

  if (s.down) {                               // release edge — the only place a tap is emitted
    s.down = false;
    const bool tap = !s.holdFired &&          // a hold must NOT also fire a tap
                     !(s.fired && (uint32_t)(now - s.lastEventAt) < c.debounceMs);
    s.fired = true; s.lastEventAt = now;      // also swallows contact bounce after a hold
    return tap ? TOUCH_TAP : TOUCH_NONE;
  }
  return TOUCH_NONE;
}

// ---- word wrap / truncate ----
// Greedy wrap of `s` into `maxLines` NUL-terminated lines of <= `cols` chars,
// written at out[line*stride]. Long words are hard-split. If text is left over,
// the last line is ellipsized. Returns the number of lines written.
inline int textWrap(const char* s, int cols, int maxLines, char* out, int stride) {
  if (!out || cols <= 0 || maxLines <= 0 || stride <= 1) return 0;
  if (!s) s = "";
  const int cap = cols < stride - 1 ? cols : stride - 1;
  int lines = 0, i = 0;

  while (lines < maxLines) {
    while (s[i] == ' ') i++;
    if (!s[i]) break;

    int take = 0, lastSpace = -1;
    while (s[i + take] && s[i + take] != '\n' && take < cap) {
      if (s[i + take] == ' ') lastSpace = take;
      take++;
    }
    int brk = take;
    const char nextCh = s[i + take];
    if (take == cap && nextCh && nextCh != '\n' && nextCh != ' ' && lastSpace > 0) brk = lastSpace;

    char* dst = out + lines * stride;
    memcpy(dst, s + i, (size_t)brk);
    dst[brk] = 0;
    while (brk > 0 && dst[brk - 1] == ' ') dst[--brk] = 0;
    lines++;
    i += brk;
    if (s[i] == '\n') i++;
  }

  int j = i;
  while (s[j] == ' ' || s[j] == '\n') j++;
  if (s[j] && lines > 0 && cap >= 3) {          // leftover text -> ellipsize the last line
    char* last = out + (lines - 1) * stride;
    int L = (int)strlen(last);
    if (L > cap - 3) L = cap - 3;
    last[L] = 0;
    memcpy(last + L, "...", 4);
  }
  return lines;
}

// ---- formatters ----
inline void fmtUptime(uint32_t sec, char* out, int n) {
  if (!out || n <= 0) return;
  const uint32_t d = sec / 86400u, h = (sec / 3600u) % 24u, m = (sec / 60u) % 60u, ss = sec % 60u;
  if (d) snprintf(out, (size_t)n, "%ud %02u:%02u", (unsigned)d, (unsigned)h, (unsigned)m);
  else   snprintf(out, (size_t)n, "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)ss);
}

// Rounded percent used. Returns -1 when total is unknown (SD not mounted) so the
// caller can print a dash instead of a lie.
inline int pctUsed(uint64_t used, uint64_t total) {
  if (!total) return -1;
  if (used > total) used = total;
  return (int)((used * 100u + total / 2u) / total);
}
