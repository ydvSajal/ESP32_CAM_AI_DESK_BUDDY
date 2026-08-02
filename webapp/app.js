/* Desk Buddy PWA — vanilla, no build step. docs/API.md is the contract. */
'use strict';

/* ---------------------------------------------------------------- pure fns
   Everything in DB is pure and unit-tested by test/webapp/test.html. */

var SECRET_FIELDS = ['wifi_pass', 'device_pin', 'openrouter_key', 'gemini_key',
  'drive_refresh_token', 'drive_client_secret'];

var DB = {
  SECRET_FIELDS: SECRET_FIELDS,

  /* "192.168.1.42" | "deskbuddy.local:80/" | "http://x/" -> "http://x" */
  normalizeBase: function (s) {
    s = String(s == null ? '' : s).trim();
    if (!s) return '';
    if (!/^https?:\/\//i.test(s)) s = 'http://' + s;
    try {
      var u = new URL(s);
      if (!u.hostname) return '';
      return u.protocol + '//' + u.host;
    } catch (e) { return ''; }
  },

  /* http://x -> ws://x/ws?pin=... (pin omitted when empty) */
  wsUrl: function (base, pin) {
    base = DB.normalizeBase(base);
    if (!base) return '';
    var u = base.replace(/^http/i, 'ws') + '/ws';
    return pin ? u + '?pin=' + encodeURIComponent(pin) : u;
  },

  apiUrl: function (base, path, params) {
    var u = DB.normalizeBase(base) + path;
    var q = [];
    for (var k in params || {}) if (params[k] !== undefined && params[k] !== null && params[k] !== '')
      q.push(encodeURIComponent(k) + '=' + encodeURIComponent(params[k]));
    return q.length ? u + '?' + q.join('&') : u;
  },

  fmtBytes: function (n) {
    if (typeof n !== 'number' || !isFinite(n) || n < 0) return '–';
    var u = ['B', 'KB', 'MB', 'GB', 'TB'], i = 0;
    while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
    return (i === 0 ? n : n.toFixed(1)) + ' ' + u[i];
  },

  fmtMB: function (mb) {
    return typeof mb === 'number' && isFinite(mb) ? DB.fmtBytes(mb * 1024 * 1024) : '–';
  },

  fmtUptime: function (s) {
    if (typeof s !== 'number' || !isFinite(s) || s < 0) return '–';
    s = Math.floor(s);
    var d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600),
      m = Math.floor(s % 3600 / 60), sec = s % 60;
    if (d) return d + 'd ' + h + 'h ' + m + 'm';
    if (h) return h + 'h ' + m + 'm';
    if (m) return m + 'm ' + sec + 's';
    return sec + 's';
  },

  /* device epoch + tz offset -> readable local-to-device time */
  fmtDeviceTime: function (epoch, tzOffsetMin) {
    if (typeof epoch !== 'number' || !isFinite(epoch)) return '–';
    var d = new Date((epoch + (tzOffsetMin || 0) * 60) * 1000);
    return d.toISOString().replace('T', ' ').slice(0, 19) +
      ' (UTC' + (tzOffsetMin >= 0 ? '+' : '-') +
      Math.floor(Math.abs(tzOffsetMin || 0) / 60) + ')';
  },

  /* Partial-update builder for POST /api/settings (API.md 3.8).
     form:     what the inputs currently hold
     original: what GET /api/settings returned (secrets masked)
     Rules: unchanged field -> omitted; "" on a previously-set field -> sent as ""
     (clear); a secret still equal to its mask -> omitted (user did not touch it). */
  diffSettings: function (form, original) {
    var out = {}, orig = original || {};
    Object.keys(form || {}).forEach(function (k) {
      var v = form[k], o = orig[k];
      if (k === 'video') {
        var ov = o || {};
        if (!v) return;
        if (['width', 'height', 'fps', 'quality'].some(function (p) { return Number(v[p]) !== Number(ov[p]); }))
          out.video = { width: +v.width, height: +v.height, fps: +v.fps, quality: +v.quality };
        return;
      }
      if (k === 'tz_offset_min') { if (Number(v) !== Number(o)) out[k] = Number(v); return; }
      v = v == null ? '' : String(v);
      o = o == null ? '' : String(o);
      // ponytail: a secret typed to exactly its own mask reads as "unchanged".
      // Ceiling: needs a per-field dirty flag to fix; not worth it for 6 fields.
      if (v === o) return;
      out[k] = v;
    });
    return out;
  },

  /* Reconnect backoff: 1,2,4,8,16,30,30... seconds (ms). */
  backoffMs: function (attempt) {
    return Math.min(30000, 1000 * Math.pow(2, Math.max(0, attempt | 0)));
  },

  /* WS envelope router. Unknown types are ignored (API.md 4). Returns the
     type it dispatched, or null. */
  dispatchWs: function (raw, handlers) {
    var msg;
    try { msg = typeof raw === 'string' ? JSON.parse(raw) : raw; }
    catch (e) { return null; }
    if (!msg || typeof msg.type !== 'string') return null;
    var h = (handlers || {})[msg.type];
    if (typeof h !== 'function') return null;
    h(msg.payload || {}, msg.ts);
    return msg.type;
  },

  /* Turns any failure into a short user-safe line. Never a stack trace. */
  errText: function (e) {
    if (!e) return 'Unknown error';
    if (e.status === 401) return 'Wrong or missing device PIN.';
    if (e.status === 409) return e.message || 'Device busy — try again in a moment.';
    if (e.status === 503) return e.message || 'That part of the device is not ready.';
    if (e.status === 404) return e.message || 'Not found on the device.';
    if (e.status) return (e.message || 'Device error') + ' (HTTP ' + e.status + ')';
    return 'Device unreachable — check the address and that you are on the same Wi-Fi.';
  }
};
if (typeof module !== 'undefined') module.exports = DB;

/* ------------------------------------------------------------------- app */

(function () {
  var $ = function (id) { return document.getElementById(id); };
  // no UI present (test page, or require()d from node) — pure functions only
  if (typeof document === 'undefined' || !$('conn')) return;

  var LS = { addr: 'db.addr', pin: 'db.pin' };
  /* The app normally ships in the device's own flash and is served at /, so the
     device address IS this page's origin — no address to ask for. Only the
     optional static-host path (docs/DEPLOY.md) needs the Device panel, and that
     one is served over https (Vercel), which cannot talk to the device anyway. */
  var selfHosted = location.protocol === 'http:';
  var base = localStorage.getItem(LS.addr) || (selfHosted ? location.origin : '');
  var pin = localStorage.getItem(LS.pin) || '';
  var ws = null, wsAttempt = 0, wsTimer = null, pollTimer = null;
  var settingsOriginal = null, streaming = false, aiId = null;

  /* ---- transport ---- */
  function headers(extra) {
    var h = extra || {};
    if (pin) h['X-Device-Pin'] = pin;
    return h;
  }
  function api(path, opts) {
    opts = opts || {};
    if (!base) return Promise.reject({ message: 'No device address set.' });
    return fetch(DB.apiUrl(base, path, opts.params), {
      method: opts.method || 'GET',
      headers: headers(opts.body ? { 'Content-Type': 'application/json' } : {}),
      body: opts.body ? JSON.stringify(opts.body) : undefined,
      cache: 'no-store'
    }).then(function (r) {
      if (r.status === 401) { needPin(); }
      if (!r.ok) {
        return r.json().catch(function () { return {}; }).then(function (j) {
          throw { status: r.status, code: j.error, message: j.message };
        });
      }
      return r.json();          // every remaining call is JSON; binary goes via <img>/<a>
    }, function () { throw { message: null }; }); // network failure -> unreachable
  }

  function banner(text, bad) {
    var b = $('banner');
    if (!text) { b.hidden = true; return; }
    b.hidden = false; b.textContent = text;
    b.className = 'banner' + (bad === false ? ' ok' : '');
  }
  function needPin() {
    banner('Device PIN required or wrong. Enter it below.');
    openDeviceBar();
    $('f-pin').focus();
  }

  /* ---- device bar ---- */
  function openDeviceBar(open) {
    var on = open === undefined ? true : open;
    $('devicebar').hidden = !on;
    $('btn-device').setAttribute('aria-expanded', String(on));
  }
  $('btn-device').onclick = function () { openDeviceBar($('devicebar').hidden); };
  $('devicebar').onsubmit = function (e) {
    e.preventDefault();
    base = DB.normalizeBase($('f-addr').value);
    pin = $('f-pin').value;
    if (!base) { banner('That address does not look valid.'); return; }
    localStorage.setItem(LS.addr, base); localStorage.setItem(LS.pin, pin);
    banner('');
    openDeviceBar(false);
    connect();
  };
  $('btn-forget').onclick = function () {
    localStorage.removeItem(LS.addr); localStorage.removeItem(LS.pin);
    base = ''; pin = ''; $('f-addr').value = ''; $('f-pin').value = '';
    setConn(false, 'no device set');
  };

  /* ---- tabs ---- */
  Array.prototype.forEach.call(document.querySelectorAll('.tab'), function (t) {
    t.onclick = function () {
      Array.prototype.forEach.call(document.querySelectorAll('.tab'), function (o) {
        o.removeAttribute('aria-current');
        $('tab-' + o.dataset.tab).hidden = true;
      });
      t.setAttribute('aria-current', 'page');
      $('tab-' + t.dataset.tab).hidden = false;
      if (t.dataset.tab === 'settings' && !settingsOriginal) loadSettings();
      if (t.dataset.tab === 'rec') loadDays();
    };
  });

  /* ---- status ---- */
  function setConn(ok, label) {
    var c = $('conn');
    c.textContent = label;
    c.className = 'pill ' + (ok ? 'pill-ok' : 'pill-bad');
  }
  function renderStatus(s) {
    $('s-link').textContent = ws && ws.readyState === 1 ? 'WebSocket (live)' : 'polling /api/status';
    $('s-uptime').textContent = DB.fmtUptime(s.uptime_s);
    $('s-heap').textContent = DB.fmtBytes(s.heap_free) + ' (min ' + DB.fmtBytes(s.heap_min) + ')';
    $('s-psram').textContent = DB.fmtBytes(s.psram_free);
    var r = s.recording || {};
    $('s-rec').textContent = r.active
      ? '● REC ' + (r.fps || 0).toFixed(1) + ' fps — ' + (r.current_file || '') +
        ' (' + DB.fmtUptime(r.segment_elapsed_s) + ', ' + (r.segments_today || 0) + ' today, ' +
        (r.dropped_frames || 0) + ' dropped)'
      : 'stopped';
    $('s-rec').className = r.active ? 'rec-on' : '';
    var u = s.upload || {};
    $('s-queue').textContent = !u.enabled ? 'disabled'
      : (u.queued || 0) + ' queued' + (u.uploading ? ' — uploading ' + u.uploading + ' ' + (u.progress_pct || 0) + '%' : '') +
        (u.last_error ? ' — last error: ' + u.last_error : '');
    var w = s.wifi || {};
    $('s-wifi').textContent = w.connected ? w.ssid + ' @ ' + w.ip + ' (' + w.rssi + ' dBm)' : 'not connected';
    $('s-time').textContent = DB.fmtDeviceTime(s.time, s.tz_offset_min) + (s.time_synced ? '' : ' — NOT synced');
    $('s-fw').textContent = (s.device || '?') + ' ' + (s.fw || '') + ' · reboots ' + (s.reboots || 0) +
      (s.setup_mode ? ' · SETUP MODE' : '');
    var sd = s.sd || {};
    var pct = sd.total_mb ? Math.round(sd.used_mb / sd.total_mb * 100) : 0;
    $('s-sd').value = pct;
    $('s-sdtext').textContent = sd.mounted
      ? DB.fmtMB(sd.used_mb) + ' used of ' + DB.fmtMB(sd.total_mb) + ' (' + pct + '%) — ' + DB.fmtMB(sd.free_mb) + ' free'
      : 'SD not mounted';
    setConn(true, s.setup_mode ? 'setup mode' : 'connected');
    banner('');
  }

  /* ---- WS + polling fallback ---- */
  var wsHandlers = {
    status: renderStatus,
    ai_chunk: function (p) { if (p.id === aiId) $('ai-answer').textContent += p.text || ''; },
    ai_done: function (p) {
      if (p.id !== aiId) return;
      aiId = null; $('ai-send').disabled = false;
      if (p.error) $('ai-answer').textContent = 'AI failed: ' + p.error;
    },
    upload_event: function (p) {
      if (p.event === 'failed') banner('Upload failed: ' + (p.file || '') + ' — ' + (p.error || ''));
    },
    error: function (p) {
      banner('[' + p.source + '] ' + p.message + (p.fatal ? ' — device is restarting.' : ''));
    }
  };

  function connect() {
    stopWs();
    if (!base) { setConn(false, 'no device set'); openDeviceBar(true); return; }
    poll();               // immediate reachability check, also covers WS-down case
    openWs();
  }
  function stopWs() {
    clearTimeout(wsTimer);
    if (ws) { ws.onclose = null; ws.close(); ws = null; }
  }
  function openWs() {
    var url = DB.wsUrl(base, pin);
    if (!url) return;
    try { ws = new WebSocket(url); } catch (e) { scheduleWs(); return; }
    ws.onopen = function () { wsAttempt = 0; stopPolling(); setConn(true, 'connected'); };
    ws.onmessage = function (ev) { DB.dispatchWs(ev.data, wsHandlers); };
    ws.onclose = function (ev) {
      ws = null;
      if (ev.code === 4401) { needPin(); return; } // API.md 4: bad PIN on WS
      startPolling();
      scheduleWs();
    };
    ws.onerror = function () { /* onclose always follows */ };
  }
  function scheduleWs() {
    var ms = DB.backoffMs(wsAttempt++);
    clearTimeout(wsTimer);
    wsTimer = setTimeout(openWs, ms);
  }
  function startPolling() { if (!pollTimer) { pollTimer = setInterval(poll, 5000); poll(); } }
  function stopPolling() { clearInterval(pollTimer); pollTimer = null; }
  function poll() {
    api('/api/status').then(renderStatus, function (e) {
      setConn(false, 'device unreachable');
      banner(DB.errText(e));
    });
  }

  /* ---- live ----
     /api/snapshot and /api/stream accept ?pin= (API.md 3.2/3.3) precisely so the
     browser can load them natively. The browser decodes MJPEG itself — no fetch,
     no reader, no multipart parse, no blob, no object URLs to leak. */
  var img = $('live-img');
  function showUrl(path) {
    img.hidden = false;
    img.src = DB.apiUrl(base, path, { pin: pin, t: Date.now() });   // t = cache buster
  }
  img.onerror = function () {
    img.hidden = true;
    $('live-msg').textContent = streaming
      ? 'Stream failed — another viewer may already have it (only one allowed), or the camera is not ready.'
      : 'Snapshot failed — check the PIN and that the camera is ready.';
    if (streaming) stopStream();
  };
  img.onload = function () {
    if (!streaming) $('live-msg').textContent = 'Snapshot at ' + new Date().toLocaleTimeString();
  };
  $('btn-snap').onclick = function () {
    if (streaming) stopStream();
    $('live-msg').textContent = 'Fetching snapshot…';
    showUrl('/api/snapshot');
  };
  $('btn-stream').onclick = function () { streaming ? stopStream() : startStream(); };
  function startStream() {
    streaming = true;
    $('btn-stream').textContent = 'Stop stream';
    $('btn-stream').setAttribute('aria-pressed', 'true');
    $('live-msg').textContent = 'Streaming.';
    showUrl('/api/stream');
  }
  function stopStream() {
    streaming = false;
    img.removeAttribute('src');        // closes the connection, frees the device's one slot
    img.hidden = true;                 // (src='' would re-request the page and fire onerror)
    $('btn-stream').textContent = 'Start stream';
    $('btn-stream').setAttribute('aria-pressed', 'false');
  }
  /* ---- AI ---- */
  $('ai-form').onsubmit = function (e) {
    e.preventDefault();
    var prompt = $('ai-prompt').value.trim();
    if (!prompt) return;
    /* ALWAYS stream:true. stream:false parks the device's single AsyncTCP task for up
       to AI_TIMEOUT_MS and queues every other HTTP request behind it (API.md 3.4).
       The answer therefore arrives over /ws, so refuse to ask without one. */
    if (!ws || ws.readyState !== 1) {
      $('ai-answer').textContent = 'AI needs the live connection. Reconnecting — try again in a moment.';
      return;
    }
    $('ai-answer').textContent = '';
    $('ai-send').disabled = true;
    api('/api/ai', { method: 'POST', body: { prompt: prompt, provider: $('ai-provider').value, stream: true } })
      .then(function (j) {
        aiId = j.id;
      }, function (err) {
        $('ai-send').disabled = false;
        $('ai-answer').textContent = DB.errText(err);
      });
  };

  /* ---- recordings ---- */
  function loadDays() {
    api('/api/recordings').then(function (j) {
      $('rec-free').textContent = DB.fmtMB(j.sd_free_mb) + ' free';
      var ul = $('rec-days'); ul.textContent = '';
      (j.days || []).forEach(function (d) {
        var li = document.createElement('li'), b = document.createElement('button');
        b.textContent = d.day + ' — ' + d.segments + ' segments, ' + DB.fmtBytes(d.bytes);
        b.onclick = function () { loadSegs(d.day); };
        li.appendChild(b); ul.appendChild(li);
      });
      if (!ul.children.length) ul.textContent = 'No recordings yet.';
    }, function (e) { $('rec-days').textContent = DB.errText(e); });
  }
  $('btn-days').onclick = loadDays;
  function loadSegs(day) {
    $('rec-dayhead').hidden = false;
    $('rec-dayhead').textContent = 'Segments — ' + day;
    api('/api/recordings', { params: { day: day } }).then(function (j) {
      var ul = $('rec-segs'); ul.textContent = '';
      (j.segments || []).forEach(function (s) {
        var li = document.createElement('li');
        li.appendChild(document.createTextNode(s.name + ' · ' + DB.fmtBytes(s.bytes) + ' · ' +
          DB.fmtUptime(s.duration_s) + ' · ' + (s.uploaded ? 'uploaded' : 'on SD only') + ' '));
        /* Plain link: /api/recordings/file takes ?pin= (API.md 3.6), so the browser
           streams the ~20 MB segment straight to disk instead of us buffering it. */
        var a = document.createElement('a');
        a.textContent = 'Download';
        a.href = DB.apiUrl(base, '/api/recordings/file', { path: s.path, pin: pin });
        a.download = s.name;
        li.appendChild(a); ul.appendChild(li);
      });
      if (j.truncated) ul.appendChild(document.createTextNode('(list truncated)'));
      if (!ul.children.length) ul.textContent = 'No segments.';
    }, function (e) { $('rec-segs').textContent = DB.errText(e); });
  }
  /* ---- settings ---- */
  function loadSettings() {
    api('/api/settings').then(function (s) {
      settingsOriginal = s;
      ['wifi_ssid', 'wifi_pass', 'device_pin', 'openrouter_key', 'gemini_key',
        'drive_client_id', 'drive_client_secret', 'drive_refresh_token', 'drive_folder_id',
        'ai_provider', 'tz_offset_min'].forEach(function (k) {
          $('set-' + k).value = s[k] == null ? '' : s[k];
        });
      var v = s.video || {};
      $('set-video_size').value = v.width + 'x' + v.height;
      $('set-video_fps').value = v.fps;
      $('set-video_quality').value = v.quality;
      $('set-msg').textContent = 'Loaded. Secret fields show masked values — edit only what you want to change.';
    }, function (e) { $('set-msg').textContent = DB.errText(e); });
  }
  $('btn-reload').onclick = loadSettings;

  function formSettings() {
    var f = {};
    ['wifi_ssid', 'wifi_pass', 'device_pin', 'openrouter_key', 'gemini_key',
      'drive_client_id', 'drive_client_secret', 'drive_refresh_token', 'drive_folder_id',
      'ai_provider', 'tz_offset_min'].forEach(function (k) { f[k] = $('set-' + k).value; });
    var wh = $('set-video_size').value.split('x');
    f.video = { width: +wh[0], height: +wh[1], fps: +$('set-video_fps').value, quality: +$('set-video_quality').value };
    return f;
  }
  $('set-form').onsubmit = function (e) {
    e.preventDefault();
    if (!settingsOriginal) { $('set-msg').textContent = 'Load settings from the device first.'; return; }
    var body = DB.diffSettings(formSettings(), settingsOriginal);
    if (!Object.keys(body).length) { $('set-msg').textContent = 'Nothing changed — nothing sent.'; return; }
    if (body.device_pin !== undefined &&
      !confirm('This changes the device PIN to "' + body.device_pin + '". The app will use the new PIN. Continue?')) return;
    $('set-msg').textContent = 'Sending: ' + Object.keys(body).join(', ');
    api('/api/settings', { method: 'POST', body: body }).then(function (j) {
      $('set-msg').textContent = 'Saved: ' + (j.saved || []).join(', ') +
        (j.reboot_required ? ' — reboot required for these to take effect.' : '');
      if (body.device_pin !== undefined) {
        pin = body.device_pin; localStorage.setItem(LS.pin, pin); $('f-pin').value = pin; connect();
      }
      settingsOriginal = null;
      loadSettings();
    }, function (err) { $('set-msg').textContent = DB.errText(err); });
  };
  $('btn-reboot').onclick = function () {
    if (!confirm('Reboot the device now? Recording stops for ~15 seconds.')) return;
    api('/api/reboot', { method: 'POST' }).then(function () {
      banner('Reboot requested — reconnecting…');
      setTimeout(connect, 8000);
    }, function (e) { banner(DB.errText(e)); });
  };

  /* ---- boot ---- */
  $('f-addr').value = base;
  $('f-pin').value = pin;
  if (base) connect(); else { setConn(false, 'no device set'); openDeviceBar(true); }
  window.addEventListener('online', connect);
  window.addEventListener('offline', function () {
    setConn(false, 'offline'); banner('This phone/computer is offline. Device unreachable.');
  });
  if ('serviceWorker' in navigator) navigator.serviceWorker.register('sw.js').catch(function () {});
})();
