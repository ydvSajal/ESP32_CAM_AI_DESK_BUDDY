#pragma once
// uploader.h — Owner: A2. Runs on CORE_SERVICES (core 1), uploadTask prio 2.
// Closed segments -> Google Drive resumable upload. Best-effort: never blocks recording.
// Implement per HANDOVER.md section 4; emits upload_event over /ws (docs/API.md 4).
#include <string>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

#ifdef ARDUINO
#include <Arduino.h>

extern "C" void uploadTask(void* pv);

// There is no enqueue entry point: sdTask pushes closed segments onto recorder.h's
// recSegmentQueue and the ".pending" sidecar is the single source of truth for
// "not uploaded yet". uploadTask drains that queue and nothing else.
uint32_t uploadQueueDepth();
const char* uploadCurrentFile();        // "" when idle
uint8_t uploadProgressPct();
uint32_t uploadLastOk();                // epoch seconds, 0 = never
const char* uploadLastError();          // "" = none
#endif  // ARDUINO

// =====================================================================
//  Pure helpers — no Arduino, no hardware. Unit-tested in
//  test/firmware/test_services under [env:native]. Kept inline so the
//  native test can include this header with -I src and nothing else.
// =====================================================================

// "/rec/20260802/003000.avi" -> "20260802". "" when the path is not shaped like a segment.
inline std::string driveDayFromPath(const std::string& p) {
  size_t last = p.rfind('/');
  if (last == std::string::npos || last == 0) return "";
  size_t prev = p.rfind('/', last - 1);
  if (prev == std::string::npos) return "";
  std::string d = p.substr(prev + 1, last - prev - 1);
  if (d.size() != 8) return "";
  for (char c : d) if (c < '0' || c > '9') return "";
  return d;
}

// "/rec/20260802/003000.avi" -> "DeskBuddy/20260802" (the Drive folder chain to ensure).
inline std::string driveFolderPath(const std::string& segPath, const std::string& root = "DeskBuddy") {
  std::string d = driveDayFromPath(segPath);
  return d.empty() ? root : root + "/" + d;
}

// "/rec/20260802/003000.avi" -> "003000.avi"
inline std::string driveFileName(const std::string& p) {
  size_t last = p.rfind('/');
  return last == std::string::npos ? p : p.substr(last + 1);
}

// Exponential backoff, doubling from `base`, hard-capped at `cap`. attempt 0 => base.
inline uint32_t backoffMs(uint32_t attempt, uint32_t base = 5000, uint32_t cap = 300000) {
  if (attempt > 20) attempt = 20;
  uint64_t v = (uint64_t)base << attempt;
  return v > cap ? cap : (uint32_t)v;
}

// Drive's 308 reply carries `Range: bytes=0-262143`; the next byte we must send is 262144.
// Absent/blank header means Drive has nothing yet -> resume from 0.
inline uint64_t nextOffsetFromRange(const std::string& range) {
  size_t d = range.rfind('-');
  if (d == std::string::npos) return 0;
  return (uint64_t)strtoull(range.c_str() + d + 1, nullptr, 10) + 1;
}

// Content-Range for a resumable PUT: bytes <off>-<off+len-1>/<total>
inline std::string contentRange(uint64_t off, uint32_t len, uint64_t total) {
  char b[96];
  snprintf(b, sizeof b, "bytes %llu-%llu/%llu",
           (unsigned long long)off, (unsigned long long)(off + len - 1), (unsigned long long)total);
  return b;
}
