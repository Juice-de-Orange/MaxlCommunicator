/*
 * Application shell cache, and nothing else.
 *
 * CLAUDE.md 4.2: "Do not attempt to work around the foreground limitation with
 * service workers, wake locks or periodic background sync -- none of them keep a
 * GATT connection alive. Background sync is what Phase 9 is for."
 *
 * So there is no `sync` handler, no `periodicsync` handler and no fetch
 * interception of /api/ingest here. This worker exists for one reason: the app
 * opens when the phone has no network, which is the state it is usually in when
 * it matters. Adding anything else to this file is a design change, not an
 * improvement.
 */

const CACHE = "maxl-shell-v1";

self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(CACHE).then((cache) => cache.addAll(["./", "./index.html", "./manifest.webmanifest"])),
  );
  self.skipWaiting();
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches
      .keys()
      .then((keys) => Promise.all(keys.filter((key) => key !== CACHE).map((key) => caches.delete(key)))),
  );
  self.clients.claim();
});

self.addEventListener("fetch", (event) => {
  const request = event.request;
  // Never the API. A cached ingest response would tell the app the server has
  // events it does not have, and the app would then acknowledge the device's
  // journal on the strength of it.
  if (request.method !== "GET" || new URL(request.url).pathname.startsWith("/api/")) {
    return;
  }
  event.respondWith(
    fetch(request)
      .then((response) => {
        const copy = response.clone();
        caches.open(CACHE).then((cache) => cache.put(request, copy));
        return response;
      })
      .catch(() => caches.match(request).then((hit) => hit ?? Response.error())),
  );
});
