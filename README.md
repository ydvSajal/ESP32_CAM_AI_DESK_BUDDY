# ESP32-S3 Desk Buddy

One ESP32-S3 that records 24/7 to SD (5-min AVI segments, uploaded to Google Drive),
shows a clock/status/AI/camera screen on a TFT, answers AI questions, and is driven by
an installable PWA over your LAN. LAN only — no backend, no tunnels.

## Flash

```
cd firmware
pio run -t upload        # firmware
pio run -t uploadfs      # the PWA -> LittleFS (data_dir = ../webapp). Not optional.
pio device monitor
```

Both steps. Skip `uploadfs` and `/api/*` works but `/` is a 404 — the URL on the display
opens nothing. Re-run it alone after editing `webapp/`.

First boot with empty NVS: join the `deskbuddy-setup` Wi-Fi AP, open the captive portal,
enter Wi-Fi + device PIN + keys, save. Device reboots and prints its URL on the display.

## Where the plan lives

- **[HANDOVER.md](HANDOVER.md)** — architecture, core/task split, work split. The contract; do not redesign.
- **[docs/API.md](docs/API.md)** — the frozen device HTTP/WS API. Firmware and PWA both code against this.
- **[firmware/src/config.h](firmware/src/config.h)** — every pin, size, and interval. The only place hardware is defined.
- **docs/SETUP.md** — flashing + Google Drive OAuth walkthrough (written at integration).
