'use strict';
// CrossPoint Cards service worker — makes the app usable with no internet.
// It precaches the app shell (HTML/CSS/JS/engine) so studying works fully
// offline; all study state and reconcile logic live in app.js over IndexedDB.
// API calls are never cached: the client talks to the network only to reconcile,
// and falls back to local data when offline.

const VERSION = 'v4';
const CACHE = `cp-shell-${VERSION}`;
const SHELL = [
  '/',
  '/index.html',
  '/app.js',
  '/engine.js',
  '/manifest.webmanifest',
  '/icon.svg',
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE).then((cache) => cache.addAll(SHELL)).then(() => self.skipWaiting()),
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
      .then(() => self.clients.claim()),
  );
});

// Lets the page tell a freshly-installed worker to take over immediately.
self.addEventListener('message', (event) => {
  if (event.data === 'skipWaiting') self.skipWaiting();
});

self.addEventListener('fetch', (event) => {
  const { request } = event;
  if (request.method !== 'GET') return; // mutations always hit the network

  const url = new URL(request.url);
  if (url.origin !== self.location.origin) return; // third-party: leave alone
  if (url.pathname.startsWith('/api/')) return; // never cache API; client handles offline

  // Stale-while-revalidate for the shell: respond from cache instantly (works
  // offline), refresh the cache from the network in the background. Navigations
  // fall back to the cached shell so any in-app route loads offline.
  event.respondWith(
    (async () => {
      const cache = await caches.open(CACHE);
      const cached = await cache.match(request, { ignoreSearch: true });
      const network = fetch(request)
        .then((res) => {
          if (res && res.ok && res.type === 'basic') cache.put(request, res.clone());
          return res;
        })
        .catch(() => null);

      if (cached) {
        event.waitUntil(network); // refresh in background
        return cached;
      }
      const fresh = await network;
      if (fresh) return fresh;
      if (request.mode === 'navigate') {
        return (await cache.match('/index.html')) || (await cache.match('/')) || Response.error();
      }
      return Response.error();
    })(),
  );
});
