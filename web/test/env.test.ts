/*
 * The documented local flow in web/README.md: fill in `.env`, `npm run dev`,
 * `npm run device:add`. Both read the process environment, and neither of them
 * loaded `.env` -- only `db:migrate` and this test suite did, so the suite was
 * green while the dashboard answered 500 "SESSION_SECRET is not set" and the
 * device script refused with "DATABASE_URL is not set".
 */

import { spawnSync } from "node:child_process";
import { existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const WEB_DIR = fileURLToPath(new URL("..", import.meta.url));
const KEYS = ["SESSION_SECRET", "DASHBOARD_PASSWORD"] as const;

function context(path: string) {
  const url = new URL(`http://localhost${path}`);
  return {
    url,
    request: new Request(url),
    redirect: (to: string, status = 302) => new Response(null, { status, headers: { location: to } }),
  };
}

describe("the server reads .env from the directory it is started in", () => {
  let dir: string;
  const saved: Record<string, string | undefined> = {};

  beforeEach(() => {
    dir = mkdtempSync(join(tmpdir(), "maxl-env-"));
    for (const key of KEYS) {
      saved[key] = process.env[key];
      delete process.env[key];
    }
    vi.spyOn(process, "cwd").mockReturnValue(dir);
    vi.resetModules();
  });

  afterEach(() => {
    vi.restoreAllMocks();
    rmSync(dir, { recursive: true, force: true });
    for (const key of KEYS) {
      if (saved[key] === undefined) delete process.env[key];
      else process.env[key] = saved[key];
    }
  });

  it("asks for a login instead of answering 500 when SESSION_SECRET is only in .env", async () => {
    writeFileSync(join(dir, ".env"), "SESSION_SECRET=from-the-file\n");
    const { onRequest } = await import("../src/middleware");

    const response = (await onRequest(context("/") as never, (async () => new Response("page")) as never)) as Response;

    expect(response.status).toBe(302);
    expect(response.headers.get("location")).toBe("/login");
    expect(process.env.SESSION_SECRET).toBe("from-the-file");
  });

  it("lets the real environment win over .env, as the container deployment needs", async () => {
    writeFileSync(join(dir, ".env"), "SESSION_SECRET=from-the-file\n");
    process.env.SESSION_SECRET = "from-the-environment";
    await import("../src/middleware");

    expect(process.env.SESSION_SECRET).toBe("from-the-environment");
  });

  it("still fails closed when neither supplies the secret", async () => {
    const { onRequest } = await import("../src/middleware");

    const response = (await onRequest(context("/") as never, (async () => new Response("page")) as never)) as Response;

    expect(response.status).toBe(500);
  });
});

// Needs a filled-in web/.env, which CI does not have: there the variables come
// from the job environment and there is nothing for the script to load.
describe.skipIf(!existsSync(join(WEB_DIR, ".env")))("the device script reads web/.env", () => {
  it("lists devices with no DATABASE_URL in the environment", () => {
    const env = { ...process.env };
    delete env.DATABASE_URL;

    const result = spawnSync(join(WEB_DIR, "node_modules/.bin/tsx"), ["scripts/device.ts", "list"], {
      cwd: WEB_DIR,
      env,
      encoding: "utf8",
    });

    expect(result.stderr).not.toContain("DATABASE_URL is not set");
    expect(result.status).toBe(0);
  });
});
