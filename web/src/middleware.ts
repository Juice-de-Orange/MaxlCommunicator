/*
 * Everything except the login page and the ingest endpoint needs a session.
 *
 * /api/ingest is deliberately outside this. CLAUDE.md 4.3: it "authenticates a
 * device-bridge pair, not a web session", and it does that itself with a bearer
 * token. Putting a cookie check in front of it would break every non-browser
 * client, which is the one thing the bridge protocol is written to avoid.
 */

import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

import { defineMiddleware } from "astro:middleware";

import { loadDotEnv } from "./db/env";
import { hasValidSession } from "./lib/auth";

/*
 * `.env`, once, before the first request is handled -- this module is loaded
 * ahead of every page and endpoint, and they all read `process.env`.
 *
 * Without it `npm run dev` answered 500 "SESSION_SECRET is not set" with a
 * filled-in `.env` sitting next to it: only `db:migrate` and the tests loaded
 * the file, so the suite was green and the documented flow in web/README.md was
 * not. Relative to the working directory rather than to this file, because in
 * the built server this file is a chunk somewhere under dist/.
 *
 * The container has no `.env` and gets its variables from compose; a value that
 * is already set is never overwritten.
 */
loadDotEnv(pathToFileURL(resolve(process.cwd(), ".env")));

/*
 * Paths that authenticate themselves.
 *
 * /api/ingest and /api/config are the bridge's, and CLAUDE.md 4.3 has them
 * authenticate a device-bridge pair rather than a web session -- a cookie check
 * in front of either breaks every non-browser client, which is the one thing the
 * bridge protocol is written to avoid.
 *
 * /api/config carries both directions: POST checks the dashboard session itself
 * and GET checks the device's ingest token. Listing it here does not make it
 * open; it makes the endpoint responsible for its own door, which it is.
 *
 * Found the hard way. With /api/config behind this middleware, GET answered 302
 * to the login page and the bridge could never collect a pending config -- and
 * the route tests did not see it, because they call the handler directly.
 */
const OPEN_PATHS = new Set(["/login", "/api/login", "/api/ingest", "/api/config"]);

/*
 * The PWA. It is served from this origin because Web Bluetooth needs a secure
 * context and a cross-origin ingest POST would buy nothing -- but it is not part
 * of the dashboard and must not sit behind the dashboard's password.
 *
 * It carries no data of its own: what it holds comes from a node over BLE and
 * goes to the server under a device token. Requiring a dashboard login to open
 * it would mean logging into an admin surface on a phone in the field to use a
 * radio, which is the wrong shape entirely.
 */
const OPEN_PREFIXES = ["/app/", "/app"];

export const onRequest = defineMiddleware(async (context, next) => {
  const path = context.url.pathname;
  if (OPEN_PATHS.has(path) || path.startsWith("/_") || OPEN_PREFIXES.some((p) => path === p || path.startsWith(p))) {
    return next();
  }

  const secret = process.env.SESSION_SECRET;
  if (!secret) {
    // Failing closed. A dashboard that silently serves everything because a
    // variable is missing is worse than one that refuses to start.
    return new Response(
      "SESSION_SECRET is not set. See web/.env.example.",
      { status: 500, headers: { "content-type": "text/plain; charset=utf-8" } },
    );
  }

  if (!hasValidSession(context.request.headers.get("cookie"), secret)) {
    return context.redirect("/login");
  }
  return next();
});
