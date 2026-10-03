/*
 * Config push, both halves.
 *
 * The property under test is the one CLAUDE.md 4.3 states and that is easy to
 * get wrong in a way nobody notices: "a pushed config is not an applied config".
 * A dashboard that marked its own push as applied would show a node running
 * settings it has never received.
 */

import { afterAll, beforeAll, beforeEach, describe, expect, it } from "vitest";
import { eq } from "drizzle-orm";

import type { Database } from "../src/db/client";
import {
  configVersions,
  devices,
} from "../src/db/schema";
import {
  createSessionCookie,
  generateIngestToken,
  hashIngestToken,
} from "../src/lib/auth";
import { GET, POST } from "../src/pages/api/config";
import { POST as ingest } from "../src/pages/api/ingest";
import { decodeConfigTlvs, encodeConfigTlvs, validateConfig } from "../src/lib/config-tlv";
import { configApplied } from "./synth";
import { openTestDatabase, wipe } from "./database";

const NODE_ID = 0x0007;
const SECRET = "0".repeat(64);

let database: Database;
let token: string;
let deviceRowId: number;

// `await`, not `return`: an Astro APIRoute is declared as
// `Response | Promise<Response>`, and handing that straight back makes the
// helper's own type a lie. tsc lets it pass; astro check does not, and the
// container build is where that surfaced.
async function push(settings: unknown, nodeId: number = NODE_ID, cookie?: string) {
  const request = new Request("http://localhost/api/config", {
    method: "POST",
    headers: {
      "content-type": "application/json",
      cookie: cookie ?? createSessionCookie(SECRET).split(";")[0]!,
    },
    body: JSON.stringify({ nodeId, settings }),
  });
  return await POST({ request } as never);
}

async function collect(bearer: string = token, nodeId: number = NODE_ID) {
  const url = new URL(`http://localhost/api/config?nodeId=${nodeId}`);
  const request = new Request(url, { headers: { authorization: `Bearer ${bearer}` } });
  return await GET({ request, url } as never);
}

beforeAll(() => {
  process.env.SESSION_SECRET = SECRET;
  database = openTestDatabase();
});

beforeEach(async () => {
  const { db } = database;
  await wipe(database);

  token = generateIngestToken();
  const [row] = await db
    .insert(devices)
    .values({ nodeId: NODE_ID, name: "config node", ingestTokenHash: hashIngestToken(token) })
    .returning({ id: devices.id });
  deviceRowId = row!.id;
});

afterAll(async () => {
  await wipe(database);
  await database.sql.end();
});

describe("TLV encoding", () => {
  it("round-trips every field", () => {
    const settings = {
      sniffIntervalMs: 5000,
      band: 1,
      sfMode: 1,
      fixedSf: 12,
      txPowerDbm: 14,
      telemetryIntervalS: 300,
      beaconIntervalS: 900,
      gnssFixTimeoutS: 120,
    };
    expect(decodeConfigTlvs(encodeConfigTlvs(settings))).toEqual(settings);
  });

  it("uses the type numbers the protocol document gives", () => {
    const bytes = encodeConfigTlvs({ sniffIntervalMs: 2000 });
    expect([...bytes]).toEqual([0x02, 0x02, 0xd0, 0x07]);
  });

  it("emits nothing for settings that were not touched", () => {
    // Pushing the whole set every time would make every change look like a
    // change to everything in the node's own log.
    expect(encodeConfigTlvs({ band: 0 })).toHaveLength(3);
  });

  it("refuses a sniff interval outside the documented range", () => {
    // 250..10000. A node told to sniff every 20 ms has a flat battery by
    // lunchtime, and finding that out from an acknowledgement an hour later is
    // an hour wasted.
    expect(validateConfig({ sniffIntervalMs: 20 })).toMatch(/Sniff/);
    expect(validateConfig({ sniffIntervalMs: 250 })).toBeNull();
    expect(validateConfig({ sniffIntervalMs: 10001 })).toMatch(/Sniff/);
  });

  it("refuses a spreading factor outside 7..12", () => {
    expect(validateConfig({ fixedSf: 6 })).not.toBeNull();
    expect(validateConfig({ fixedSf: 13 })).not.toBeNull();
    expect(validateConfig({ fixedSf: 9 })).toBeNull();
  });

  it("has no field for the duty cycle limit", () => {
    // CLAUDE.md 1.3: "It is not configurable, in firmware or over the air."
    const encoded = encodeConfigTlvs({ sniffIntervalMs: 2000 } as never);
    expect([...encoded]).not.toContain(0x0a);
  });
});

describe("pushing", () => {
  it("needs a dashboard session", async () => {
    const request = new Request("http://localhost/api/config", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ nodeId: NODE_ID, settings: { band: 1 } }),
    });
    expect((await POST({ request } as never)).status).toBe(401);
  });

  it("does not accept an ingest token in place of a session", async () => {
    // The two directions authenticate differently on purpose. A device token
    // proves a node, not an operator.
    const request = new Request("http://localhost/api/config", {
      method: "POST",
      headers: { "content-type": "application/json", authorization: `Bearer ${token}` },
      body: JSON.stringify({ nodeId: NODE_ID, settings: { band: 1 } }),
    });
    expect((await POST({ request } as never)).status).toBe(401);
  });

  it("stores a version with pushed_at and a null applied_at", async () => {
    const response = await push({ sniffIntervalMs: 5000 });
    expect(response.status).toBe(200);
    expect((await response.json()).configVersion).toBe(1);

    const [row] = await database.db
      .select()
      .from(configVersions)
      .where(eq(configVersions.deviceId, deviceRowId));
    expect(row!.pushedAt).toBeInstanceOf(Date);
    expect(row!.appliedAt).toBeNull();
    expect(decodeConfigTlvs(row!.tlvs)).toEqual({ sniffIntervalMs: 5000 });
  });

  it("numbers versions monotonically", async () => {
    expect((await (await push({ band: 0 })).json()).configVersion).toBe(1);
    expect((await (await push({ band: 1 })).json()).configVersion).toBe(2);
    expect((await (await push({ fixedSf: 10 })).json()).configVersion).toBe(3);
  });

  it("refuses a value out of range before it reaches the node", async () => {
    const response = await push({ sniffIntervalMs: 20 });
    expect(response.status).toBe(400);
    expect(await database.db.select().from(configVersions)).toHaveLength(0);
  });

  it("refuses a push with nothing in it", async () => {
    expect((await push({})).status).toBe(400);
  });

  it("answers 404 for a node that does not exist", async () => {
    expect((await push({ band: 1 }, 0x1234)).status).toBe(404);
  });

  it.each([1.5, 2 ** 40, -1, 0x10000])("answers 400, not 500, for the node id %s", async (nodeId) => {
    const response = await push({ band: 1 }, nodeId);
    expect(response.status).toBe(400);
    expect((await response.json()).detail).toBe("nodeId must be an integer between 0 and 65535");
  });
});

describe("collecting", () => {
  it("needs the device's ingest token", async () => {
    await push({ band: 1 });
    expect((await collect("wrong")).status).toBe(401);
  });

  it("hands back the newest unapplied config", async () => {
    await push({ band: 1 });
    await push({ sniffIntervalMs: 5000 });

    const body = await (await collect()).json();
    expect(body.pending.configVersion).toBe(2);
    expect(decodeConfigTlvs(Uint8Array.from(Buffer.from(body.pending.tlvs, "base64")))).toEqual({
      sniffIntervalMs: 5000,
    });
  });

  it("hands back only the newest, not a backlog", async () => {
    // Three pushes while the node was out of range. Delivering all three would
    // spend three SET_CONFIG round trips to arrive at what the last one says,
    // and BLE connection time is the actual bottleneck.
    await push({ band: 1 });
    await push({ band: 0 });
    await push({ band: 1 });
    const body = await (await collect()).json();
    expect(body.pending.configVersion).toBe(3);
  });

  it("has nothing pending once the device acknowledges", async () => {
    await push({ sniffIntervalMs: 5000 });
    expect((await (await collect()).json()).pending).not.toBeNull();

    // The acknowledgement travels the normal way: as a journal event through
    // the ingest endpoint, projected onto config_versions.
    const event = configApplied({
      journal: 1, configVersion: 1, appliedMask: 0b0010,
      at: new Date("2026-08-31T10:00:00Z"),
    });
    const request = new Request("http://localhost/api/ingest", {
      method: "POST",
      headers: { "content-type": "application/json", authorization: `Bearer ${token}` },
      body: JSON.stringify({
        nodeId: NODE_ID,
        events: [{
          journalCounter: event.journalCounter,
          direction: event.direction,
          opcode: event.opcode,
          body: Buffer.from(event.body).toString("base64"),
          receivedAt: event.receivedAt.toISOString(),
        }],
      }),
    });
    expect((await ingest({ request } as never)).status).toBe(200);

    expect((await (await collect()).json()).pending).toBeNull();
  });

  it.each(["99999999999", "1.5", "abc", ""])("answers 401, not 500, for ?nodeId=%s", async (nodeId) => {
    const url = new URL(`http://localhost/api/config?nodeId=${nodeId}`);
    const request = new Request(url, { headers: { authorization: `Bearer ${token}` } });
    expect((await GET({ request, url } as never)).status).toBe(401);
  });

  it("answers 401 for an unknown node the same way as for a bad token", async () => {
    const unknown = await collect(token, 0x4321);
    const badToken = await collect("wrong");
    expect(unknown.status).toBe(badToken.status);
  });
});
