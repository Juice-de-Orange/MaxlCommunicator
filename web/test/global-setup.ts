/*
 * Once per `npm test`, before any test file: make sure the tests' own database
 * exists and carries the current schema. See test/database.ts for why the suite
 * does not run in the database `DATABASE_URL` names.
 *
 * The migrations are the real ones from drizzle/, so a broken migration still
 * fails here as a broken migration rather than as fifty confusing test
 * failures.
 */

import { fileURLToPath } from "node:url";

import { migrate } from "drizzle-orm/postgres-js/migrator";
import postgres from "postgres";

import { createDatabase } from "../src/db/client";
import { loadDotEnv } from "../src/db/env";
import { describeDatabaseError } from "../src/db/errors";
import { databaseName, testDatabaseUrl } from "./database";

export default async function setup(): Promise<void> {
  loadDotEnv(new URL("../.env", import.meta.url));

  const url = process.env.DATABASE_URL;
  if (!url) {
    throw new Error("DATABASE_URL is not set -- run `npm run db:up` and copy .env.example");
  }
  const testUrl = testDatabaseUrl(url);
  const name = databaseName(testUrl);

  if (testUrl !== url) {
    // CREATE DATABASE cannot run inside the database it creates, so this goes
    // through the one DATABASE_URL names. Nothing else is done there.
    const admin = postgres(url, { max: 1, onnotice: () => {} });
    try {
      const existing = await admin`select 1 from pg_database where datname = ${name}`;
      if (existing.length === 0) {
        await admin.unsafe(`create database "${name.replaceAll('"', '""')}"`);
      }
    } catch (error) {
      throw new Error(
        `could not create the test database "${name}": ${describeDatabaseError(error)}. ` +
          "Is PostgreSQL running (`npm run db:up`), and may this role create databases? " +
          `If not, create "${name}" by hand once.`,
      );
    } finally {
      await admin.end();
    }
  }

  const { db, sql } = createDatabase(testUrl, { max: 1 });
  try {
    await migrate(db, { migrationsFolder: fileURLToPath(new URL("../drizzle", import.meta.url)) });
  } catch (error) {
    throw new Error(`could not migrate the test database "${name}": ${describeDatabaseError(error)}`);
  } finally {
    await sql.end();
  }
}
