/* Fake Desk Buddy device implementing docs/API.md. `node webapp/mock-server.js`
   then open http://localhost:8080/ (PIN 482913). Dev-only, zero dependencies.
   ponytail: hand-rolled 25-line WS server (server->client frames only, no ping,
   no fragmentation). Ceiling: enough for docs/API.md; use `ws` if it ever needs more. */
const http = require('http'), fs = require('fs'), path = require('path'), crypto = require('crypto');
const PIN = process.env.MOCK_PIN === undefined ? '482913' : process.env.MOCK_PIN;
const ROOT = path.join(__dirname, '..');
const JPEG = Buffer.from('/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAABAAEDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/9oACAEBAAA/APn+iiigD//Z', 'base64');
const start = Date.now(), sockets = new Set();
let streamBusy = false, aiBusy = false;
let S = { wifi_ssid: 'home-2g', wifi_pass: 'hunter2hunter2', device_pin: PIN,
  openrouter_key: 'sk-or-v1-abcdef123456', gemini_key: '', drive_refresh_token: '1//0gAbCmNo1',
  drive_client_id: '8123-abc.apps.googleusercontent.com', drive_client_secret: 'GOCSPX-abcxY7q',
  drive_folder_id: '1AbCdEfGhIjKlMnOp', ai_provider: 'openrouter',
  video: { width: 800, height: 600, fps: 10, quality: 12 }, tz_offset_min: 330 };
/* API.md 3.7: <=8 chars is masked entirely (a 6-char PIN must not show 4 of them);
   longer keeps the last 4, total capped at 12. Mirrors storage.cpp maskSecret(). */
const mask = s => !s ? '' : s.length > 8
  ? '*'.repeat(Math.min(8, s.length - 4)) + s.slice(-4)
  : '*'.repeat(s.length);
const SECRETS = ['wifi_pass', 'device_pin', 'openrouter_key', 'gemini_key', 'drive_refresh_token', 'drive_client_secret'];

const status = () => ({ device: 'deskbuddy', fw: '1.0.0-mock',
  uptime_s: Math.floor((Date.now() - start) / 1000), time: Math.floor(Date.now() / 1000),
  time_iso: new Date().toISOString().replace(/\.\d+Z/, 'Z'), tz_offset_min: S.tz_offset_min,
  time_synced: true, pin_required: !!S.device_pin, setup_mode: false,
  heap_free: 142384 + (Date.now() % 9000 | 0), heap_min: 98120, psram_free: 3812048,
  wifi: { connected: true, ssid: S.wifi_ssid, ip: '127.0.0.1', rssi: -50 - (Date.now() / 1000 % 20 | 0) },
  sd: { mounted: true, total_mb: 61035, used_mb: 20418, free_mb: 40617 },
  recording: { active: true, fps: 9.8, current_file: '/rec/20260802/003500.avi',
    segment_elapsed_s: (Date.now() - start) / 1000 % 300 | 0, segments_today: 178, dropped_frames: 3 },
  upload: { enabled: true, queued: 4, uploading: '/rec/20260802/003000.avi', progress_pct: 62,
    last_ok: 1754092310, last_error: null },
  ai: { enabled: true, provider: S.ai_provider, busy: aiBusy }, reboots: 2 });

const send = (res, code, obj) => { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); };
const err = (res, code, e, m) => send(res, code, { error: e, message: m });

const srv = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type, X-Device-Pin');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  if (req.method === 'OPTIONS') return res.writeHead(204).end();
  const p = u.pathname;
  if (p === '/api/status') return send(res, 200, status());
  // API.md 1: these three are loaded by <img src>/<a download>, which cannot set a header,
  // so they take ?pin= too. Everything else stays header-only.
  const QUERY_PIN_OK = ['/api/snapshot', '/api/stream', '/api/recordings/file'];
  if (p.startsWith('/api/')) {
    const ok = !S.device_pin || req.headers['x-device-pin'] === S.device_pin ||
      (QUERY_PIN_OK.includes(p) && u.searchParams.get('pin') === S.device_pin);
    if (!ok) return err(res, 401, 'unauthorized', 'Missing or invalid X-Device-Pin');
  }
  if (p === '/api/snapshot')
    return res.writeHead(200, { 'Content-Type': 'image/jpeg', 'Cache-Control': 'no-store',
      'X-Timestamp': Math.floor(Date.now() / 1000) }).end(JPEG);
  if (p === '/api/stream') {
    if (streamBusy) return err(res, 409, 'busy', 'stream already in use');
    streamBusy = true;
    res.writeHead(200, { 'Content-Type': 'multipart/x-mixed-replace; boundary=frame' });
    const t = setInterval(() => {                                  // 5 fps, API.md 3.3
      res.write('--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ' + JPEG.length +
        '\r\nX-Timestamp: ' + Math.floor(Date.now() / 1000) + '\r\n\r\n');
      res.write(JPEG); res.write('\r\n');
    }, 200);
    res.on('close', () => { clearInterval(t); streamBusy = false; });
    return;
  }
  if (p === '/api/recordings') {
    const day = u.searchParams.get('day');
    if (!day) return send(res, 200, { days: [{ day: '20260802', segments: 178, bytes: 3612884992 },
      { day: '20260801', segments: 288, bytes: 5891203072 }], sd_free_mb: 40617 });
    if (!/^\d{8}$/.test(day) || day > '20260802') return err(res, 404, 'not_found', 'no such day');
    return send(res, 200, { day, truncated: false, segments: [
      { name: '003000.avi', path: `/rec/${day}/003000.avi`, bytes: 20340736, start: 1754092200, duration_s: 300, uploaded: true },
      { name: '003500.avi', path: `/rec/${day}/003500.avi`, bytes: 8912384, start: 1754092500, duration_s: 142, uploaded: false }] });
  }
  if (p === '/api/recordings/file') {
    const f = u.searchParams.get('path') || '';
    if (!f.startsWith('/rec/') || f.includes('..')) return err(res, 400, 'bad_request', 'bad path');
    return res.writeHead(200, { 'Content-Type': 'video/x-msvideo', 'Accept-Ranges': 'bytes' }).end(Buffer.alloc(4096, 1));
  }
  if (p === '/api/settings' && req.method === 'GET') {
    const o = Object.assign({}, S);
    SECRETS.forEach(k => o[k] = mask(S[k]));
    return send(res, 200, o);
  }
  let body = '';
  req.on('data', c => body += c);
  req.on('end', () => {
    let j = {}; try { j = JSON.parse(body || '{}'); } catch (e) { return err(res, 400, 'bad_request', 'bad json'); }
    if (p === '/api/settings' && req.method === 'POST') {
      if (j.ai_provider && !['openrouter', 'gemini'].includes(j.ai_provider))
        return err(res, 400, 'bad_request', 'ai_provider must be openrouter or gemini');
      const saved = Object.keys(j);
      Object.assign(S, j);
      return send(res, 200, { ok: true, saved,
        reboot_required: saved.some(k => ['wifi_ssid', 'wifi_pass', 'video'].includes(k)) });
    }
    if (p === '/api/ai' && req.method === 'POST') {
      if (!j.prompt || j.prompt.length > 2000) return err(res, 400, 'bad_request', 'prompt must be 1-2000 chars');
      const prov = j.provider || S.ai_provider;
      if (!['openrouter', 'gemini'].includes(prov)) return err(res, 400, 'bad_request', 'unknown provider');
      if (!S[prov === 'gemini' ? 'gemini_key' : 'openrouter_key'])
        return err(res, 503, 'unavailable', 'no api key for provider ' + prov);
      if (aiBusy) return err(res, 409, 'busy', 'ai request already in flight');
      const id = crypto.randomBytes(2).toString('hex');
      const words = ('You asked: "' + j.prompt + '". Mock answer: a keyboard, a mug, and a small green plant.').split(' ');
      if (!j.stream) return send(res, 200, { id, provider: prov, answer: words.join(' '), elapsed_ms: 120 });
      aiBusy = true;
      send(res, 202, { id, provider: prov, streaming: true });
      words.forEach((w, i) => setTimeout(() => push('ai_chunk', { id, seq: i, text: (i ? ' ' : '') + w }), 120 * i));
      setTimeout(() => { aiBusy = false; push('ai_done', { id, provider: prov, chunks: words.length, elapsed_ms: 120 * words.length, error: null }); }, 120 * words.length + 50);
      return;
    }
    if (p === '/api/reboot' && req.method === 'POST') return send(res, 200, { ok: true, rebooting_in_ms: 500 });
    if (p.startsWith('/api/')) return err(res, 404, 'not_found', 'unknown path');
    // static: webapp/ at / and the repo test/ tree at /test/
    const file = path.join(ROOT, p.startsWith('/test/') ? p : 'webapp' + (p === '/' ? '/index.html' : p));
    fs.readFile(file, (e, d) => e ? err(res, 404, 'not_found', 'no file') :
      res.writeHead(200, { 'Content-Type': { '.html': 'text/html', '.js': 'text/javascript',
        '.css': 'text/css', '.json': 'application/json', '.png': 'image/png' }[path.extname(file)] || 'text/plain' }).end(d));
  });
});

/* --- minimal WebSocket (RFC 6455) --- */
function frame(s, opcode = 0x81) {
  const b = Buffer.from(s); let h;
  if (b.length < 126) h = Buffer.from([opcode, b.length]);
  else if (b.length < 65536) { h = Buffer.alloc(4); h[0] = opcode; h[1] = 126; h.writeUInt16BE(b.length, 2); }
  else { h = Buffer.alloc(10); h[0] = opcode; h[1] = 127; h.writeBigUInt64BE(BigInt(b.length), 2); }
  return Buffer.concat([h, b]);
}
function push(type, payload) {
  const m = frame(JSON.stringify({ type, ts: Math.floor(Date.now() / 1000), payload }));
  sockets.forEach(s => s.writable && s.write(m));
}
srv.on('upgrade', (req, sock) => {
  const u = new URL(req.url, 'http://x');
  const accept = crypto.createHash('sha1')
    .update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n');
  if (S.device_pin && u.searchParams.get('pin') !== S.device_pin) {   // API.md 4: close 4401
    const c = Buffer.alloc(2); c.writeUInt16BE(4401);
    sock.write(frame(c, 0x88)); return sock.end();
  }
  sockets.add(sock);
  sock.on('close', () => sockets.delete(sock));
  sock.on('error', () => sockets.delete(sock));
  sock.write(frame(JSON.stringify({ type: 'status', ts: Math.floor(Date.now() / 1000), payload: status() })));
});
setInterval(() => push('status', status()), 5000);              // API.md STATUS_PUSH_MS
setInterval(() => push('upload_event', { event: 'progress', file: '/rec/20260802/003000.avi',
  progress_pct: Date.now() / 1000 % 100 | 0, queued: 4, drive_file_id: null, error: null }), 15000);

srv.listen(process.env.PORT || 8080, () =>
  console.log('mock device on http://localhost:' + (process.env.PORT || 8080) + '  PIN=' + (S.device_pin || '(none)')));
