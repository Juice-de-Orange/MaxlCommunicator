/*
 * What the command-line tools say when something is wrong.
 *
 * They used to say it as an uncaught DrizzleQueryError: forty lines of stack
 * trace with the failed query and its parameters -- and for `device add` on a
 * node id that already exists, one of those parameters is the hash of the token
 * that had just been generated. One line each, a non-zero exit, no hash.
 */

import { spawnSync } from "node:child_process";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { afterAll, beforeAll, describe, expect, it } from "vitest";

import type { Database } from "../src/db/client";
import { devices } from "../src/db/schema";
import { hashIngestToken } from "../src/lib/auth";
import { describeDatabaseError } from "../src/db/errors";
import { openTestDatabase, wipe } from "./database";

const WEB_DIR = fileURLToPath(new URL("..", import.meta.url));
const TSX = join(WEB_DIR, "node_modules/.bin/tsx");
const SHA256_HEX = /[0-9a-f]{64}/;

// The child inherits this process's environment, and with it the DATABASE_URL
// that test/setup.ts pointed at the test database.
function run(script: string, args: string[], env: NodeJS.ProcessEnv = process.env) {
  const result = spawnSync(TSX, [script, ...args], { cwd: WEB_DIR, env, encoding: "utf8" });
  const output = result.stdout + result.stderr;
  return { status: result.status, output, lines: output.split("\n").filter((line) => line.trim() !== "") };
}

let database: Database;

beforeAll(async () => {
  database = openTestDatabase();
  await wipe(database);
  await database.db
    .insert(devices)
    .values({ nodeId: 1, name: "already here", ingestTokenHash: hashIngestToken("existing") });
});

afterAll(async () => {
  await wipe(database);
  await database.sql.end();
});

describe("device add on a node id that exists", () => {
  it("says so in one line, changes nothing and prints no hash", async () => {
    const result = run("scripts/device.ts", ["add", "--node-id", "1", "--name", "again"]);

    expect(result.status).toBe(1);
    expect(result.lines).toHaveLength(1);
    expect(result.lines[0]).toContain("node 1 is already registered");
    expect(result.output).not.toMatch(SHA256_HEX);
    expect(result.output).not.toContain("ingest token:");

    const rows = await database.db.select().from(devices);
    expect(rows).toHaveLength(1);
    expect(rows[0]!.name).toBe("already here");
    expect(rows[0]!.ingestTokenHash).toBe(hashIngestToken("existing"));
  });
});

describe("device rotate without a usable --node-id", () => {
  it.each([[[]], [["--node-id", "abc"]]])("prints the usage for %j", (args) => {
    const result = run("scripts/device.ts", ["rotate", ...args]);

    expect(result.status).toBe(2);
    expect(result.lines).toEqual(["usage: device rotate --node-id <0..65535>"]);
  });
});

describe("with the database down", () => {
  // Port 1 on loopback: nothing listens there.
  const env = { ...process.env, DATABASE_URL: "postgres://maxl:unused@127.0.0.1:1/maxl" };

  it("db:migrate fails in one line", () => {
    const result = run("src/db/migrate.ts", [], env);

    expect(result.status).toBe(1);
    expect(result.lines).toHaveLength(1);
    expect(result.lines[0]).toMatch(/^migration failed: cannot reach the database \(.*ECONNREFUSED.*\)/);
  });

  it("device add fails in one line and prints no hash", () => {
    const result = run("scripts/device.ts", ["add", "--node-id", "2", "--name", "x"], env);

    expect(result.status).toBe(1);
    expect(result.lines).toHaveLength(1);
    expect(result.lines[0]).toMatch(/^device add failed: cannot reach the database/);
    expect(result.output).not.toMatch(SHA256_HEX);
  });
});

describe("describeDatabaseError", () => {
  it("reports the cause, never the wrapper with the query parameters", () => {
    const hash = "a".repeat(64);
    const wrapper = new Error(`Failed query: insert into "devices" …\nparams: 1,dup,${hash}`, {
      cause: Object.assign(new Error('duplicate key value violates unique constraint "devices_node_id_unique"'), {
        code: "23505",
      }),
    });

    const line = describeDatabaseError(wrapper);
    expect(line).toBe('duplicate key value violates unique constraint "devices_node_id_unique" (23505)');
    expect(line).not.toContain(hash);
  });

  it("reads a refused connection out of an AggregateError", () => {
    const refused = Object.assign(new Error("connect ECONNREFUSED 127.0.0.1:5433"), { code: "ECONNREFUSED" });
    const wrapper = new Error("Failed query: select 1", { cause: new AggregateError([refused], "") });

    expect(describeDatabaseError(wrapper)).toMatch(/^cannot reach the database \(connect ECONNREFUSED 127\.0\.0\.1:5433\)/);
  });
});
