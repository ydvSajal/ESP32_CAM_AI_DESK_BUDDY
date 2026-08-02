/* App-shell cache only. Device API responses are NEVER cached — the shell must
   load offline and then say "device unreachable" instead of showing stale data. */
var CACHE = 'deskbuddy-shell-v1';
var SHELL = ['./', './index.html', './app.js', './style.css', './manifest.json',
  './icon-192.png', './icon-512.png'];

self.addEventListener('install', function (e) {
  e.waitUntil(caches.open(CACHE).then(function (c) { return c.addAll(SHELL); }).then(function () {
    return self.skipWaiting();
  }));
});

self.addEventListener('activate', function (e) {
  e.waitUntil(caches.keys().then(function (keys) {
    return Promise.all(keys.filter(function (k) { return k !== CACHE; }).map(function (k) { return caches.delete(k); }));
  }).then(function () { return self.clients.claim(); }));
});

self.addEventListener('fetch', function (e) {
  var req = e.request;
  // Anything that is not our own shell (i.e. every device call) goes straight
  // to the network and is never cached.
  if (req.method !== 'GET' || new URL(req.url).origin !== self.location.origin) return;
  if (/\/(api|ws)\b/.test(req.url)) return;
  e.respondWith(caches.match(req, { ignoreSearch: true }).then(function (hit) {
    return hit || fetch(req).catch(function () { return caches.match('./index.html'); });
  }));
});
