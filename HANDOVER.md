# HANDOVER — ESP32-S3 Desk Buddy + 24/7 Surveillance

**Audience:** Opus orchestrator. You spawn sub-agents per the Work Split section. Lower-capability agents implement; this file is the contract. Do not redesign architecture — implement it.

---

## 1. Product summary

One ESP32-S3 device that is simultaneously:

1. **Surveillance unit** — camera records 24/7 to SD card (MJPEG-in-AVI segments), mic optional audio, segments upload to Google Drive when Wi-Fi idle allows.
2. **Desk buddy** — TFT display shows clock, date, Wi-Fi status, device URL, and rotating info screens. Capacitive touch pin cycles screens.
3. **AI assistant** — user asks questions from the web app; device (or thin proxy) calls OpenRouter or Gemini with user-supplied API keys, answer shown in web app and optionally summarized on display.
4. **Web-controlled** — PWA (installable) shows live snapshot/stream, recording status, SD usage, upload queue, AI chat, settings. Device prints its URL on the display at every boot.

## 2. Hardware assumptions (agent: verify against actual board first)

| Part | Assumption | Config location |
|---|---|---|
| Board | ESP32-S3 with PSRAM (8MB) | `firmware/src/config.h` |
| Camera | OV2640, DVP interface | `config.h` pin block |
| Mic | PDM or I2S MEMS | `config.h` |
| Display | SPI TFT (ST7789/ILI9341 class), 240x240 or 240x320 | `config.h` |
| SD | SDMMC or SPI SD slot | `config.h` |
| Touch | 1 GPIO, ESP32-S3 native `touchRead()` | `config.h` |

**Rule:** every pin, resolution, and peripheral choice lives ONLY in `config.h`. Nothing hardcoded elsewhere. First implementation task is confirming the real board and filling this file.

## 3. Tech stack (slim — do not add to it)

- **Firmware:** PlatformIO + Arduino framework (`espressif32`). Libraries: `esp32-camera` (bundled), `LovyanGFX` (display), ESP-IDF FreeRTOS APIs (bundled), `ESPAsyncWebServer` + `AsyncTCP`. No LVGL, no RTOS abstractions, no OTA framework in v1.
- **Web app:** ONE static PWA — vanilla HTML/CSS/JS, no framework, no build step. `manifest.json` + tiny service worker for installability. **Served from the device's own flash (LittleFS) at `/`.** Vercel hosting is optional and secondary.

  > **Revised after A3 (2026-08-02).** The original plan hosted the PWA on Vercel. That cannot work: Vercel serves HTTPS, the device serves plain HTTP on the LAN, and every browser blocks HTTPS→HTTP as mixed content. The user would have to disable a browser security setting to use their own camera. Serving the app from the device makes it same-origin, kills the mixed-content problem and the CORS layer, needs no internet at all, and makes the URL shown on the display actually open the app — which is what the product promised. The app is a few KB of static files; flash is not the constraint. Vercel stays documented in `docs/DEPLOY.md` as an optional convenience for anyone who accepts the browser flag.
- **Cloud:** Google Drive upload direct from device via Drive REST API v3 (resumable upload) with OAuth2 refresh token. NO custom backend server in v1. Vercel hosts static PWA only.
- **Secrets:** Wi-Fi creds, OpenRouter key, Gemini key, Drive refresh token — stored in NVS, entered once via device's setup page. Never committed, never baked into firmware.

**Why no backend:** device is on LAN with the user. The PWA and every API live on the device's own HTTP server, same origin. Remote access outside LAN is out of scope v1 (note in README: Tailscale on router is user's escape hatch).

**Auth on browser-native URLs.** `GET /api/snapshot`, `/api/stream`, and `/api/recordings/file` are loaded by `<img src>` and `<a download>`, which cannot send a custom header. These three accept `?pin=` as an alternative to `X-Device-Pin` (same as `/ws` already does). Without this the app has to fetch-and-buffer a whole 20 MB segment in RAM to attach a header. Every other endpoint stays header-only.

## 4. System design — dual core allocation (fixed, do not deviate)

```
Core 0 (PRO_CPU)                    Core 1 (APP_CPU)
─────────────────                   ─────────────────
camTask   (prio 5)                  netTask: AsyncWebServer + WS (event-driven)
  frame grab -> ring buffer         uploadTask (prio 2)
sdTask    (prio 4)                    closed segments -> Drive resumable upload
  ring buffer -> AVI segment          runs only when SD write load low
  on SD, rotate every 5 min         aiTask (prio 2)
                                      HTTPS call to OpenRouter/Gemini, queue depth 1
                                    uiTask (prio 3)
                                      display redraw 1 Hz + touch poll 20 Hz
```

- Frames flow through a **PSRAM ring buffer** (camTask producer, sdTask consumer, netTask snapshot-reader). One mutex, short holds.
- **Recording never yields to anything.** Upload and AI are best-effort; if heap/bandwidth tight they wait. Surveillance is priority 1 by definition.
- Live stream to PWA = MJPEG at reduced fps (5) reading same ring buffer; cap 1 concurrent stream client.
- Watchdog on camTask + sdTask; any stall > 10 s = log to SD + `esp_restart()`. 24/7 means auto-recovery, not uptime heroics.
- Segment files: `/rec/YYYYMMDD/HHMMSS.avi`, 5-min chunks. SD full policy: delete oldest day. Audio: record to companion `.wav` per segment in v1 (muxing audio into AVI is v2 — note as ponytail ceiling).

## 5. Repo structure (create exactly this)

```
/firmware
  platformio.ini
  src/
    config.h            # ALL pins, sizes, intervals, feature flags
    main.cpp            # setup: NVS load, tasks spawn; nothing else
    camera.cpp/.h       # camTask + ring buffer
    recorder.cpp/.h     # sdTask, AVI writer, rotation, retention
    uploader.cpp/.h     # uploadTask, Drive resumable upload
    ai.cpp/.h           # aiTask, OpenRouter + Gemini clients (same interface)
    webserver.cpp/.h    # routes + WS + captive setup portal
    display.cpp/.h      # uiTask, screens, touch handling
    storage.cpp/.h      # NVS settings struct load/save
/webapp
  index.html  app.js  style.css  manifest.json  sw.js
/docs
  API.md                # device HTTP/WS API contract (write FIRST — both sides code against it)
  SETUP.md              # flash, Drive OAuth walkthrough, first boot
/test
  TEST_PLAN.md          # section 8 expanded into runnable checklist
  firmware/             # PlatformIO native unit tests (pure logic only)
  webapp/test.html      # in-browser assertion page for app.js pure functions
```

## 6. Device API contract (freeze in docs/API.md before any implementation)

```
GET  /api/status        # uptime, heap, SD free/used, recording state, upload queue, wifi RSSI, time
GET  /api/snapshot      # single JPEG
GET  /api/stream        # MJPEG multipart, max 1 client
POST /api/ai            # {prompt, provider:"openrouter"|"gemini"} -> {answer}  (chunked/SSE if easy, else blocking)
GET  /api/recordings    # list days/segments
POST /api/settings      # wifi, keys, intervals; device applies + persists NVS
GET  /api/settings      # current (keys masked)
POST /api/reboot
WS   /ws                # push: status every 5 s, ai answer chunks, upload events
```

Setup mode: no Wi-Fi creds in NVS → SoftAP `deskbuddy-setup` + captive portal serving minimal settings form.

## 7. Display screens (touch cycles: tap = next screen, hold 2 s = force status screen)

1. **Clock** — big time, date, Wi-Fi icon, REC dot.
2. **Status** — device URL (`http://<ip>`), SSID, SD %, upload queue count. *Shown automatically for 30 s at every boot — this is the "URL on screen" requirement.*
3. **AI** — last question + answer (scrolling/truncated).
4. **Cam** — recording stats: segment count today, fps, last upload time.

## 8. Test requirements (acceptance — lower models implement, these define "done")

Expand into `/test/TEST_PLAN.md` with pass/fail steps. A phase is not complete until its tests pass on hardware.

**T1 Firmware boot & config**
- Fresh flash, empty NVS → SoftAP setup portal appears, form saves, device reboots onto Wi-Fi.
- Boot with creds → connected < 15 s, URL screen shown 30 s.

**T2 Recording (critical path)**
- Records continuously 24 h: no gaps > 2 s between segments, all AVIs playable in VLC.
- SD filled artificially → oldest day deleted, recording continues.
- Pull power mid-segment → reboot recovers, prior segments intact, at most last partial segment lost.
- camTask starved (cover lens, heavy web load) → watchdog restarts, recording resumes < 30 s.

**T3 Upload**
- Segment closes → appears in correct Drive folder (`DeskBuddy/YYYYMMDD/`) within 10 min on idle Wi-Fi.
- Wi-Fi cut 1 h → queue persists, drains on reconnect, no duplicates.
- Upload running → recording fps unchanged (measure).

**T4 Web app**
- PWA installs on Android + desktop Chrome; offline open shows shell + "device unreachable".
- Status updates via WS every 5 s; snapshot < 2 s; stream ≥ 3 fps while recording continues.
- Settings round-trip: save keys, reboot, keys still active, GET shows masked.

**T5 AI**
- Prompt via PWA → answer < 20 s (network permitting) from each provider; provider switch works.
- Bad/missing key → clean error in UI, no crash, no reboot.
- AI request during recording → zero dropped segments.

**T6 Display/touch**
- Tap advances screen every time (debounced, no double-fire); hold 2 s → status screen.
- Clock drifts < 2 s/day (NTP resync hourly).

**T7 Soak**
- 72 h continuous: no reboot loops (watchdog restarts logged + counted, < 3/day), heap floor stable (no leak trend), all footage present.

**T8 Security minimum**
- Settings/API endpoints require the device PIN (set during setup) via simple token header. Keys never appear in logs, serial output, or unmasked API responses.

## 9. Work split — Opus spawns these agents

Order matters: A0 → (A1 ∥ A2 ∥ A3) → A4. Contract for parallel work = `docs/API.md` + `config.h`, written in A0 and then frozen.

- **A0 Foundation (do first, sequential):** confirm real board + pins, write `config.h`, `platformio.ini`, `docs/API.md`, `storage.cpp`, `main.cpp` skeleton with all tasks stubbed and core-pinned. Boots, connects Wi-Fi, serves `/api/status` stub.
- **A1 Firmware core (Core 0 domain):** `camera.cpp`, `recorder.cpp` + T2 tests. Hardest agent — give it the strongest model available.
- **A2 Firmware services (Core 1 domain):** `webserver.cpp`, `uploader.cpp`, `ai.cpp` + T3/T5/T8 tests.
- **A3 Frontend:** `/webapp` PWA against `docs/API.md` (mock device with a 30-line local mock server if hardware busy) + T4 tests. Deploy static to Vercel.
- **A4 Display UI:** `display.cpp` screens + touch + T6. Small task, cheap model fine.
- **Integration (Opus itself):** merge, run T1/T7 soak, write `SETUP.md` incl. Google Drive OAuth walkthrough (user creates GCP project, gets refresh token via provided helper script — put a small `docs/get_drive_token.md` with the curl steps).

Per-agent rules: touch only your files; API/config changes require updating `docs/API.md` in same commit and flagging Opus; every module leaves one runnable check (unit test for pure logic, documented hardware test step otherwise).

## 10. Explicit non-goals (v1 — do not build)

- No remote access outside LAN (no tunnels, no relay backend).
- No motion detection, no face/object recognition on-device.
- No audio muxed into video (separate .wav; mux is v2).
- No OTA updates, no multi-device support, no user accounts.
- No framework on the web app. No LVGL on device.

## 11. Open items Opus must resolve with user before A1 starts

1. Exact board model + pinout (photo or product link).
2. Display model + resolution.
3. Google account ready for a GCP project (Drive API needs it).
4. Target video resolution/fps (default: 800x600 @ 10–12 fps, JPEG q=12 — safe for SD write + PSRAM).

---

## 12. Integration log (2026-08-02)

Sections 1–11 are the contract and were not otherwise altered. Two design revisions are recorded
inline above (section 3 hosting, section 3 `?pin=`); everything else below is implementation.

### Seams closed between agents

| # | Problem | Resolution |
|---|---|---|
| F1 | Vercel-hosted HTTPS PWA cannot call the device's HTTP LAN address (mixed content) | PWA ships in device flash. `data_dir = ../webapp`, `LittleFS.begin()` + `serveStatic("/")` registered last so `/api/*` wins; `app.js` defaults `base` to `location.origin` on an `http:` page. `DEPLOY.md` rewritten device-first, `SETUP.md` §2/§6 and `README.md` carry the `uploadfs` step |
| F2 | Header-only PIN broke `<img src>` / `<a download>` | `?pin=` accepted on `/api/snapshot`, `/api/stream`, `/api/recordings/file` only (`authed(r, true)`). A3's manual MJPEG parse and fetch-to-blob download deleted — the browser does both natively now. `mock-server.js` matches |
| F3 | `stream:false` parks the single AsyncTCP task for up to `AI_TIMEOUT_MS` | PWA always sends `stream:true` and refuses to ask without a live WS. Blocking mode kept for `curl`, hazard documented at the handler |
| F4 | `API.md` epoch/ISO mismatch; masking rule vs example | Both fixed; `maskSecret()` masks a short PIN entirely (a 4–6 char PIN has no safe tail) |
| F5 | Two upload-marker schemes (A1 `.pending` sidecar, A2 `.up`/`.rsm`) | A1's sidecar is the single authority. A2's macros deleted |
| F6 | Uploader could compete with the recorder for the SD card | `recorderWriteLoadPct()` gates it against `UPLOAD_MAX_SD_LOAD_PCT` (50) alongside heap and Wi-Fi |
| — | `pio test` found no tests (`test_dir` defaulted to `firmware/test`) | `[platformio] test_dir = ../test/firmware` |
| — | Boot URL screen could flash the clock first | `displayForceStatus()` moved above the task-spawn block in `main.cpp` |

### Consistency audit

Zero duplicate `#define` in `config.h` after three agents appended blocks independently. No
cross-module call without a header declaration. `main.cpp` stacks/priorities/cores match section 4
exactly. `esp_task_wdt_reset()` present in both the camTask and sdTask loops. `ENABLE_AI` /
`ENABLE_UPLOAD` off-branches provide every symbol the on-branch does.

### What actually ran (observed, not asserted)

| Suite | Result |
|---|---|
| `test_recorder` (native, gcc 16.1.0) | **8 tests, 0 failures** |
| `test_services` (native) | **11 tests, 0 failures** |
| `test_display` (native) | **13 tests, 0 failures** |
| `test/webapp` pure functions (node) | **ALL 63 TESTS PASSED** |
| Live `mock-server.js` auth matrix | snapshot: no pin 401, `?pin=` 200, wrong `?pin=` 401, header 200; `/api/settings?pin=` **401** (header-only, as designed); `/api/recordings/file?pin=` 200; `/api/status` 200 unauthenticated; `/` serves the app; `/api/stream?pin=` streamed 4464 bytes of MJPEG |

### Real ESP32-S3 build (2026-08-02, update)

Python 3.12 + PlatformIO 6.1.19 installed on the dev machine; `pio run -e seeed_xiao_esp32s3`
(compile-only, no board attached, no upload) against the actual `espressif32` platform:

```
RAM:   18.9% (61980 / 327680 bytes)
Flash: 48.9% (1539193 / 3145728 bytes)
[SUCCESS] Took 102.05 seconds
```

Every module compiled and linked, including `camera.cpp` against the bundled esp32-camera
component. The ESPAsyncWebServer 3.x call signatures flagged as risk below all resolved cleanly
(`beginResponse`, `beginChunkedResponse`, `RESPONSE_TRY_AGAIN`, the `WS_EVT_CONNECT` arg cast) —
one harmless `send_P` deprecation note, no errors. This does not replace flashing: no hardware
has run any of this code, so camera/SD/display/touch pin correctness is still unverified.

### Known risk before the first real build (superseded above — kept for history)

Nothing had been compiled for the ESP32; only the pure-logic halves were built and run. The
guess was that ESPAsyncWebServer 3.x call signatures in `webserver.cpp` were the likely break
point — they were not.

Section 11 is still open and still needs the user: board confirmation, display model, GCP
project, target video res/fps. Flashing and every hardware-dependent T-row in TEST_PLAN.md are
still outstanding — they need the physical board.
