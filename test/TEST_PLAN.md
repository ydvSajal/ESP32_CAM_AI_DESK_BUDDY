# TEST_PLAN — ESP32-S3 Desk Buddy

HANDOVER.md section 8, expanded into runnable pass/fail steps.
Each agent owns its own section and appends it here. **Do not edit another agent's section.**

## How to run the automated parts

Only pure logic is automated. Everything with a camera, an SD card, a panel or a radio in it is
a hardware step below, done by a human with the board in front of them.

```bash
cd firmware
pio test -e native            # Unity tests in test/firmware/*  (test_dir = ../test/firmware)

node webapp/mock-server.js    # fake device on http://localhost:8080 implementing docs/API.md
# then open http://localhost:8080/test/webapp/test.html  -> header must read ALL <n> TESTS PASSED
```

`pio test -e native` builds `[env:native]` with host gcc — no board attached, no Arduino. It
covers `rec_pure.h` (AVI/rotation/retention math), the display state machine, and the service
helpers. If it cannot find any tests, `test_dir` in `firmware/platformio.ini` is wrong.

## Sections and what they prove

| Section | HANDOVER 8 requirement | Owner | Automated part |
|---|---|---|---|
| T1 | Firmware boot & config | integration | none — hardware only |
| T2 | Recording (critical path) | A1 | `test_recorder` |
| T3 | Upload | A2 | `test_services` |
| T4 | Web app | A3 | `test/webapp/test.html` |
| T5 | AI | A2 | `test_services` |
| T6 | Display/touch | A4 | `test_display` |
| T7 | 72 h soak | integration | none — hardware only |
| T8 | Security minimum | A2 | partial (`test_services` masking) |

**Release gate:** T1 and T2 must pass before the device is left running unattended. T7 is the
last gate — nothing ships until 72 h have actually elapsed on real hardware.

---

## T1 Firmware boot & config — owner integration

Proves the device comes up from nothing, provisions itself, and shows its URL. Everything here
is hardware. HANDOVER section 8 T1.

### T1.0 Pre-flight

| # | Step | Pass |
|---|---|---|
| 0.1 | Board matches `firmware/src/config.h` (pins, panel, SD, touch pad) | Every row of the config.h banner is confirmed against the real board |
| 0.2 | `pio run -t upload` then `pio run -t uploadfs` | Both succeed |
| 0.3 | `pio device monitor -b 115200` open before the first boot | Boot log visible from `[boot] deskbuddy` |

If 0.1 was skipped, stop. Wrong pins fail as camera-init errors, not as config errors, and you
will spend the day debugging the wrong module.

### T1.1 Empty NVS → SoftAP setup portal

| # | Step | Pass |
|---|---|---|
| 1.1 | Erase NVS: `pio run -t erase`, then reflash firmware **and** filesystem | Serial prints `[boot] setup mode: AP deskbuddy-setup at 192.168.4.1` |
| 1.2 | From a phone, join Wi-Fi `deskbuddy-setup` | Joins; captive-portal notification appears on Android/iOS |
| 1.3 | Open `http://192.168.4.1` (or accept the captive prompt) | The setup form renders — SSID, password, PIN, keys, tz |
| 1.4 | Any other path, e.g. `http://192.168.4.1/foo` | Redirects to `/` (probe handlers + `onNotFound`) |
| 1.5 | Submit with the SSID field empty | Stays on the form, error text, device does **not** reboot |
| 1.6 | Submit SSID + password + a 6-char PIN | Page says saved; device reboots within ~2 s |

### T1.2 Boot with credentials → on Wi-Fi in under 15 s

| # | Step | Pass |
|---|---|---|
| 2.1 | Time from `[boot] deskbuddy` to `[boot] wifi ok: http://<ip>` | **< 15 s** (`WIFI_CONNECT_TIMEOUT_MS`) |
| 2.2 | `curl http://<ip>/api/status` | 200 JSON, `setup_mode:false`, `pin_required:true` |
| 2.3 | `curl http://deskbuddy.local/api/status` from a machine with mDNS | Same 200 (skip on networks that block mDNS — note it, do not fail T1) |
| 2.4 | Open `http://<ip>/` in a browser | The PWA loads from flash. A 404 here means `uploadfs` was skipped |

### T1.3 The URL is on the screen at every boot (product requirement)

| # | Step | Pass |
|---|---|---|
| 3.1 | Watch the panel from power-on | The **first** frame drawn is the Status screen with `http://<ip>`, not the clock |
| 3.2 | Time it | Status stays **≥ 30 s** (`BOOT_STATUS_SCREEN_MS`), then screens behave normally |
| 3.3 | Power-cycle 5 times | 5/5 boots show it. Not 4/5 — this is the requirement the user asked for by name |
| 3.4 | Boot with the router off | Screen shows the offline/SoftAP state instead of a bogus IP, and recording still starts |

### T1.4 Wi-Fi failure does not stop the camera

| # | Step | Pass |
|---|---|---|
| 4.1 | Set a wrong password in Settings, reboot | Serial: `[boot] wifi failed; running offline (recording still runs)` |
| 4.2 | Wait 5 min, then pull the SD card and read it | Segments exist for that window — surveillance does not depend on the network |

### T1.5 config.h is the only place hardware lives

| # | Step | Pass |
|---|---|---|
| 5.1 | `grep -rnE "GPIO_NUM_[0-9]+|\b(1[0-9]|[2-4][0-9])\b" firmware/src --include=*.cpp` reviewed by eye | No pin number appears in any `.cpp`; every one comes from a `config.h` macro |
| 5.2 | Change `TFT_ROTATION`, rebuild, reflash | Display rotates. Nothing else was edited |

### T1.6 Sign-off

T1 passes when 1.1–1.4 all pass on a device that was erased first. 1.5 is a code review row and
can be done without hardware.

---

## T7 Soak — owner integration

72 h continuous. This is the only test that can catch a leak, and it is the reason the product
claim is "24/7". HANDOVER section 8 T7.

### T7.0 Setup

| # | Step | Pass |
|---|---|---|
| 0.1 | T1 and T2 already passed on this exact build | Recorded in this file with the git commit |
| 0.2 | Endurance-rated SD card, ≥ 64 GB, freshly formatted FAT32 | — |
| 0.3 | Mains-powered 5V/1A+ supply, not a laptop port | — |
| 0.4 | Drive credentials + one AI key set, PIN set | `/api/status` shows `upload.enabled:true` |
| 0.5 | Logger running for the whole window: `while true; do curl -s -H "X-Device-Pin: $PIN" http://<ip>/api/status >> soak.jsonl; echo >> soak.jsonl; sleep 60; done` | `soak.jsonl` grows one line a minute |

Start the clock only once 0.5 is running. A soak with no log is an anecdote.

### T7.1 It stays up

| # | Metric (from `soak.jsonl`) | Pass |
|---|---|---|
| 1.1 | `reboots` delta over 72 h | **< 9** total (< 3/day, HANDOVER T7). Every one has a matching watchdog line in the serial log |
| 1.2 | Longest gap between log lines | < 5 min (anything larger is an unlogged reboot or a hang — investigate before passing) |
| 1.3 | `uptime_s` resets | Each reset lines up with a `reboots` increment. An uptime reset without one means a brownout, not a watchdog — fix the power supply and restart the 72 h |

### T7.2 Heap floor is flat

| # | Metric | Pass |
|---|---|---|
| 2.1 | `heap_min` at hour 6 vs hour 72 | Difference **< 10 %**, and the last 24 h show no downward trend |
| 2.2 | `heap_free` never below `UPLOAD_MIN_HEAP` (60000) for more than one sample in a row | The uploader is allowed to stand down; the device is not allowed to sit at the floor |
| 2.3 | `psram_free` at hour 72 | Within a few KB of hour 6 — the ring is allocated once at boot and never grows |

A steady `heap_min` decline is a leak. Bisect by feature flag: `ENABLE_UPLOAD 0`, then
`ENABLE_AI 0`, and re-soak 12 h each — that isolates it to a task without reading any code.

### T7.3 All the footage is actually there

| # | Step | Pass |
|---|---|---|
| 3.1 | `GET /api/recordings` for each of the 3 days | 3 days listed |
| 3.2 | Segments per day vs expected (`86400 / SEGMENT_SECONDS` = 288) | ≥ 285/day, and every missing one is explained by a logged reboot |
| 3.3 | Pull the card; play 6 segments spread across the 72 h in VLC | All play, none truncated, audio/video timing sane |
| 3.4 | Total bytes on card vs `sd.used_mb` from the last status line | Within 1 % |
| 3.5 | If retention triggered (card filled): oldest day gone, newest intact, no gap in the middle | — |

### T7.4 Uploads kept up

| # | Step | Pass |
|---|---|---|
| 4.1 | Drive folder `DeskBuddy/` after 72 h | 3 date folders |
| 4.2 | Segment count in Drive vs on card | Drive ≥ 95 % of card, and `upload.queued` at the end is < 20 (the backlog drains, it does not grow monotonically) |
| 4.3 | Any duplicate filenames in a Drive day folder | **Zero.** A duplicate means the `.pending` sidecar lost a race — a real bug, fail T7 |
| 4.4 | `upload.last_error` samples | Transient errors OK; the same error in every one of the last 60 samples is a stuck uploader |

### T7.5 Nothing degraded

| # | Step | Pass |
|---|---|---|
| 5.1 | `recording.fps` hour 1 vs hour 72 | Within 10 % |
| 5.2 | `recording.dropped_frames` rate over the last 12 h vs the first 12 h | Not growing — a rising drop rate is an SD card wearing out or a leak squeezing the ring |
| 5.3 | Clock: device time vs a real clock at hour 72 | Within 6 s (T6.7's 2 s/day, NTP resync hourly) |
| 5.4 | Tap the touch pad at hour 72 | Still advances one screen. Capacitive baselines drift over days — this is the row that catches it |
| 5.5 | One AI question at hour 72 | Answers in < 20 s, no reboot |

### T7.6 Sign-off

T7 passes when 7.1–7.5 all pass over one **unbroken** 72 h window. Any reflash, power cut, or
config change restarts the clock at zero. Record in this file: build commit, start/end timestamp,
final reboot count, `heap_min` at hour 6 and hour 72, and segments-on-card vs segments-in-Drive.

---

## T2 Recording (critical path) — owner A1

Covers `firmware/src/camera.cpp` (camTask, core 0) and `firmware/src/recorder.cpp` (sdTask, core 0).
"Recording always wins" is the acceptance theme: every case below passes only if footage keeps landing on the card.

### T2.0 Pre-flight (run once before the rest)

| # | Step | Pass |
|---|---|---|
| 0.1 | `cd firmware && pio test -e native` | 8/8 tests pass (AVI layout, idx1 offsets, seg paths, retention). Needs `test_dir = ../test/firmware` in platformio.ini. |
| 0.2 | Insert a FAT32 SD card ≥ 8 GB, flash, open serial at 115200 | `[cam] init ok 800x600 q12` and `[rec] SD mounted, NNNN MB free` within 10 s of boot |
| 0.3 | `GET /api/status` | `sd.mounted: true`, `recording.active: true`, `recording.fps` within ±20 % of `settings.video.fps` |

If 0.2 fails, everything below is blocked — fix the card/pins in `config.h` first.

### T2.1 Segment files are valid AVI and play in VLC

| # | Step | Pass |
|---|---|---|
| 1.1 | Let it record 12 min, then pull the card | `/rec/YYYYMMDD/` holds ≥ 2 closed `.avi` files |
| 1.2 | Open the oldest `.avi` in VLC | Plays start to finish, no "broken or incomplete" dialog |
| 1.3 | Drag the VLC seek bar to 50 %, 90 %, 10 % | Each seek lands on a frame immediately (this is what `idx1` buys — a file without it will refuse to seek) |
| 1.4 | `ffprobe <file>.avi` | `Video: mjpeg`, `Duration ≈ SEGMENT_SECONDS`, `nb_frames` matches `duration × fps` ±5 % |
| 1.5 | Hex-dump the first 4 bytes and bytes 8..11 | `RIFF` and `AVI ` |
| 1.6 | Check the file's `RIFF` size field (bytes 4..7, little-endian) | equals `filesize − 8` — proves the close-time header patch ran |

**Fail modes to log:** VLC plays but cannot seek → `idx1` missing (check `[rec] no PSRAM for idx1` on serial). VLC reports 0:00 duration → `dwTotalFrames`/`dwLength` patch did not run (segment was killed, not closed).

### T2.2 24 h continuous, no gap > 2 s

| # | Step | Pass |
|---|---|---|
| 2.1 | Record 24 h undisturbed. Then list `/rec/*/` sorted by name | ~288 files/day at 300 s segments |
| 2.2 | For each consecutive pair, compute `start(n+1) − start(n)` from the `HHMMSS` filenames | every delta ≤ `SEGMENT_SECONDS + 2` (i.e. ≤ 302 s) — **this is the gap-under-2-s criterion** |
| 2.3 | Sum `nb_frames` across a full hour of segments | ≥ 95 % of `3600 × fps` — proves rotation is not eating frames |
| 2.4 | Watch a rotation on serial | `[rec] closed …` and the next `[rec] segment …` inside the same second |
| 2.5 | `GET /api/status` at the end | `recording.dropped_frames` < 1 % of total frames recorded |

**Why it should hold:** the ring buffer holds `RING_FRAME_COUNT` (24) frames ≈ 2.4 s at 10 fps, so frames produced during a close are buffered, not lost. If 2.2 fails on a slow card, the fix is pre-opening the next file (noted as a `ponytail:` ceiling in `recorder.cpp`).

### T2.3 SD fills → oldest day deleted, recording continues

| # | Step | Pass |
|---|---|---|
| 3.1 | Create 3 fake day dirs on the card (e.g. `/rec/20260101`, `/rec/20260102`, `/rec/20260103`) each padded with junk files until `free < SD_FREE_MIN_MB` (512 MB) | — |
| 3.2 | Boot and watch serial | `[rec] retention: purging 20260101` (the **oldest**, never today's dir) |
| 3.3 | Watch `/api/status` `recording.active` and `recording.fps` for the whole purge | never goes false; fps stays within ±20 % — deletion is batched `SD_DELETE_BATCH` files per pass exactly so it cannot stall the writer |
| 3.4 | After the purge | `[rec] purged /rec/20260101`, dir gone, `sd.free_mb` risen |
| 3.5 | Repeat until only today's dir remains | serial says `low space but nothing safe to delete — recording anyway`; recording still active (**never stop for a full card**) |
| 3.6 | Every segment written during 3.2–3.5 | still plays in VLC |

### T2.4 Power loss mid-segment

| # | Step | Pass |
|---|---|---|
| 4.1 | Record ≥ 15 min, then yank USB power ~2 min into a segment | — |
| 4.2 | Read the card on a PC before rebooting the device | the in-progress `.avi` has a sibling `.avi.rec` marker; all earlier segments have `.avi.pending` (or none, if already uploaded) |
| 4.3 | Every *prior* segment | still plays in VLC, seeks fine — a crash must never damage a closed file |
| 4.4 | Reboot the device, watch serial | `[rec] discarding interrupted /rec/…/HHMMSS.avi` exactly once; then a fresh `[rec] segment …` |
| 4.5 | After recovery | the interrupted `.avi` and its `.rec` marker are both gone; **at most one** segment was lost |
| 4.6 | Check the uploader queue after boot | every `.pending` segment was re-queued (`upload.queued` in `/api/status` ≥ the number of `.pending` files) — proves reboot does not lose un-uploaded footage |
| 4.7 | Repeat 4.1 five times | zero corrupted prior segments across all five |

### T2.5 camTask starved → watchdog restarts, recording resumes < 30 s

| # | Step | Pass |
|---|---|---|
| 5.1 | Cover the lens completely, record 2 min | recording continues (a dark frame is still a frame); fps unchanged |
| 5.2 | While recording, hold `/api/stream` open **and** hammer `/api/snapshot` in a loop from two machines | `recording.fps` stays within ±20 %; `recording.dropped_frames` may rise — dropping *stream* frames is correct, dropping *recorded* frames is not |
| 5.3 | Physically unplug the camera ribbon while running | serial shows `[cam] fb_get failed` then `[cam] retrying init` |
| 5.4 | Leave it unplugged > `WDT_TIMEOUT_S` (10 s) | `[cam] stalled NNNN ms — restarting`, board reboots |
| 5.5 | Reconnect the ribbon, measure from reboot to the first new `.avi` growing | < 30 s |
| 5.6 | `reboots` counter in `/api/status` after the cycle | incremented by exactly 1 per stall |

### T2.6 Ring buffer — a slow consumer must never stall recording

| # | Step | Pass |
|---|---|---|
| 6.1 | Open `/api/stream` from a client, then suspend it (Wi-Fi off on the phone, or `kill -STOP` on curl) for 60 s | recording fps unchanged; the stuck client costs at most one ring slot |
| 6.2 | During 6.1, `GET /api/snapshot` | still returns a JPEG within 2 s |
| 6.3 | Free-heap and free-PSRAM before vs. after 20 stream connect/disconnect cycles | no downward trend — proves every borrow got released |
| 6.4 | Run `/api/recordings?day=<today>` repeatedly while recording | responses correct, no `[rec] write error`, fps unchanged |

**Fail mode:** if fps drops while a client is stalled, a mutex is being held across a network send — that is the one thing the ring-buffer design exists to prevent.

### T2.7 Sign-off

T2 is complete when 2.1–2.6 all pass **on the real board with the real card**, and the 24 h run of T2.2 produced a file set with no gap > 2 s and every segment playable.

---

## T6 Display/touch — owner A4

Covers `firmware/src/display.cpp` / `display.h` (uiTask, core 1, prio `UI_TASK_PRIO`).
HANDOVER section 8 gives two bullets; below is what actually has to be pressed and watched.

Equipment: the assembled board with the ST7789 wired per `config.h`, a phone stopwatch, a serial
monitor at 115200, and a second device on the same LAN with a browser.

### T6.0 Pre-flight

| # | Step | Pass |
|---|---|---|
| 0.1 | `cd firmware && pio test -e native -f test_display` | 13/13 pass (screen cycle, touch state machine, word wrap, formatters). Needs `test_dir = ../test/firmware` in `platformio.ini` — flag to Opus if missing. |
| 0.2 | Flash and watch the panel through boot | Panel lights, no white/garbage frame, no reboot loop |
| 0.3 | Serial for the first 60 s | No `Guru Meditation` and no stack-overflow abort naming task `ui` — `UI_TASK_STACK` is sufficient |

**If the panel is lit but blank or wrong-coloured:** change only `TFT_ROTATION` in `config.h` and the
two calibration knobs in `display.cpp`'s `LGFX` constructor (`c.invert`, `c.rgb_order`). Those are the
only panel-variant switches; nothing else may be touched.

### T6.1 Boot URL screen — 30 s, every single boot (product requirement)

| # | Step | Pass |
|---|---|---|
| 1.1 | Power-cycle. Start the stopwatch the moment the panel lights. | The **Status** screen (title `STATUS`) is showing within 2 s |
| 1.2 | Read the top line | `http://<ip>` matching the IP the router leased; not truncated; legible at arm's length |
| 1.3 | Read the second line | `http://deskbuddy.local` (from `MDNS_HOST`) |
| 1.4 | Type the line from 1.2 into the second device's browser | The PWA loads — the URL on screen is live, not stale from an earlier DHCP lease |
| 1.5 | Watch the stopwatch | Status stays up 30 s +/- 1 s (`BOOT_STATUS_SCREEN_MS`), then flips to **Clock** by itself |
| 1.6 | Repeat 1.1-1.5 five times, including one hard power-yank (not the reset button) | 5/5. This is an "every time it starts" requirement — one miss is a fail. |
| 1.7 | Boot with the router off (no Wi-Fi) | Status still appears for 30 s and shows `no wifi`, `-`, `SSID -`. Never blank, never garbage. |

### T6.2 Tap advances exactly one screen

| # | Step | Pass |
|---|---|---|
| 2.1 | Wait out the boot screen. Note the screen (Clock). | — |
| 2.2 | One deliberate tap on the touch pad (`TOUCH_PIN`) | Advances exactly one screen: Clock -> Status |
| 2.3 | Repeat with ~1 s between taps: tap, tap, tap | Status -> AI -> Cam -> Clock. Wraps. Never skips a screen. |
| 2.4 | 20 taps at roughly 1 Hz, counting screen changes as you go | Exactly 20 advances. **19 or 21 is a fail** — 21 means `TOUCH_DEBOUNCE_MS` is too short, 19 means `TOUCH_DELTA_PCT` is too high. |
| 2.5 | 10 fast taps (~3 Hz) | At most 10 advances, and at least one for every tap you felt land. No runaway cycling. |
| 2.6 | Rest a fingertip lightly then lift, without a firm press | Either one advance or none — never two |

### T6.3 Hold = Status, and a hold must not also tap

| # | Step | Pass |
|---|---|---|
| 3.1 | From the Clock screen, hold the pad and count to 2 s | Screen jumps to **Status** at ~2 s (`TOUCH_HOLD_MS`) while your finger is still down |
| 3.2 | Keep holding another 5 s | Stays on Status. Does not keep cycling — the hold fires once. |
| 3.3 | Release | Stays on Status. **Any advance to AI on release is a fail** — that is the hold leaking a tap. |
| 3.4 | Repeat 3.1-3.3 starting from each of the other three screens | Always lands on Status, always stays there after release |
| 3.5 | Hold during the 30 s boot window | Boot screen is cancelled: it stays on Status and the Clock does not force itself in at t=30 s |

### T6.4 Touch survives environment drift

| # | Step | Pass |
|---|---|---|
| 4.1 | Temporary build logging `touchRead(TOUCH_PIN)` to serial; leave untouched 10 min | The raw value wanders, but **zero** phantom screen changes |
| 4.2 | Breathe on the pad; move a warm hand near it without contact | No phantom advance |
| 4.3 | Move the device from a cold room to a warm one, wait 10 min, then tap | Tap registers first try — the EMA baseline (`TOUCH_BASELINE_TAU_MS`) followed the drift |
| 4.4 | Tap through a case or sticker, if one is fitted | Registers. If not, lower `TOUCH_DELTA_PCT` and re-run T6.2 in full. |

**Tuning rule:** only `TOUCH_DELTA_PCT` and `TOUCH_BASELINE_TAU_MS` in `config.h` may be changed. If a
raw touch threshold appears anywhere in `display.cpp`, that is a fail regardless of behaviour.

### T6.5 Screen content, including "no data yet"

| # | Screen | Step | Pass |
|---|---|---|---|
| 5.1 | Clock | Compare with an NTP-synced phone clock | `HH:MM` matches to the minute; date line reads like `Sun 02 Aug 2026` |
| 5.2 | Clock | Boot with no Wi-Fi, so no NTP | Shows `--:--` and `no time yet`. Never `00:00` presented as truth, never garbage. |
| 5.3 | Clock | Wi-Fi icon, top right | 1-4 cyan bars while connected; red `x` with the router off; updates within 1 s of unplugging the router |
| 5.4 | Clock | Start and then stop recording | Red dot + `REC` appears top left within 1 s of `recordingActive()` flipping, and clears the same way |
| 5.5 | Status | Eject the SD card and reboot | `SD  --` (not `SD 0%`), `Queue 0`, no crash |
| 5.6 | Status | With the card in | `SD  NN% used` within 1 % of `GET /api/status`; uptime line matches `uptime` from the same call |
| 5.7 | Status | Let segments queue with Wi-Fi off | `Queue N` tracks `uploadQueueDepth()` |
| 5.8 | AI | Before any question has been asked | `no question yet` — not blank, not stale |
| 5.9 | AI | Ask a short question from the PWA | Question in cyan, answer in white, both wrapped at word boundaries |
| 5.10 | AI | Ask something with a long answer ("explain TCP") | Text fills the panel and ends in `...`. Never overflows the edge, never overwrites the header. |
| 5.11 | AI | Ask a question containing a 40-character unbroken token | Hard-split across lines, still ellipsized if the answer is long |
| 5.12 | Cam | After ~20 min of recording | `Today  N seg` matches the `.avi` count in `/rec/YYYYMMDD/`; `FPS` within +/- 20 % of `/api/status` |
| 5.13 | Cam | Before any upload has succeeded, then after one | `Upload never`, then `Upload HH:MM:SS` matching the Drive file's time |
| 5.14 | Cam | No SD card, then card in and recording | Bottom line `no SD card`, then `recording` |

### T6.6 No flicker, no starvation

| # | Step | Pass |
|---|---|---|
| 6.1 | Watch the Clock screen for 2 min | Only changed values repaint. **No full-screen flash once per second** — a visible 1 Hz blink means the dirty-field cache is not working. |
| 6.2 | Film the panel at 60 fps and step through frames | No fully background-coloured frame except immediately after a deliberate screen change |
| 6.3 | Stream MJPEG to the PWA while recording, then tap through all four screens | Screens still advance within one `TOUCH_POLL_MS` cycle and recording fps in `/api/status` is unchanged — uiTask never blocks core 0 |
| 6.4 | Trigger an AI answer while tapping | No stutter, no torn text, no reboot (AI text is copied under lock and drawn unlocked) |
| 6.5 | Compare `/api/status` heap floor before and after 1 h of tapping | No downward trend — `display.cpp` allocates nothing per frame |

### T6.7 Clock drift < 2 s/day

| # | Step | Pass |
|---|---|---|
| 7.1 | At t=0, photograph the Clock screen beside an NTP-synced phone clock | Offset recorded, should be under 1 s |
| 7.2 | Block NTP at the router (drop outbound UDP 123) and leave the device 24 h | — |
| 7.3 | Photograph both clocks at t=24 h | Free-running drift measured. Expect tens of seconds — that is the SoC RTC, not the display, and it is why 7.4 exists. |
| 7.4 | Unblock NTP, wait one `NTP_RESYNC_MS` window (1 h), photograph again | Back to within 2 s of the phone. **This is the T6 pass condition:** with hourly resync, observed error stays < 2 s/day. |
| 7.5 | Repeat the photo at t=48 h and t=72 h (piggyback on the T7 soak) | Still < 2 s each time, and no visible backwards jump mid-minute |

### T6.8 Sign-off

T6 is complete when T6.0-T6.7 pass on the real board with the real panel, T6.1 passed 5/5
power-cycles, and T6.2.4 counted exactly 20 advances for 20 taps.

---

## T3 Upload — owner A2

Covers `firmware/src/uploader.cpp` (uploadTask, core 1, prio `UPLOAD_TASK_PRIO`).
Acceptance theme: **footage reaches Drive exactly once, and recording never notices.**

Equipment: a Google account with the Drive API enabled and a refresh token obtained per
`docs/get_drive_token.md`, a browser signed into that account, `curl`, and a way to cut Wi-Fi
(router admin page or unplug the AP).

### T3.0 Pre-flight

| # | Step | Pass |
|---|---|---|
| 0.1 | `cd firmware && pio test -e native -f test_services` | 11/11 pass (Drive folder paths, backoff schedule, Content-Range, request bodies, SSE, masking). Needs `test_dir = ../test/firmware` in `platformio.ini` — flag to Opus if missing. |
| 0.2 | `POST /api/settings` with `drive_client_id`, `drive_client_secret`, `drive_refresh_token`, then `POST /api/reboot` | `{"ok":true,...}`; after reboot `GET /api/settings` shows the token masked (`********mNo1`) and the client id in full |
| 0.3 | Watch serial for 60 s after boot | No `[up]` line containing anything that looks like a token. Zero is the only acceptable count. |
| 0.4 | `GET /api/status` → `upload` block | `enabled: true`, `queued` ≥ 0, `last_error: null` |

If `drive_refresh_token` is empty the uploader stays asleep by design — 0.2 is a hard prerequisite.

### T3.1 A closed segment lands in `DeskBuddy/YYYYMMDD/` within 10 min

| # | Step | Pass |
|---|---|---|
| 1.1 | Start recording on idle Wi-Fi. Wait for one rotation (`SEGMENT_SECONDS`, 5 min). | Serial: `[up] done /rec/YYYYMMDD/HHMMSS.avi` |
| 1.2 | Open Drive in the browser | Folder `DeskBuddy` exists at the root (or under `drive_folder_id` if one was set), containing `YYYYMMDD` |
| 1.3 | Inside that day folder | `HHMMSS.avi` present, size **byte-identical** to the file on the SD card |
| 1.4 | Download it and open in VLC | Plays start to finish — a truncated resumable upload shows up here and nowhere else |
| 1.5 | Time from segment close to Drive listing | < 10 min |
| 1.6 | Let a second segment close | Goes into the **same** day folder — no duplicate `DeskBuddy` and no duplicate `YYYYMMDD` (proves the folder-id cache) |
| 1.7 | Let the clock roll past local midnight during a soak | A new `YYYYMMDD` folder is created on demand; the old one is untouched |
| 1.8 | `GET /api/recordings?day=<today>` | Uploaded segments report `"uploaded": true`, the in-progress one `false` |

### T3.2 Wi-Fi cut for 1 h — queue persists, drains on reconnect, no duplicates

| # | Step | Pass |
|---|---|---|
| 2.1 | Note the Drive file count. Cut Wi-Fi at the router. | Recording continues (check `recording.active` on the display, core 0 is unaffected) |
| 2.2 | Leave it 1 h (≈12 segments) | Serial shows `[up] … device busy or offline (retry in Ns)` with **N growing** 5,10,20,40,80,160,300 and then staying at 300 — never a tight retry loop |
| 2.3 | Read the SD card | ~12 `.avi` files each with an `.avi.pending` sidecar |
| 2.4 | Restore Wi-Fi. Watch `upload.queued` in `/api/status`. | Counts down to 0 within ~30 min; `upload.last_error` returns to `null` |
| 2.5 | Count files in the Drive day folder | Exactly the number of closed segments. **Any file appearing twice, or any `HHMMSS (1).avi`, is a fail.** |
| 2.6 | Re-check the card | Every uploaded segment's `.avi.pending` sidecar is gone; the `.avi` itself is still there |
| 2.7 | Now reboot the device and wait 15 min | **Nothing re-uploads.** The absent sidecar is the "already done" marker and it is checked before every attempt. |

### T3.3 Interrupted upload resumes from Drive's byte offset

| # | Step | Pass |
|---|---|---|
| 3.1 | Pick a large segment (≥ 3 chunks, i.e. ≥ 768 KB). Start its upload and cut Wi-Fi mid-transfer (watch `upload.progress_pct` cross ~40 %). | — |
| 3.2 | Restore Wi-Fi | Serial shows the item retried; total bytes pushed over the air for that file is **less than 2×** its size |
| 3.3 | The resulting Drive file | Correct size, plays in VLC — resume landed on the right offset, not a re-send from 0 |
| 3.4 | Repeat but power-cycle the device mid-upload instead of cutting Wi-Fi | The segment uploads again **from 0** and appears in Drive **once**. Restarting from 0 here is the documented `ponytail:` ceiling (session URI is RAM-only); a duplicate file is not. |
| 3.5 | Let a session sit unfinished > 1 week, then reconnect | `[up] resumable session expired`, backoff, then a clean fresh upload — no crash, no wedged queue |

### T3.4 Upload running → recording fps unchanged (the whole point)

| # | Step | Pass |
|---|---|---|
| 4.1 | With uploads idle, sample `recording.fps` from `/api/status` every 10 s for 5 min. Record the mean — call it F0. | — |
| 4.2 | Queue 10 segments (cut Wi-Fi for 50 min, restore) and sample `recording.fps` the same way for the whole drain | Mean within **±5 %** of F0; no single sample below 0.8 × F0 |
| 4.3 | `recording.dropped_frames` delta across the drain | Same order as an idle 50 min window — uploading must not add drops |
| 4.4 | Compare segment start times (from filenames) across the drain window | No gap > `SEGMENT_SECONDS + 2` — T2.2's criterion must still hold under upload load |
| 4.5 | Watch `heap_free` in `/api/status` during a chunk-heavy drain | Never dips under `UPLOAD_MIN_HEAP` (60 000); if it does, the uploader is expected to stand down and log it rather than fail an allocation |
| 4.6 | Artificially drop free heap (open `/api/stream` plus 4 parallel `/api/recordings` calls) during an upload | Uploader pauses, `upload.queued` holds steady, recording unaffected, and it resumes when heap recovers |

### T3.5 `upload_event` over `/ws` matches reality

| # | Step | Pass |
|---|---|---|
| 5.1 | `websocat "ws://<ip>/ws?pin=<pin>"` while a segment uploads | Sequence per file: `queued` → `started` → `progress`* → `done` |
| 5.2 | Inspect the `progress` frames | At most one per 10 % **and** per 5 s (whichever is rarer); `progress_pct` monotonically increasing |
| 5.3 | The `done` frame | `drive_file_id` non-null and matching the file's id in the Drive URL; `error` null |
| 5.4 | Force a failure (bad `drive_client_secret`) | A `failed` frame with a non-null `error`, and an `error` envelope with `source:"uploader"`. Neither contains any part of the secret. |
| 5.5 | Compare every `upload_event.queued` with `/api/status` `upload.queued` at the same moment | Agree within one event |

### T3.6 Sign-off

T3 is complete when 3.1–3.5 pass on the real board against a real Drive account, T3.2.5 found
**zero** duplicates, and T3.4.2 measured fps within ±5 % of the idle baseline.

---

## T5 AI — owner A2

Covers `firmware/src/ai.cpp` (aiTask, core 1, prio `AI_TASK_PRIO`, queue depth 1) and the
`POST /api/ai` route in `webserver.cpp`.

Set up both keys first (`openrouter_key`, `gemini_key`) via `POST /api/settings`, then reboot.

### T5.0 Pre-flight

| # | Step | Pass |
|---|---|---|
| 0.1 | `pio test -e native -f test_services` | Passes — covers both providers' request bodies and both SSE shapes |
| 0.2 | `GET /api/status` → `ai` block | `enabled: true`, `provider` matches `settings.ai_provider`, `busy: false` |

### T5.1 An answer from each provider in under 20 s

| # | Step | Pass |
|---|---|---|
| 1.1 | `curl -X POST http://<ip>/api/ai -H 'X-Device-Pin: <pin>' -H 'Content-Type: application/json' -d '{"prompt":"say hello in five words","provider":"openrouter"}'` | `200` with `answer` non-empty and `elapsed_ms` < 20000 |
| 1.2 | Same with `"provider":"gemini"` | `200`, non-empty answer. Provider switch works per-request. |
| 1.3 | Omit `provider` entirely | Uses `settings.ai_provider`; the `provider` field in the response says which one ran |
| 1.4 | Change `ai_provider` via `POST /api/settings`, then repeat 1.3 | Now uses the new default, **without a reboot** |
| 1.5 | Ask from the PWA chat box | Answer renders in the UI within 20 s |
| 1.6 | Ask a question with quotes, newlines and an emoji | Answer comes back intact; no truncation at the quote, no malformed JSON in the response |

### T5.2 Streaming over `/ws`

| # | Step | Pass |
|---|---|---|
| 2.1 | Open `websocat "ws://<ip>/ws?pin=<pin>"`, then POST with `"stream":true` | HTTP replies `202` `{"id":"xxxx","provider":...,"streaming":true}` **immediately** (< 200 ms) |
| 2.2 | Watch the socket | `ai_chunk` frames with the same `id`, `seq` starting at 0 and incrementing by exactly 1, no gaps, no repeats |
| 2.3 | Concatenate every `text` in `seq` order | Reads as a coherent answer — same content as the blocking mode would return |
| 2.4 | The final frame | `ai_done` with the same `id`, `chunks` equal to the number of `ai_chunk` frames seen, `error: null` |
| 2.5 | During streaming, watch the `status` frames | Still arriving on schedule — a streamed request does not block the push loop |
| 2.6 | During streaming, `GET /api/snapshot` from another machine | Returns a JPEG normally |

### T5.3 Queue depth 1

| # | Step | Pass |
|---|---|---|
| 3.1 | Fire two `POST /api/ai` back to back (`&` in the shell) | First `200`/`202`; second `409` `{"error":"busy","message":"ai request already in flight"}` |
| 3.2 | `GET /api/status` while one is in flight | `ai.busy: true` |
| 3.3 | After the first completes, retry the second | Succeeds. The 409 must not leave the queue wedged. |
| 3.4 | Fire 20 requests in a tight loop | Exactly one runs at a time; 19 clean 409s; no crash, no heap decline |

### T5.4 Every failure is clean — no crash, no reboot

Record `reboots` from `/api/status` before this block and re-check after each row; **it must not change.**

| # | Step | Pass |
|---|---|---|
| 4.1 | Set `openrouter_key` to `sk-or-v1-definitelywrong`, ask a question | `502` `{"error":"internal","message":"provider returned 401"}`. Device alive. |
| 4.2 | Clear `openrouter_key` to `""`, ask with `"provider":"openrouter"` | `503` `{"error":"unavailable","message":"no api key for provider openrouter"}` |
| 4.3 | Same for `gemini_key` | `503` naming `gemini` |
| 4.4 | Cut Wi-Fi, ask a question | `503` `{"error":"unavailable","message":"no wifi"}` — answered from the LAN side if reachable, otherwise the PWA shows unreachable. No hang. |
| 4.5 | Block outbound 443 at the router mid-request | Request ends within `AI_TIMEOUT_MS` (20 s) with `502 provider timed out`; `ai.busy` returns to `false` |
| 4.6 | `POST /api/ai` with `{"prompt":""}` | `400 bad_request` |
| 4.7 | `POST /api/ai` with a 2001-character prompt | `400 bad_request` |
| 4.8 | `POST /api/ai` with `{"prompt":"hi","provider":"anthropic"}` | `400 bad_request` `unknown provider` |
| 4.9 | `POST /api/ai` with a malformed body (`{`) | `400`, not a reboot |
| 4.10 | Repeat 4.1–4.9 three times, then check `heap_min` | Not trending down — failed requests free everything they allocated |
| 4.11 | After all of the above | `reboots` is exactly what it was at the start of T5.4 |

### T5.5 AI during recording → zero dropped segments

| # | Step | Pass |
|---|---|---|
| 5.1 | Baseline `recording.fps` over 5 min with no AI traffic (call it F0) | — |
| 5.2 | Ask a question every 30 s for 30 min (both providers, mixed blocking and streaming) | `recording.fps` mean within ±5 % of F0 |
| 5.3 | List the segments written during 5.2 | ~6 files, no gap > `SEGMENT_SECONDS + 2`, all play in VLC |
| 5.4 | `recording.dropped_frames` delta | Same order as an idle window |
| 5.5 | During a **blocking** (`stream:false`) request, poll `GET /api/status` from another machine | May be delayed until the answer lands — this is the documented `ponytail:` ceiling of blocking mode. **Recording fps must still be unaffected**, and `/ws` `status` frames must still arrive. |
| 5.6 | Repeat 5.5 with `stream:true` | No delay at all — this is why the PWA should default to streaming |

### T5.6 Display integration (with A4)

| # | Step | Pass |
|---|---|---|
| 6.1 | Ask a question, then tap to the AI screen | Shows that question and answer (`aiLastQA()`) |
| 6.2 | Ask a second question | Screen updates to the new pair |
| 6.3 | Trigger a failing request (bad key) | The **previous good** pair stays on screen — a failure never overwrites the last answer with an error string |

### T5.7 Sign-off

T5 is complete when both providers answered inside 20 s (T5.1), streaming produced a gapless
`seq` sequence (T5.2.2), every failure in T5.4 returned the exact contract status with `reboots`
unchanged, and T5.5 showed zero recording impact.

---

## T8 Security minimum — owner A2

Covers PIN enforcement across `webserver.cpp` and secret masking across `storage.cpp`,
`webserver.cpp`, `uploader.cpp`, `ai.cpp`. HANDOVER section 8: *"Settings/API endpoints require
the device PIN. Keys never appear in logs, serial output, or unmasked API responses."*

Two hard rules: **an unauthenticated request must never return data**, and **a secret must never
leave the device except as the `Authorization` header of the request that needs it.**

### T8.0 Set-up

Set `device_pin` to `482913` and reboot. Export `PIN=482913`, `IP=<device-ip>`.

### T8.1 Every protected endpoint rejects a missing PIN

Run each with **no** `X-Device-Pin` header. Every row must be `401` with body
`{"error":"unauthorized","message":"Missing or invalid X-Device-Pin"}` — not 200, not 500, not
an empty body, and **not any fragment of the resource**.

| # | Request | Pass |
|---|---|---|
| 1.1 | `curl -i $IP/api/snapshot` | 401 |
| 1.2 | `curl -i $IP/api/stream` | 401 |
| 1.3 | `curl -i -X POST $IP/api/ai -d '{"prompt":"hi"}' -H 'Content-Type: application/json'` | 401 |
| 1.4 | `curl -i $IP/api/recordings` | 401 |
| 1.5 | `curl -i "$IP/api/recordings/file?path=/rec/20260802/003000.avi"` | 401 |
| 1.6 | `curl -i $IP/api/settings` | 401 — **and no key material anywhere in the response** |
| 1.7 | `curl -i -X POST $IP/api/settings -d '{"device_pin":""}' -H 'Content-Type: application/json'` | 401, and the PIN is **still set** afterwards (an unauthenticated request must not be able to disable auth) |
| 1.8 | `curl -i -X POST $IP/api/reboot` | 401, and the device does **not** reboot (`uptime_s` keeps climbing) |
| 1.9 | `curl -i -X POST $IP/api/setup -d '{"wifi_ssid":"x"}' -H 'Content-Type: application/json'` | `404` — the setup route is gone once the device is in station mode |

### T8.2 Wrong PIN, and the exempt list is exactly the contract

| # | Request | Pass |
|---|---|---|
| 2.1 | Any 1.x request with `X-Device-Pin: 000000` | 401, same body as a missing header (the device does not distinguish the two) |
| 2.2 | `X-Device-Pin: 482913 ` (trailing space) | 401 — exact string compare |
| 2.3 | `X-Device-Pin: 48291` (prefix) | 401 |
| 2.4 | `x-device-pin: 482913` (lowercase header name) | **200** — HTTP header names are case-insensitive; a 401 here breaks browsers |
| 2.5 | `curl -i $IP/api/status` with no header | **200** — the only always-exempt GET |
| 2.6 | `curl -i -X OPTIONS $IP/api/settings` with no header | **204** — preflight is always exempt |
| 2.7 | `GET /api/status` body | `pin_required: true`; and with the PIN cleared it flips to `false` |
| 2.8 | Clear `device_pin` to `""`, retry every row of T8.1 | All succeed — empty PIN disables auth, as documented |

### T8.3 WebSocket PIN handshake

| # | Step | Pass |
|---|---|---|
| 3.1 | `websocat "ws://$IP/ws?pin=$PIN"` | Connects and receives `status` frames |
| 3.2 | `websocat "ws://$IP/ws?pin=wrong"` | Handshake accepted, then closed immediately with code **4401** |
| 3.3 | `websocat "ws://$IP/ws"` (no param) | Closed with **4401** |
| 3.4 | In case 3.2/3.3, capture everything received before the close | **Zero** frames. Not one `status` payload may leak to an unauthenticated socket. |
| 3.5 | With `device_pin` cleared, `websocat "ws://$IP/ws"` | Connects normally |
| 3.6 | Open 3 authorised sockets, then kill them abruptly | Device sheds them (`ws.cleanupClients()`); heap returns to baseline |

### T8.4 Secrets never appear in a response

| # | Step | Pass |
|---|---|---|
| 4.1 | Set every secret to a distinctive canary: `openrouter_key=sk-or-v1-CANARY1234`, `gemini_key=AIzaCANARY5678`, `drive_refresh_token=1//0gCANARY9012`, `drive_client_secret=GOCSPX-CANARY3456`, `wifi_pass=CANARYpass`. Reboot. | — |
| 4.2 | `curl $IP/api/settings -H "X-Device-Pin: $PIN" \| grep -i canary` | **No output.** Each field shows only the masked form (`********1234`), never more than the last 4 characters. |
| 4.3 | Same response: `device_pin` field | Masked (`**13`) — the PIN masks itself too |
| 4.4 | Same response: `drive_client_id`, `drive_folder_id`, `wifi_ssid` | Returned in **full** — these are not secrets (docs/API.md 3.7) |
| 4.5 | `curl $IP/api/status \| grep -i canary` | No output |
| 4.6 | `curl $IP/api/recordings -H "X-Device-Pin: $PIN" \| grep -i canary` | No output |
| 4.7 | Trigger every error path in T5.4 and T3.5.4 and grep every response body for `canary` | No output. Provider and Drive error bodies are read and discarded, never quoted back. |
| 4.8 | `POST /api/settings` sending back the **masked** value the GET returned | The masked string is stored literally (that is what "send only what you edited" means) — so verify the PWA does **not** do this, then restore the real key |

### T8.5 Secrets never appear on serial

| # | Step | Pass |
|---|---|---|
| 5.1 | With the canaries from 8.4 set, capture serial from power-on for 10 min covering: boot, Wi-Fi connect, a settings save, two AI questions (one failing with a bad key), and one Drive upload (one failing with a bad secret) | — |
| 5.2 | `grep -i canary serial.log` | **Zero matches. This is the T8 pass/fail line.** |
| 5.3 | `grep -iE 'sk-or-|AIza|GOCSPX-|1//0g|Bearer ' serial.log` | Zero matches — no key-shaped string in any form |
| 5.4 | Set `CORE_DEBUG_LEVEL=5` in `platformio.ini`, reflash, repeat 5.1–5.3 | Still zero. If the HTTP client library logs headers at level 5, that is a real finding — drop the debug level in the shipped build and record it here. |
| 5.5 | Force a panic (pull the camera ribbon, per T2.5) and read the backtrace/core dump | No key material in the dump |

### T8.6 Path traversal and input hardening

| # | Request | Pass |
|---|---|---|
| 6.1 | `GET /api/recordings/file?path=/etc/passwd` | `400 bad_request` |
| 6.2 | `GET /api/recordings/file?path=/rec/../../secret` | `400 bad_request` — the `..` check fires even though the prefix matches |
| 6.3 | `GET /api/recordings/file` with no `path` | `400` |
| 6.4 | `GET /api/recordings/file?path=/rec/20260101/nope.avi` | `404 not_found` |
| 6.5 | `POST /api/settings` with `device_pin: "12"` | `400`, and the existing PIN is unchanged |
| 6.6 | `POST /api/settings` with `video.fps: 99` | `400`, and **nothing at all** was written (re-GET to confirm every other field is untouched — validation is all-or-nothing) |
| 6.7 | `POST /api/settings` with `tz_offset_min: 9999` | `400` |
| 6.8 | `GET /api/recordings?day=<script>` | `400` (length check), never echoed into the response |

### T8.7 CORS is open on purpose, auth is not

| # | Step | Pass |
|---|---|---|
| 7.1 | From the Vercel-hosted PWA (different origin), call `GET /api/status` | Succeeds — `Access-Control-Allow-Origin: *` is present |
| 7.2 | Browser devtools, any protected call | A preflight `OPTIONS` returns `204` with `Access-Control-Allow-Headers: Content-Type, X-Device-Pin`; the real request then carries the PIN |
| 7.3 | From a hostile page on another origin, call `GET /api/settings` without a PIN | `401`. **CORS is not the access control — the PIN is.** A wide-open CORS policy with a PIN behind it is the intended design; a 200 here is a critical fail. |

### T8.8 Sign-off

T8 is complete when T8.1–T8.7 pass, `grep -i canary` over a 10-minute serial capture returned
nothing (T8.5.2), and no `/api` response other than `/api/status` and `OPTIONS` succeeded without
a valid PIN.

**Known, accepted v1 gap (docs/API.md 1):** there is no rate limiting on PIN attempts. The device
is LAN-only. If it is ever exposed beyond the LAN, add the 1 s penalty after 5 failures noted in
the API contract and re-run T8.2.

---

## T4 Web app — owner A3

Covers `webapp/` (the PWA) only. Two run modes for every case:

- **MOCK** — `node webapp/mock-server.js`, then open `http://localhost:8080/`. PIN `482913`
  (`MOCK_PIN="" node webapp/mock-server.js` starts a no-auth device). The mock serves the PWA
  *and* the API from one origin, so it is the fast loop; it does not prove CORS.
- **HW** — real device, **app served from the device itself** (`pio run -t uploadfs`, then open
  `http://<ip>/` or `http://deskbuddy.local/`). Same origin, so the Device panel needs only the
  PIN — the address defaults to `location.origin`. This mode proves LittleFS serving, mDNS and
  real timing. Rows marked **HW only** cannot pass on the mock.

  > Revised at integration: T4 originally ran HW from a Vercel-hosted page. That path is dead —
  > HTTPS→HTTP is blocked as mixed content (HANDOVER section 3, `docs/DEPLOY.md`). Any row that
  > said "proves CORS" now proves device-served same-origin instead.

### T4.0 Pre-flight

| # | Step | Pass |
|---|---|---|
| 0.1 | Open `test/webapp/test.html` (directly, or `http://localhost:8080/test/webapp/test.html`) | Header reads `ALL <n> TESTS PASSED`, tab title `PASS`, zero red lines |
| 0.2 | `node webapp/mock-server.js` | Prints `mock device on http://localhost:8080  PIN=482913` |
| 0.3 | Open `http://localhost:8080/` with DevTools open | No console errors; Status tab populates within 2 s |

If 0.1 fails, stop — a pure function is broken and every screen below is unreliable.

### T4.1 PWA install + offline shell (HANDOVER T4 bullet 1)

| # | Step | Pass |
|---|---|---|
| 1.1 | Desktop Chrome ▸ DevTools ▸ Application ▸ Manifest | Name "ESP32-S3 Desk Buddy", both icons resolve, no manifest errors |
| 1.2 | Application ▸ Service Workers | `sw.js` activated and running |
| 1.3 | Install from the omnibox install icon | Opens in its own window, icon = blue ring |
| 1.4 | Android Chrome, same URL ▸ "Add to home screen" | Installs; launches standalone with no browser chrome |
| 1.5 | Stop the mock / power off the device, then reload the installed app | Shell renders (header, tabs, Device panel) and the banner reads **"Device unreachable — check the address and that you are on the same Wi-Fi."** — **never** the browser's error page |
| 1.6 | Turn the phone's Wi-Fi off entirely, relaunch | Same as 1.5, plus the status pill shows `offline` |
| 1.7 | Application ▸ Cache Storage ▸ `deskbuddy-shell-v1` | Contains only the 7 shell files. **No `/api/*` entry ever appears** — re-check after using every tab |

### T4.2 Status over WS, with polling fallback

| # | Step | Pass |
|---|---|---|
| 2.1 | Status tab, watch the "Connection" row | Reads `WebSocket (live)`; uptime advances in 5 s steps (`STATUS_PUSH_MS`) |
| 2.2 | All rows populated | Uptime, free heap (+min), PSRAM, recording line, upload queue, Wi-Fi SSID/IP/RSSI, device time, firmware — no `–` placeholders left |
| 2.3 | SD bar | Percentage matches `used_mb / total_mb` from `GET /api/status`; text shows used / total / free |
| 2.4 | While recording (`recording.active = true`) | Row is red and starts with `● REC` |
| 2.5 | Kill the mock server (or unplug the device) | Within ~10 s the pill goes red `device unreachable` and the banner appears. No console exception |
| 2.6 | Start it again | Reconnects on its own. DevTools ▸ Network ▸ WS shows retry gaps of about 1, 2, 4, 8, 16, 30, 30 s (`DB.backoffMs`) |
| 2.7 | Block only the WS, leaving HTTP up (mock: comment out `srv.on('upgrade')`) | Connection row switches to `polling /api/status` and values still refresh every 5 s |

### T4.3 PIN handling (ties to T8)

| # | Step | Pass |
|---|---|---|
| 3.1 | Device panel: enter a **wrong** PIN, Connect | Status still fills in (`/api/status` is auth-exempt) but the WS closes with **4401** → banner "Device PIN required or wrong", Device panel opens, focus lands in the PIN field |
| 3.2 | With the wrong PIN, open Settings | 401 → "Wrong or missing device PIN." No stack trace, no blank screen |
| 3.3 | Enter the correct PIN, Connect | Everything works; reload the page → still connected (address + PIN persisted in `localStorage`) |
| 3.4 | DevTools ▸ Network, any protected call | Request carries `X-Device-Pin`; the WS URL carries `?pin=` |
| 3.5 | `MOCK_PIN="" node webapp/mock-server.js`, leave the PIN field blank | All tabs work with no PIN sent anywhere |
| 3.6 | Click **Forget** | localStorage cleared, pill shows `no device set`, Device panel opens |

### T4.4 Live: snapshot + stream

| # | Step | Pass |
|---|---|---|
| 4.1 | "Take snapshot" | Image appears **< 2 s** (HANDOVER T4); caption shows the time |
| 4.2 | "Start stream" | Button flips to "Stop stream" (`aria-pressed=true`) and the image updates continuously |
| 4.3 | **HW only** — count frames over 10 s while recording | **>= 3 fps**, and `recording.fps` on the Status tab does not drop |
| 4.4 | Open a second browser/device and start its stream too | Second client gets 409 → "The camera stream is already in use by another device…". The first client keeps streaming |
| 4.5 | Device closes the stream mid-flight (mock: kill the server) | Message "Stream closed by device — press Start to reconnect." Button resets. No runaway retry loop in the Network tab |
| 4.6 | "Stop stream" | The request shows as cancelled; on HW the stream slot frees, so 4.4 can be repeated |

### T4.5 AI chat

| # | Step | Pass |
|---|---|---|
| 5.1 | Prompt "what is on my desk?", provider OpenRouter, Ask | Answer streams in word by word (`ai_chunk`); Ask re-enables on `ai_done` |
| 5.2 | Switch to Gemini and re-ask | Request body carries `"provider":"gemini"` |
| 5.3 | Pick a provider with no key (mock: Gemini has none) | "no api key for provider gemini". No crash |
| 5.4 | Ask again while one request is in flight | Ask is disabled; a forced double-send shows the 409 "busy" line |
| 5.5 | Take the device offline mid-answer | Falls back to "Device unreachable…"; the partial answer is discarded when `ai_done` carries an error |
| 5.6 | **HW only** — configure a bad OpenRouter key | Clean "provider returned 401" line, **no API key visible in the UI or console**, no device reboot |
| 5.7 | Any error above | Rendered text is one sentence — never a JS stack trace or raw JSON |

### T4.6 Recordings

| # | Step | Pass |
|---|---|---|
| 6.1 | Recordings tab | Day list renders with segment count and size; "N GB free" shown |
| 6.2 | Click a day | Segment list shows name, size, duration and `uploaded` / `on SD only` |
| 6.3 | "Download" on a segment | File saves under its `.avi` name; **HW only:** it opens in VLC |
| 6.4 | Request an unknown day (`?day=20200101` by hand) | "Not found on the device." |
| 6.5 | **HW only** — remove the SD card | "That part of the device is not ready." (503); the app stays usable |

### T4.7 Settings partial-update round-trip (HANDOVER T4 bullet 3)

The whole point: **only edited fields are sent.** Watch the POST body in DevTools for every row.

| # | Step | Pass |
|---|---|---|
| 7.1 | Open Settings | Secrets show masked (`********3456`); `drive_client_id` / `drive_folder_id` in full; helper text explains the masking |
| 7.2 | Change nothing, Save | "Nothing changed — nothing sent." **Zero** network requests |
| 7.3 | Change only the AI provider, Save | Body is exactly `{"ai_provider":"gemini"}`; response `saved:["ai_provider"]` |
| 7.4 | Type a new OpenRouter key, Save | Body contains `openrouter_key` **and nothing else** — untouched masked fields are never echoed back |
| 7.5 | Clear the Gemini field (already empty), Save | `gemini_key` is **not** sent — there is no change |
| 7.6 | Set a Gemini key and save; then clear it and save | The second body is `{"gemini_key":""}` and the device clears the field |
| 7.7 | Change the Wi-Fi password | Response has `reboot_required: true` and the message says a reboot is needed. The app does **not** reboot by itself |
| 7.8 | Change the resolution to 640x480 | Body sends the whole `video` object with all four keys |
| 7.9 | Force an invalid value (fps 99 via DevTools) | 400 → the device's `message` is shown; nothing else is saved |
| 7.10 | Change the device PIN | Confirm dialog first; afterwards the app reconnects with the new PIN and the WS does not 4401 |
| 7.11 | **HW only** — save keys, reboot, reopen | Keys still active (AI works) and `GET /api/settings` still returns them masked (T8) |

### T4.8 Reboot

| # | Step | Pass |
|---|---|---|
| 8.1 | "Reboot device" | Confirm dialog appears; Cancel sends nothing |
| 8.2 | Confirm | `POST /api/reboot` → banner "Reboot requested — reconnecting…"; **HW only:** the app reconnects by itself within ~30 s |

### T4.9 Responsive / theme / accessibility

| # | Step | Pass |
|---|---|---|
| 9.1 | DevTools device toolbar at 360 px wide | No horizontal scrollbar; the tab strip scrolls; every control is reachable |
| 9.2 | Desktop at 1440 px | Content stays in a readable column, not stretched edge to edge |
| 9.3 | Toggle `prefers-color-scheme` | Dark by default; the light theme is readable, body-text contrast >= 4.5:1 |
| 9.4 | `Tab` through the whole page with no mouse | Every button, input and tab is reachable **in order** with a visible focus ring; the full flow (set device → status → ask AI → save settings) is completable |
| 9.5 | Lighthouse ▸ Accessibility (against the mock) | >= 95, zero "form elements must have labels" findings |
| 9.6 | Screen reader (NVDA/VoiceOver) on the Status tab | Pill and banner announce on change (`aria-live`); every input reads its own label |

### T4.10 Sign-off

T4 is complete when 4.0–4.9 pass on the **mock** (all non-HW rows) and 4.1, 4.2, 4.3, 4.4, 4.5,
4.7 and 4.8 pass against **real hardware with the PWA served from the device's own flash** —
that last part is what proves LittleFS serving, mDNS and the 5 s WS push all work together.
