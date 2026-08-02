# Device API — FROZEN CONTRACT v1

Expansion of HANDOVER.md section 6. **This file is the contract between A1/A2/A3/A4.**
Any change requires updating this file in the same commit and flagging Opus.

- Base URL: `http://<device-ip>` (also `http://deskbuddy.local`), port 80.
- All JSON is `Content-Type: application/json`, UTF-8, no BOM.
- All timestamps are Unix epoch seconds (UTC, integer) unless the field name ends in `_iso`.
- Unknown fields in requests are ignored. Clients MUST ignore unknown response fields (forward compat).
- **The PWA is served by this device**, from LittleFS at `/`, same origin as `/api/*`
  (HANDOVER section 3, revised). An HTTPS page cannot call a plain-HTTP LAN address, so
  hosting the app anywhere else is a browser-security dead end.
- CORS: device sends `Access-Control-Allow-Origin: *` and
  `Access-Control-Allow-Headers: Content-Type, X-Device-Pin` on every response, and answers
  `OPTIONS` on every path with `204`. Same-origin makes this unnecessary for the shipped
  app; it is kept so the optional static-host setup in `docs/DEPLOY.md` and the local mock
  server still work. **CORS is not the access control — the PIN is.**

---

## 1. Authentication

Scheme: a shared PIN set during setup, sent as a header on every request.

```
X-Device-Pin: 482913
```

- PIN is 4–12 characters, stored in NVS (`Settings.devicePin`). Compared as an exact string.
- If `Settings.devicePin` is **empty**, auth is disabled entirely (fresh device / user opted out).
  The device advertises this via `status.pin_required = false` so the PWA can hide the PIN field.
- No sessions, no cookies, no tokens. LAN-only device (HANDOVER section 10).

### Exempt endpoints

| Endpoint | Exempt | Why |
|---|---|---|
| `GET /api/status` | always | PWA must show reachability before the user has typed a PIN |
| `OPTIONS *` | always | CORS preflight cannot carry the header |
| `GET /` and the PWA's static files | always | the app shell is *how* the user enters the PIN; it is the same HTML/JS that is public in this repo and it exposes no device data |
| `POST /api/setup` | only in SoftAP setup mode | initial provisioning |
| everything else | never | |

### `?pin=` on browser-native URLs

`GET /api/snapshot`, `GET /api/stream` and `GET /api/recordings/file` are loaded by
`<img src>` and `<a download>`, which cannot set a request header. These three — and only
these three — accept `?pin=<pin>` as an exact alternative to `X-Device-Pin` (the same
mechanism `/ws` already uses). Either one satisfies auth; neither is preferred.

Every other endpoint stays header-only, so the PIN does not end up in `Referer` headers
and proxy logs on requests that had no need to put it there.

Once the device has Wi-Fi credentials and is in station mode, the setup-portal exemptions are gone —
`POST /api/setup` returns `404` and normal `POST /api/settings` (PIN-protected) is the only way in.

### 401 response

Missing or wrong header on a protected endpoint:

```http
HTTP/1.1 401 Unauthorized
Content-Type: application/json
```
```json
{ "error": "unauthorized", "message": "Missing or invalid X-Device-Pin" }
```

The device does not distinguish "missing" from "wrong" in the body, and adds no rate limiting in v1
(ponytail ceiling: add a 1 s penalty delay after 5 failures if this ever leaves the LAN).

---

## 2. Error format

Every non-2xx response (except the raw-binary endpoints, which may close the stream instead) has this body:

```json
{ "error": "<machine_code>", "message": "<human string>" }
```

| HTTP | `error` | When |
|---|---|---|
| 400 | `bad_request` | malformed JSON, missing required field, value out of range |
| 401 | `unauthorized` | PIN missing/wrong |
| 404 | `not_found` | unknown path, or a recording path that does not exist |
| 409 | `busy` | `/api/stream` already has a client; `/api/ai` already has a request in flight |
| 500 | `internal` | unexpected firmware failure |
| 503 | `unavailable` | subsystem not ready (camera init failed, SD not mounted, no Wi-Fi for AI/upload) |

Secrets are NEVER echoed in `message`.

---

## 3. Endpoints

### 3.1 `GET /api/status`

Auth: **exempt**. This is the health/reachability probe.

Response `200`:

```json
{
  "device": "deskbuddy",
  "fw": "1.0.0",
  "uptime_s": 84213,
  "time": 1785631200,
  "time_iso": "2026-08-02T00:40:00Z",
  "tz_offset_min": 330,
  "time_synced": true,
  "pin_required": true,
  "setup_mode": false,
  "heap_free": 142384,
  "heap_min": 98120,
  "psram_free": 3812048,
  "wifi": {
    "connected": true,
    "ssid": "home-2g",
    "ip": "192.168.1.42",
    "rssi": -58
  },
  "sd": {
    "mounted": true,
    "total_mb": 61035,
    "used_mb": 20418,
    "free_mb": 40617
  },
  "recording": {
    "active": true,
    "fps": 9.8,
    "current_file": "/rec/20260802/003500.avi",
    "segment_elapsed_s": 142,
    "segments_today": 178,
    "dropped_frames": 3
  },
  "upload": {
    "enabled": true,
    "queued": 4,
    "uploading": "/rec/20260802/003000.avi",
    "progress_pct": 62,
    "last_ok": 1785630710,
    "last_error": null
  },
  "ai": { "enabled": true, "provider": "openrouter", "busy": false },
  "reboots": 2
}
```

Notes for implementers:
- `psram_free`, `heap_min` come from `ESP.getFreePsram()` / `esp_get_minimum_free_heap_size()`.
- Any subsystem that is disabled at compile time reports `"enabled": false` and zeroed fields; it is never omitted.
- `upload.uploading` and `upload.last_error` are `null` when idle / no error.
- In SoftAP setup mode this endpoint still answers, with `setup_mode: true`, `wifi.connected: false`,
  and camera/SD/upload blocks present but possibly `mounted:false` / `active:false`.

---

### 3.2 `GET /api/snapshot`

Auth: required — header **or** `?pin=` (section 1).
Returns the most recent full JPEG frame from the ring buffer (no extra capture, no blocking of camTask).

Response `200`:
```
Content-Type: image/jpeg
Content-Length: <n>
Cache-Control: no-store
X-Timestamp: 1785631200
```
Body: raw JPEG bytes.

Errors: `401`, `503` (`{"error":"unavailable","message":"camera not ready"}`).

---

### 3.3 `GET /api/stream`

Auth: required — header **or** `?pin=` (section 1). **Max 1 concurrent client** (`STREAM_MAX_CLIENTS`), served at `STREAM_FPS` (5) from the same ring buffer.

Response `200`:
```
Content-Type: multipart/x-mixed-replace; boundary=frame
```
Each part:
```
--frame\r\n
Content-Type: image/jpeg\r\n
Content-Length: <n>\r\n
X-Timestamp: <epoch>\r\n
\r\n
<jpeg bytes>\r\n
```

Errors: `401`; `409` `{"error":"busy","message":"stream already in use"}`; `503` camera not ready.
The device closes the stream (no trailer) if recording needs the bandwidth — clients must reconnect.

---

### 3.4 `POST /api/ai`

Auth: required. Queue depth 1 (HANDOVER section 4) — a second request while one is in flight gets `409`.

Request:
```json
{ "prompt": "what is on my desk?", "provider": "openrouter" }
```
| field | type | required | notes |
|---|---|---|---|
| `prompt` | string | yes | 1–2000 chars |
| `provider` | `"openrouter"` \| `"gemini"` | no | defaults to `Settings.aiProvider` |
| `stream` | bool | no | default `false`. `true` => answer chunks arrive over `/ws` instead. **Clients should always send `true`** — see below |

> **`stream:false` is a trap, and the default only for backward compatibility.** Blocking
> mode parks the device's single AsyncTCP task until the provider answers, up to
> `AI_TIMEOUT_MS` (20 s). Every other HTTP request — snapshot, stream, status, settings —
> queues behind it for that whole window. (`/ws` pushes still go out; they are posted from
> netTask.) Recording is unaffected either way, it lives on the other core. The shipped PWA
> always sends `stream:true`; `stream:false` exists for `curl` and for T5.1.

Response `200` (blocking mode, `stream:false`):
```json
{
  "id": "a7f3",
  "provider": "openrouter",
  "answer": "A keyboard, a mug, and a small green plant.",
  "elapsed_ms": 4820
}
```

Response `202` (streaming mode, `stream:true`) — body is only the handle; text arrives as
`ai_chunk` / `ai_done` WebSocket messages carrying the same `id`:
```json
{ "id": "a7f3", "provider": "openrouter", "streaming": true }
```

Errors:
- `400` empty/oversized prompt, unknown provider.
- `409` `{"error":"busy","message":"ai request already in flight"}`.
- `503` `{"error":"unavailable","message":"no api key for provider gemini"}` or no Wi-Fi.
- `502` `{"error":"internal","message":"provider returned 401"}` — upstream failure. The upstream key is never quoted back.

---

### 3.5 `GET /api/recordings`

Auth: required.

Query params (all optional):
| param | type | default | meaning |
|---|---|---|---|
| `day` | `YYYYMMDD` | — | omit to list days; supply to list that day's segments |
| `limit` | int | 200 | max entries returned |

Day list — `GET /api/recordings`:
```json
{
  "days": [
    { "day": "20260802", "segments": 178, "bytes": 3612884992 },
    { "day": "20260801", "segments": 288, "bytes": 5891203072 }
  ],
  "sd_free_mb": 40617
}
```

Segment list — `GET /api/recordings?day=20260802`:
```json
{
  "day": "20260802",
  "segments": [
    { "name": "003000.avi", "path": "/rec/20260802/003000.avi", "bytes": 20340736,
      "start": 1785630600, "duration_s": 300, "uploaded": true },
    { "name": "003500.avi", "path": "/rec/20260802/003500.avi", "bytes": 8912384,
      "start": 1785630900, "duration_s": 142, "uploaded": false }
  ],
  "truncated": false
}
```
`uploaded` reflects the uploader's persisted state; `duration_s` for the in-progress segment is elapsed so far.

Errors: `401`; `404` unknown `day`; `503` SD not mounted.

---

### 3.6 `GET /api/recordings/file?path=<path>`

Auth: required — header **or** `?pin=` (section 1). Downloads one segment from SD.
`path` must start with `/rec/` (traversal rejected with `400`).

Response `200`: `Content-Type: video/x-msvideo`, `Content-Length`, `Accept-Ranges: bytes`
(single `Range: bytes=a-b` supported → `206`). Errors: `400`, `401`, `404`, `503`.

---

### 3.7 `GET /api/settings`

Auth: required. **Secrets are masked** — never returned in full (T8).

Masking rule (`maskSecret()` in `storage.cpp`):

1. Empty string stays `""`.
2. **8 characters or fewer → masked entirely**: one `*` per character, nothing preserved.
   A 4–6 character PIN has no "safe" tail; showing its last 4 hands over most of it.
3. **9 characters or more** → last 4 preserved, everything before them replaced with `*`,
   total length capped at 12 so the response does not leak the real length either.

| input | output |
|---|---|
| `""` | `""` |
| `482913` (6) | `******` |
| `hunter22` (8) | `********` |
| `hunter2hunter2` (14) | `********ter2` |
| `sk-or-v1-abcdef123456` (21) | `********3456` |

Response `200`:
```json
{
  "wifi_ssid": "home-2g",
  "wifi_pass": "********ter2",
  "device_pin": "******",
  "openrouter_key": "********3456",
  "gemini_key": "",
  "drive_refresh_token": "********mNo1",
  "drive_client_id": "8123-abc.apps.googleusercontent.com",
  "drive_client_secret": "********xY7q",
  "drive_folder_id": "1AbCdEfGhIjKlMnOp",
  "ai_provider": "openrouter",
  "video": { "width": 800, "height": 600, "fps": 10, "quality": 12 },
  "tz_offset_min": 330
}
```
`drive_client_id` and `drive_folder_id` are not secrets and are returned in full.

---

### 3.8 `POST /api/settings`

Auth: required. **Partial update** — only the keys present are written. Sending `""` clears a field;
omitting it leaves it unchanged. This is what makes the masked GET safe to round-trip: the PWA sends
back only the fields the user actually edited.

Request (every field optional):
```json
{
  "wifi_ssid": "home-2g",
  "wifi_pass": "hunter2hunter2",
  "device_pin": "482913",
  "openrouter_key": "sk-or-v1-...",
  "gemini_key": "",
  "drive_refresh_token": "1//0g...",
  "drive_client_id": "8123-abc.apps.googleusercontent.com",
  "drive_client_secret": "GOCSPX-...",
  "drive_folder_id": "1AbCdEfGhIjKlMnOp",
  "ai_provider": "gemini",
  "video": { "width": 800, "height": 600, "fps": 10, "quality": 12 },
  "tz_offset_min": 330
}
```

Validation (`400` on failure, nothing is written if any field fails):
| field | rule |
|---|---|
| `wifi_ssid` | ≤ 32 chars |
| `wifi_pass` | ≤ 63 chars |
| `device_pin` | `""` or 4–12 chars |
| `ai_provider` | `"openrouter"` or `"gemini"` |
| `video.width`/`height` | one of 320x240, 640x480, 800x600, 1024x768, 1600x1200 |
| `video.fps` | 1–20 |
| `video.quality` | 10–63 |
| `tz_offset_min` | -720 … 840 |

Response `200`:
```json
{ "ok": true, "saved": ["wifi_pass","ai_provider"], "reboot_required": true }
```
`reboot_required` is `true` when Wi-Fi credentials or video geometry changed (those need a restart);
the device does **not** reboot itself — the client calls `POST /api/reboot`.

---

### 3.9 `POST /api/reboot`

Auth: required. Body ignored.

Response `200` `{ "ok": true, "rebooting_in_ms": 500 }` then `esp_restart()`.

---

### 3.10 `POST /api/setup` (SoftAP setup mode only)

Auth: **exempt while in setup mode**; returns `404` in station mode.
Same body and validation as `POST /api/settings`, but `wifi_ssid` is required.

Response `200` `{ "ok": true, "rebooting_in_ms": 1500 }` — the device saves and reboots into station mode.

The captive portal also serves `GET /` (the minimal HTML form) and answers the OS probe paths
(`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`) with a `302` to `/`.

---

## 4. WebSocket `/ws`

- URL: `ws://<device-ip>/ws`. Auth: the PIN is passed as a query param because browsers cannot set
  headers on a WebSocket handshake: `ws://<ip>/ws?pin=482913`. Wrong/missing PIN (when one is set) →
  the handshake is accepted and the socket is immediately closed with code `4401`.
- Server → client only. Client messages are ignored in v1 (ponytail ceiling: add a `subscribe` message
  if per-client filtering is ever needed).

### Envelope

Every frame is one JSON object:
```json
{ "type": "<string>", "ts": 1785631200, "payload": { } }
```
`ts` is device epoch seconds. Clients MUST ignore unknown `type` values.

### `status` — every 5 s (`STATUS_PUSH_MS`)
`payload` is byte-for-byte the same object as `GET /api/status`.
```json
{ "type": "status", "ts": 1785631200, "payload": { "device": "deskbuddy", "uptime_s": 84213, "...": "see 3.1" } }
```

### `ai_chunk` — streaming answer fragment
```json
{ "type": "ai_chunk", "ts": 1785631204,
  "payload": { "id": "a7f3", "seq": 3, "text": " and a small green" } }
```
`seq` starts at 0 and increments per chunk for the same `id`; concatenating `text` in `seq` order
reproduces the answer. No chunk is ever re-sent.

### `ai_done` — answer finished (always sent, success or failure)
```json
{ "type": "ai_done", "ts": 1785631206,
  "payload": { "id": "a7f3", "provider": "openrouter", "chunks": 12,
               "elapsed_ms": 4820, "error": null } }
```
On failure: `"error": "provider returned 401"` and the client should discard the partial answer.

### `upload_event`
```json
{ "type": "upload_event", "ts": 1785631210,
  "payload": { "event": "progress", "file": "/rec/20260802/003000.avi",
               "progress_pct": 62, "queued": 4, "drive_file_id": null, "error": null } }
```
`event` ∈ `queued` | `started` | `progress` | `done` | `failed`.
`drive_file_id` is set only on `done`. `error` is set only on `failed`.
`progress` is emitted at most once per 10 % or 5 s, whichever is rarer.

### `error` — asynchronous device-level problem with no request to attach it to
```json
{ "type": "error", "ts": 1785631300,
  "payload": { "source": "recorder", "code": "sd_write_failed",
               "message": "write error on /rec/20260802/004000.avi", "fatal": false } }
```
`source` ∈ `camera` | `recorder` | `uploader` | `ai` | `wifi` | `sd` | `system`.
`fatal: true` means a restart is imminent (watchdog / unrecoverable) — the PWA should show a banner.
Messages here are user-safe: no keys, no tokens.

---

## 5. Quick reference

| Method | Path | Auth | Returns |
|---|---|---|---|
| GET | `/` + static files | exempt | the PWA, from LittleFS |
| GET | `/api/status` | exempt | JSON status |
| GET | `/api/snapshot` | PIN header **or** `?pin=` | `image/jpeg` |
| GET | `/api/stream` | PIN header **or** `?pin=` | `multipart/x-mixed-replace` |
| POST | `/api/ai` | PIN | JSON answer or `202` + WS stream |
| GET | `/api/recordings` | PIN | JSON day/segment list |
| GET | `/api/recordings/file` | PIN header **or** `?pin=` | `video/x-msvideo` |
| GET | `/api/settings` | PIN | JSON, secrets masked |
| POST | `/api/settings` | PIN | JSON `{ok, saved, reboot_required}` |
| POST | `/api/reboot` | PIN | JSON `{ok}` |
| POST | `/api/setup` | setup mode only | JSON `{ok}` |
| WS | `/ws?pin=` | PIN (query) | status / ai_chunk / ai_done / upload_event / error |
