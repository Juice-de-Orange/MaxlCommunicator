/*
 * The tests' own database.
 *
 * The suite empties every table before and after each file. It used to do that
 * to whatever `DATABASE_URL` pointed at -- the development database locally,
 * and the production one for anybody who followed docs/DEPLOYMENT.md, filled in
 * `.env` and then ran `npm test` next to it. A functional check lost its seeded
 * nodes that way.
 *
 * So the tests never run in the database `DATABASE_URL` names. They run in its
 * sibling with `_test` appended, which `test/global-setup.ts` creates and
 * migrates, and `wipe()` asks the server which database it is connected to
 * before it deletes anything.
 */

import { sql } from "drizzle-orm";

import { createDatabase, type Database } from "../src/db/client";
import {
  configVersions,
  deviceSamples,
  deviceState,
  devices,
  events,
  linkStats,
  messages,
  peerObservations,
} from "../src/db/schema";

export const TEST_DATABASE_SUFFIX = "_test";

/** `postgres://…/maxl` -> `postgres://…/maxl_test`. A name that already ends in
 *  the suffix is left alone, so pointing `DATABASE_URL` at a test database
 *  directly does not produce `maxl_test_test`. */
export function testDatabaseUrl(url: string): string {
  const parsed = new URL(url);
  const name = decodeURIComponent(parsed.pathname.replace(/^\//, ""));
  if (name === "") {
    throw new Error("DATABASE_URL names no database -- expected postgres://user:password@host:port/<database>");
  }
  if (!name.endsWith(TEST_DATABASE_SUFFIX)) {
    parsed.pathname = `/${encodeURIComponent(name + TEST_DATABASE_SUFFIX)}`;
  }
  return parsed.toString();
}

export function databaseName(url: string): string {
  return decodeURIComponent(new URL(url).pathname.replace(/^\//, ""));
}

/** The connection a test file works on. `test/setup.ts` has already pointed
 *  `DATABASE_URL` at the test database by the time this runs. */
export function openTestDatabase(): Database {
  const url = process.env.DATABASE_URL;
  if (!url) {
    throw new Error("DATABASE_URL is not set -- run `npm run db:up` and copy .env.example");
  }
  return createDatabase(url, { max: 4 });
}

/**
 * Empty every table -- in a test database, and nowhere else.
 *
 * The name comes from the server (`current_database()`), not from the URL: the
 * URL is what somebody configured, the answer is where the DELETEs would land.
 */
export async function wipe(database: Database): Promise<void> {
  const { db } = database;
  const [row] = await db.execute<{ name: string }>(sql`select current_database() as name`);
  const name = row?.name ?? "";
  if (!name.endsWith(TEST_DATABASE_SUFFIX)) {
    throw new Error(
      `refusing to empty the database "${name}": its name does not end in ` +
        `"${TEST_DATABASE_SUFFIX}", so it is not the tests' own`,
    );
  }
  // Order matters: everything references devices.
  await db.delete(messages);
  await db.delete(linkStats);
  await db.delete(peerObservations);
  await db.delete(deviceSamples);
  await db.delete(deviceState);
  await db.delete(configVersions);
  await db.delete(events);
  await db.delete(devices);
}
