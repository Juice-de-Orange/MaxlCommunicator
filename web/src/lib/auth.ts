/*
 * Two entirely separate things are authenticated here, and keeping them
 * separate is the point.
 *
 *   Ingest   CLAUDE.md 4.3: "the ingestion endpoint authenticates a
 *            device-bridge pair, not a web session". A bearer token per device,
 *            stored only as a hash. Nothing about a browser is involved, which
 *            is what lets the Kotlin client of phase 9 use the same endpoint
 *            without a login flow.
 *
 *   Dashboard  A single password from the environment, in a signed cookie. Two
 *            nodes and one operator do not need a user table, and a user table
 *            that exists is a user table that has to be got right.
 */

import { createHmac, randomBytes, timingSafeEqual, createHash } from "node:crypto";

const SESSION_COOKIE = "maxl_session";
const SESSION_TTL_S = 60 * 60 * 24 * 30;

/** Ingest tokens are stored as this and never in the clear. */
export function hashIngestToken(token: string): string {
  return createHash("sha256").update(token, "utf8").digest("hex");
}

export function generateIngestToken(): string {
  return randomBytes(24).toString("base64url");
}

/** Constant time, because a token comparison that leaks length or prefix is a
 *  token comparison that can be walked. */
export function safeEquals(a: string, b: string): boolean {
  const left = Buffer.from(a, "utf8");
  const right = Buffer.from(b, "utf8");
  if (left.length !== right.length) {
    return false;
  }
  return timingSafeEqual(left, right);
}

export function bearerToken(request: Request): string | null {
  const header = request.headers.get("authorization");
  if (!header) {
    return null;
  }
  const match = /^Bearer\s+(.+)$/i.exec(header.trim());
  return match ? match[1]! : null;
}

function sign(payload: string, secret: string): string {
  return createHmac("sha256", secret).update(payload).digest("base64url");
}

export function createSessionCookie(secret: string): string {
  const expires = Math.floor(Date.now() / 1000) + SESSION_TTL_S;
  const payload = `${expires}`;
  const value = `${payload}.${sign(payload, secret)}`;
  return [
    `${SESSION_COOKIE}=${value}`,
    "Path=/",
    "HttpOnly",
    "SameSite=Lax",
    `Max-Age=${SESSION_TTL_S}`,
    // Set unconditionally: this is served over TLS in every deployment that
    // matters, and Web Bluetooth would not work without one anyway.
    "Secure",
  ].join("; ");
}

export function clearSessionCookie(): string {
  return `${SESSION_COOKIE}=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0`;
}

export function hasValidSession(cookieHeader: string | null, secret: string): boolean {
  if (!cookieHeader) {
    return false;
  }
  const cookie = cookieHeader
    .split(";")
    .map((part) => part.trim())
    .find((part) => part.startsWith(`${SESSION_COOKIE}=`));
  if (!cookie) {
    return false;
  }
  const value = cookie.slice(SESSION_COOKIE.length + 1);
  const dot = value.lastIndexOf(".");
  if (dot <= 0) {
    return false;
  }
  const payload = value.slice(0, dot);
  const signature = value.slice(dot + 1);
  if (!safeEquals(signature, sign(payload, secret))) {
    return false;
  }
  const expires = Number.parseInt(payload, 10);
  return Number.isFinite(expires) && expires > Math.floor(Date.now() / 1000);
}

export const sessionCookieName = SESSION_COOKIE;
