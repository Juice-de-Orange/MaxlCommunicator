/*
 * The suite must not be able to empty a database that is not its own.
 *
 * It used to run in whatever `DATABASE_URL` named and delete every row there,
 * before and after each file. docs/DEPLOYMENT.md has the operator put the
 * production URL into `.env`; `npm test` next to that file was a data loss.
 */

import { afterAll, describe, expect, it } from "vitest";
import { sql } from "drizzle-orm";

import { createDatabase, type Database } from "../src/db/client";
import { TEST_DATABASE_SUFFIX, databaseName, openTestDatabase, testDatabaseUrl, wipe } from "./database";

describe("the test database name", () => {
  it("is the configured database with _test appended", () => {
    expect(testDatabaseUrl("postgres://maxl:secret@127.0.0.1:5433/maxl")).toBe(
      "postgres://maxl:secret@127.0.0.1:5433/maxl_test",
    );
  });

  it("keeps credentials, port and query as they are", () => {
    expect(testDatabaseUrl("postgres://u:p%40ss@db.example.com:6543/dash?sslmode=require")).toBe(
      "postgres://u:p%40ss@db.example.com:6543/dash_test?sslmode=require",
    );
  });

  it("leaves a name that already ends in _test alone", () => {
    const url = "postgres://maxl:secret@127.0.0.1:5433/maxl_test";
    expect(testDatabaseUrl(url)).toBe(url);
  });

  it("refuses a URL that names no database", () => {
    expect(() => testDatabaseUrl("postgres://maxl:secret@127.0.0.1:5433")).toThrow(/names no database/);
  });
});

describe("where the suite runs", () => {
  const opened: Database[] = [];

  afterAll(async () => {
    for (const database of opened) await database.sql.end();
  });

  it("is connected to a database whose name ends in _test", async () => {
    const database = openTestDatabase();
    opened.push(database);
    const [row] = await database.db.execute<{ name: string }>(sql`select current_database() as name`);
    expect(row!.name.endsWith(TEST_DATABASE_SUFFIX)).toBe(true);
  });

  it("refuses to empty the database DATABASE_URL originally named", async () => {
    // The sibling without the suffix: the development database locally, the
    // service database in CI. It exists, because the test database was created
    // through it.
    const testUrl = new URL(process.env.DATABASE_URL!);
    const name = databaseName(testUrl.toString()).slice(0, -TEST_DATABASE_SUFFIX.length);
    testUrl.pathname = `/${encodeURIComponent(name)}`;

    const database = createDatabase(testUrl.toString(), { max: 1 });
    opened.push(database);
    await expect(wipe(database)).rejects.toThrow(`refusing to empty the database "${name}"`);
  });
});
