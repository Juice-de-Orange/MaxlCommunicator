/*
 * One connection pool per process.
 *
 * postgres.js rather than node-postgres: bytea round-trips as a Buffer without
 * a parser of our own, and the frame bodies in `events` are the whole point of
 * that column.
 */

import { drizzle } from "drizzle-orm/postgres-js";
import postgres from "postgres";

import * as schema from "./schema";

export type Database = ReturnType<typeof createDatabase>;

export function createDatabase(url: string, options: { max?: number } = {}) {
  const sql = postgres(url, {
    max: options.max ?? 10,
    // Dates come back as `Date` in UTC. Everything stored is timestamptz, and
    // the device clock is UTC (docs/bridge-protocol.md, SET_TIME takes unix
    // seconds), so there is no local time anywhere in this system.
    types: {},
    onnotice: () => {},
  });
  return { db: drizzle(sql, { schema }), sql };
}

let shared: Database | null = null;

/** The connection the Astro pages and endpoints use. */
export function getDatabase(): Database {
  if (shared === null) {
    const url = process.env.DATABASE_URL;
    if (!url) {
      throw new Error(
        "DATABASE_URL is not set. Copy web/.env.example to web/.env, or run " +
          "`npm run db:up` for a local PostgreSQL 17.",
      );
    }
    shared = createDatabase(url);
  }
  return shared;
}
