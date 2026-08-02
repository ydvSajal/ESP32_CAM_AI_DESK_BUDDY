# SETUP — flash, wire, first boot

Order: wire → flash → first boot (SoftAP) → settings → Drive token → PWA.

## 0. Before anything: confirm the board

Everything is built against the defaults in `firmware/src/config.h` (Seeed XIAO ESP32S3 Sense, OV2640, onboard SD, ST7789 240x240, one touch pad). **If your board differs, change only `config.h`.** No pin, size, or resolution exists anywhere else in the project.

## 1. Wiring (external display only — camera, mic, SD are onboard)

| Display (ST7789) | XIAO pad | GPIO |
|---|---|---|
| SCLK | D1 | 2 |
| MOSI / SDA | D2 | 3 |
| DC | D3 | 4 |
| CS | D4 | 5 |
| RST | D5 | 6 |
| VCC / GND / BL | 3V3 / GND / 3V3 | — |

Touch pad wire (a bare pad, foil square, or exposed via) goes to **D0 / GPIO1**. Keep the wire short; long leads pick up mains hum and make the baseline wander.

SD card: FAT32, class 10 or better. 24/7 recording is a heavy write load — use an endurance-rated card, not a spare phone card.

Power: a proper 5V/1A+ supply. Camera + Wi-Fi + SD writing together will brown out a weak USB port, and a brownout mid-write is how you corrupt a card.

## 2. Flash

```bash
pip install platformio          # or use the PlatformIO VS Code extension
cd firmware
pio run -t upload               # 1. firmware. hold BOOT while plugging in if it won't enumerate
pio run -t uploadfs             # 2. the web app -> LittleFS. NOT optional.
pio device monitor -b 115200    # watch first boot
```

**Both steps.** `upload` flashes the firmware; `uploadfs` flashes the filesystem image
that holds the PWA. `platformio.ini` sets `data_dir = ../webapp`, so the image is built
straight from `webapp/` — no copying, nothing to keep in sync. Skip `uploadfs` and the
device answers `/api/*` fine but serves a 404 at `/`, i.e. the URL on the display opens
nothing. Serial says `[net] LittleFS mount failed — run 'pio run -t uploadfs'` when this
has happened. Re-run `uploadfs` on its own any time you edit `webapp/`.

## 3. First boot — two ways to provision

With no Wi-Fi credentials stored, `setup()` first listens on the same USB-serial port for
20 seconds, then — if nothing arrived — falls back to a SoftAP.

**A — USB serial (used by `webflash/`, no second Wi-Fi network to join).** Any tool holding the
port open can send one line: a JSON object ending in `\n`, e.g.
`{"wifi_ssid":"home","wifi_pass":"...","device_pin":"1234"}` (`wifi_ssid` is the only required
field). The device replies with one of `>>>PROVISION_OK`, `>>>PROVISION_ERR:<reason>`, or —
after 20 s of silence — `>>>PROVISION_TIMEOUT`, then falls through to the SoftAP below. This is
exactly what the "Connect" + form on the flashing page does; a plain serial terminal (e.g.
`pio device monitor`, minicom, PuTTY) works the same way if you type the JSON line yourself
within the window.

**B — SoftAP captive portal:**

1. Join Wi-Fi network **`deskbuddy-setup`**.
2. Open `http://192.168.4.1`.
3. Fill in: Wi-Fi SSID + password, and a **device PIN** (this is the only thing standing between your camera and everyone else on your LAN — set it).
4. Save. The device reboots and joins your network.

Either path writes the same NVS fields via `settingsApplyJson()` (`storage.cpp`) — there is one
validator, not two.

On every boot after that, the **Status screen shows the device URL for 30 seconds**. That URL (or `http://deskbuddy.local`) is what you open. Note it down once — it's also on your router's DHCP table.

## 4. Settings

Open the device URL, go to Settings, and fill in whatever you want to use:

- **OpenRouter key** and/or **Gemini key** — for the AI screen. Either alone is enough.
- **Google Drive** — client ID, client secret, refresh token. See `docs/get_drive_token.md` for how to get these; it's a one-time GCP project setup.
- **Video** — resolution, fps, quality. Defaults are 800x600 @ 10 fps, quality 12. Push these up only if your SD card keeps up; the recorder reports dropped frames on the Cam screen.
- **Timezone offset** — the clock uses NTP in UTC plus this.

Keys are stored in NVS on the device and come back **masked** when read. Saving is a *partial* update: a field you leave untouched stays as it is, and an explicitly emptied field is cleared.

## 5. Google Drive

Follow `docs/get_drive_token.md` end to end. Short version: create a GCP project, enable the Drive API, make an OAuth client (Desktop app), consent once in a browser, and exchange the resulting code for a **refresh token**. Paste the client ID, client secret, and refresh token into Settings.

Uploads land in `DeskBuddy/YYYYMMDD/`. Segments upload only when Wi-Fi and SD load allow — recording always takes priority, so on a busy device uploads lag behind. That is by design.

## 6. The web app

Already flashed — step 2 put it in the device's flash, and the device serves it at `/`.
Open the URL from the display (`http://<ip>` or `http://deskbuddy.local`) and it is there,
same origin as `/api/*`, so there is no address to type: the app defaults to its own origin.
You only need the PIN.

Hosting the files somewhere else is optional and second-class — see `docs/DEPLOY.md`. An
HTTPS host (Vercel) cannot call your device's plain-HTTP LAN address at all; browsers block
it as mixed content.

Install it: Chrome/Android "Add to home screen", or the install icon in the desktop address bar.

## 7. Verify it actually works

Run `test/TEST_PLAN.md`. At minimum before you trust it with anything:

- **T1** — fresh flash reaches the setup portal and then your Wi-Fi.
- **T2** — record for 24 h, then play the AVIs in VLC and check for gaps. This is the whole point of the device; do not skip it.
- **T7** — 72 h soak. Watch the heap floor and the watchdog restart count.

## Troubleshooting

| Symptom | First thing to check |
|---|---|
| Boot loop, camera init fails | PSRAM not enabled (`qio_opi` in `platformio.ini`), or camera ribbon not seated |
| Records for a while then stops | SD card too slow or failing — check dropped-frame count on the Cam screen |
| Segments have gaps | Card write speed; drop resolution or fps in Settings |
| No display, everything else fine | SPI pins in `config.h`, and that the panel is actually ST7789 and not ILI9341 |
| Touch fires twice per tap, or on its own | Shorten the touch wire; tune `TOUCH_THRESHOLD` in `config.h` against `touchRead()` values from the serial monitor |
| PWA can't reach the device | Same LAN? Correct PIN? Some phones' private-DNS/VPN blocks `.local` — use the raw IP |
| Uploads never happen | Refresh token expired (unverified GCP apps expire them in 7 days — publish the consent screen) |

## Known limits (v1, by design — see HANDOVER.md section 10)

LAN only, no remote access. No motion detection. Audio, when enabled, is a separate `.wav` per segment, not muxed into the video. No OTA updates — reflash over USB.
