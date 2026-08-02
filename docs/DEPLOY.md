# Deploying the web app

**The web app ships on the device.** There is nothing to deploy anywhere else.

## The only path that matters

```bash
cd firmware
pio run -t uploadfs      # builds the LittleFS image from webapp/ and flashes it
```

`platformio.ini` sets `data_dir = ../webapp`, so the filesystem image *is* the `webapp/`
folder — no copy step, no build step, nothing to keep in sync. Then open the URL the
device prints on its display (`http://<ip>` or `http://deskbuddy.local`) and the PWA is
right there, served from flash at `/`, same origin as `/api/*`.

Re-run `pio run -t uploadfs` after editing anything in `webapp/`. It is independent of
`pio run -t upload` (the firmware) — you can reflash either one alone.

## Why not Vercel

The original plan hosted the PWA on Vercel. It cannot work: Vercel serves HTTPS, the
device serves plain HTTP on your LAN, and every browser blocks HTTPS → HTTP as mixed
content. The user would have to switch off a browser security setting to look at their own
camera. Serving from the device makes everything same-origin, needs no internet at all,
and makes the URL on the display actually open the app — which is what the product
promised. (HANDOVER section 3, revised.)

## Optional: hosting the static files elsewhere

Still supported, still second-class. The device keeps its CORS headers, and the app keeps
the **Device** panel where you type the device address and PIN by hand (both persist in
`localStorage`).

- `npx serve webapp` on your laptop — plain HTTP, so it works, and it is a convenient dev
  loop. You still have to type the device address.
- `vercel deploy webapp --prod` — **HTTPS, so the device calls are blocked** unless you
  turn on Chrome's per-site "Insecure content: Allow". Do not send anyone here.
- `node webapp/mock-server.js` — a fake device on `http://localhost:8080` implementing
  `docs/API.md`, serving the app and the API from one origin. This is what `test/webapp`
  runs against. Dev only.

In every case the browser must be on the same LAN as the device: there is no backend and
no relay (HANDOVER section 10). Remote access is a Tailscale-on-your-router problem, not
this project's.
