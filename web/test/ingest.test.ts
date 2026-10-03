/*
 * The ingestion endpoint, against a real PostgreSQL 17.
 *
 * Gates 7.1 and 7.2 from docs/test-plan.md are properties of an ON CONFLICT
 * clause and of an ORDER BY. Neither exists in a mock, so a suite that stubs the
 * database proves nothing about either -- `npm run db:up` first.
 */

import { afterAll, beforeAll, beforeEach, describe, expect, it } from "vitest";
import { eq, sql } from "drizzle-orm";

import type { Database } from "../src/db/client";
import {
  configVersions,
  deviceState,
  devices,
  events,
  linkStats,
  messages,
  peerObservations,
} from "../src/db/schema";
import { generateIngestToken, hashIngestToken } from "../src/lib/auth";
import { MAX_REQUEST_BYTES, POST } from "../src/pages/api/ingest";
import { openTestDatabase, wipe } from "./database";
import { configApplied, realisticRun, type SynthEvent } from "./synth";

const NODE_ID = 0x0001;

let database: Database;
let token: string;
let deviceRowId: number;

function toBase64(bytes: Uint8Array): string {
  return Buffer.from(bytes).toString("base64");
}

function payload(events: SynthEvent[]) {
  return {
    nodeId: NODE_ID,
    events: events.map((event) => ({
      journalCounter: event.journalCounter,
      direction: event.direction,
      opcode: event.opcode,
      body: toBase64(event.body),
      receivedAt: event.receivedAt.toISOString(),
    })),
  };
}

async function post(body: unknown, bearer = token): Promise<Response> {
  const request = new Request("http://localhost/api/ingest", {
    method: "POST",
    headers: {
      "content-type": "application/json",
      ...(bearer ? { authorization: `Bearer ${bearer}` } : {}),
    },
    body: JSON.stringify(body),
  });
  // The route only ever touches `request`; Astro's other context members are
  // not part of what is under test here.
  return POST({ request } as never);
}

async function countEvents(): Promise<number> {
  const [row] = await database.db
    .select({ n: sql<number>`count(*)::int` })
    .from(events)
    .where(eq(events.deviceId, deviceRowId));
  return row!.n;
}

beforeAll(async () => {
  database = openTestDatabase();
});

beforeEach(async () => {
  const { db } = database;
  await wipe(database);

  token = generateIngestToken();
  const [row] = await db
    .insert(devices)
    .values({ nodeId: NODE_ID, name: "T-Echo A", ingestTokenHash: hashIngestToken(token) })
    .returning({ id: devices.id });
  deviceRowId = row!.id;
});

afterAll(async () => {
  await wipe(database);
  await database.sql.end();
});

describe("authentication", () => {
  it("refuses a request with no bearer token", async () => {
    const response = await post(payload(realisticRun()), "");
    expect(response.status).toBe(401);
    expect(await countEvents()).toBe(0);
  });

  it("refuses a wrong token", async () => {
    const response = await post(payload(realisticRun()), generateIngestToken());
    expect(response.status).toBe(401);
    expect(await countEvents()).toBe(0);
  });

  it("answers the same for a wrong token and an unknown node", async () => {
    // Telling an unauthenticated caller which node ids exist is telling them
    // what to attack. Both cases have to be indistinguishable.
    const wrongToken = await post(payload(realisticRun()), generateIngestToken());
    const unknownNode = await post({ ...payload(realisticRun()), nodeId: 0xbeef });
    expect(unknownNode.status).toBe(wrongToken.status);
    expect(await unknownNode.json()).toEqual(await wrongToken.json());
  });

  it("does not accept a dashboard session in place of a device token", async () => {
    // CLAUDE.md 4.3: this endpoint authenticates a device-bridge pair, not a
    // web session. A cookie must buy nothing here.
    const request = new Request("http://localhost/api/ingest", {
      method: "POST",
      headers: { "content-type": "application/json", cookie: "maxl_session=anything" },
      body: JSON.stringify(payload(realisticRun())),
    });
    const response = await POST({ request } as never);
    expect(response.status).toBe(401);
  });
});

describe("gate 7.1 -- idempotency", () => {
  it("leaves the row count unchanged when the same batch is posted three times", async () => {
    const batch = payload(realisticRun());

    const first = await post(batch);
    expect(first.status).toBe(200);
    const firstBody = await first.json();
    expect(firstBody.inserted).toBe(11);
    expect(firstBody.duplicates).toBe(0);

    const after = await countEvents();
    expect(after).toBe(11);

    for (let attempt = 2; attempt <= 3; attempt += 1) {
      const repeat = await post(batch);
      const body = await repeat.json();
      expect(repeat.status).toBe(200);
      expect(body.inserted).toBe(0);
      expect(body.duplicates).toBe(11);
      expect(await countEvents()).toBe(after);
    }
  });

  it("inserts only what is new when a batch overlaps one already stored", async () => {
    // The real shape of a reconnect: the phone re-sends from the last counter it
    // durably stored, which is usually a few behind.
    const run = realisticRun();
    await post(payload(run.slice(0, 6)));
    const response = await post(payload(run.slice(3)));
    const body = await response.json();

    expect(body.inserted).toBe(5);
    expect(body.duplicates).toBe(3);
    expect(await countEvents()).toBe(11);
  });

  it("reports the high water mark so the client knows where to resume", async () => {
    const response = await post(payload(realisticRun()));
    expect(Number((await response.json()).highWaterMark)).toBe(11);
  });
});

describe("gate 7.2 -- out-of-order arrival", () => {
  async function projectionSnapshot() {
    const { db } = database;
    const [state] = await db.select().from(deviceState).where(eq(deviceState.deviceId, deviceRowId));
    const messageRows = await db
      .select()
      .from(messages)
      .where(eq(messages.deviceId, deviceRowId))
      .orderBy(messages.frameCounter);
    const linkRows = await db
      .select()
      .from(linkStats)
      .where(eq(linkStats.deviceId, deviceRowId))
      .orderBy(linkStats.frameCounter);
    return {
      state: { ...state, deviceId: undefined },
      messages: messageRows.map((row) => ({ ...row, id: undefined })),
      linkStats: linkRows.map((row) => ({ ...row, id: undefined })),
    };
  }

  it("produces the same projections whatever order the batches arrive in", async () => {
    const run = realisticRun();

    await post(payload(run));
    const inOrder = await projectionSnapshot();

    // Wipe and replay backwards, one event per request -- the worst case, and
    // the one a phone that reads its journal from the end actually produces.
    const { db } = database;
    await db.delete(messages);
    await db.delete(linkStats);
    await db.delete(peerObservations);
    await db.delete(deviceState);
    await db.delete(events);

    for (const event of [...run].reverse()) {
      const response = await post(payload([event]));
      expect(response.status).toBe(200);
    }
    expect(await projectionSnapshot()).toEqual(inOrder);
  });

  it("does not leave a message stuck at queued when its delivery arrives first", async () => {
    const run = realisticRun();
    const delivered = run.find((e) => e.journalCounter === 8)!;
    const queued = run.find((e) => e.journalCounter === 6)!;

    await post(payload([delivered]));
    await post(payload([queued]));

    const [row] = await database.db
      .select()
      .from(messages)
      .where(eq(messages.deviceId, deviceRowId));
    expect(row!.state).toBe("delivered");
    expect(row!.attempts).toBe(1);
  });
});

describe("gate 7.3 -- a pushed config is not an applied config", () => {
  it("leaves applied_at null until the device says otherwise", async () => {
    const { db } = database;
    await db.insert(configVersions).values({
      deviceId: deviceRowId,
      configVersion: 7,
      tlvs: new Uint8Array([0x02, 0x02, 0xd0, 0x07]), // sniffIntervalMs = 2000
    });

    const [beforeRow] = await db
      .select()
      .from(configVersions)
      .where(eq(configVersions.deviceId, deviceRowId));
    expect(beforeRow!.pushedAt).toBeInstanceOf(Date);
    expect(beforeRow!.appliedAt).toBeNull();

    await post(payload(realisticRun()));

    const [afterRow] = await db
      .select()
      .from(configVersions)
      .where(eq(configVersions.deviceId, deviceRowId));
    expect(afterRow!.appliedAt).toBeInstanceOf(Date);
    expect(afterRow!.appliedMask).toBe(0b1111);
    expect([...(afterRow!.unappliedTypes ?? [])]).toEqual([0x09]);
  });

  it("does not move applied_at when the same acknowledgement is replayed", async () => {
    const { db } = database;
    await db.insert(configVersions).values({
      deviceId: deviceRowId,
      configVersion: 7,
      tlvs: new Uint8Array([0x02, 0x02, 0xd0, 0x07]),
    });
    await post(payload(realisticRun()));

    const [first] = await db.select().from(configVersions);
    // Force a full rebuild by ingesting one more event; the acknowledgement is
    // replayed as part of it.
    await post(
      payload([
        configApplied({
          journal: 12, configVersion: 7, appliedMask: 0b1111,
          unapplied: [0x09], at: new Date("2026-08-31T09:00:00Z"),
        }),
      ]),
    );
    const [second] = await db.select().from(configVersions);
    expect(second!.appliedAt!.getTime()).toBe(first!.appliedAt!.getTime());
  });
});

describe("input validation", () => {
  it("rejects a counter that is not a u32", async () => {
    const batch = payload(realisticRun());
    batch.events[0]!.journalCounter = 2 ** 33;
    const response = await post(batch);
    expect(response.status).toBe(400);
    expect(await countEvents()).toBe(0);
  });

  it("rejects a fractional counter rather than rounding it into the unique index", async () => {
    const batch = payload(realisticRun());
    batch.events[0]!.journalCounter = 3.5;
    expect((await post(batch)).status).toBe(400);
  });

  it("rejects an unknown direction", async () => {
    const batch = payload(realisticRun());
    (batch.events[0] as { direction: string }).direction = "sideways";
    expect((await post(batch)).status).toBe(400);
  });

  it("refuses a batch larger than the endpoint accepts", async () => {
    const run = realisticRun();
    const many = Array.from({ length: 600 }, (_, i) => ({ ...run[0]!, journalCounter: i + 1 }));
    expect((await post(payload(many))).status).toBe(413);
  });

  it.each([1.5, 2 ** 40, -1, 0x10000])("answers 400, not 500, for the node id %s", async (nodeId) => {
    // Not an unknown node -- not a node id at all. It used to reach the query,
    // where PostgreSQL refused the int4 parameter and the endpoint answered 500.
    const response = await post({ ...payload(realisticRun()), nodeId });
    expect(response.status).toBe(400);
    expect((await response.json()).detail).toBe("nodeId must be an integer between 0 and 65535");
    expect(await countEvents()).toBe(0);
  });

  it("accepts the largest batch the per-event and per-batch limits allow", async () => {
    // The request cap is derived from those two limits, so it must not be the
    // thing that refuses a batch they permit.
    const body = toBase64(new Uint8Array(4096));
    const many = Array.from({ length: 512 }, (_, i) => ({
      journalCounter: i + 1,
      direction: "rx",
      opcode: 0x7f,
      body,
      receivedAt: "2026-08-31T09:00:00.000+00:00",
      deviceTime: "2026-08-31T09:00:00.000+00:00",
    }));
    const batch = { nodeId: NODE_ID, events: many };
    expect(Buffer.byteLength(JSON.stringify(batch))).toBeLessThan(MAX_REQUEST_BYTES);
    const response = await post(batch);
    expect(response.status).toBe(200);
    expect((await response.json()).inserted).toBe(512);
  });

  it("refuses a request larger than any legitimate batch before parsing it", async () => {
    // Valid JSON with an empty batch: the only thing wrong with it is its size.
    // Without the cap this is a 200, after buffering and parsing all of it.
    const response = await post({ nodeId: NODE_ID, events: [], pad: "x".repeat(MAX_REQUEST_BYTES) });
    expect(response.status).toBe(413);
    expect((await response.json()).error).toBe("request_too_large");
  });

  it("refuses on the declared length alone, without reading the body", async () => {
    let pulled = 0;
    const body = new ReadableStream<Uint8Array>({
      pull(controller) {
        pulled += 1;
        controller.enqueue(new Uint8Array(1024));
      },
    });
    const request = new Request("http://localhost/api/ingest", {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "content-length": String(MAX_REQUEST_BYTES + 1),
        authorization: `Bearer ${token}`,
      },
      body,
      duplex: "half",
    } as RequestInit);
    const response = await POST({ request } as never);
    expect(response.status).toBe(413);
    // The stream's own read-ahead at most; an endless body was never drained.
    expect(pulled).toBeLessThan(4);
  });

  it("stops reading an undeclared body at the limit", async () => {
    // A chunked upload has no Content-Length to check. This one never ends.
    let sent = 0;
    const body = new ReadableStream<Uint8Array>({
      pull(controller) {
        sent += 65536;
        controller.enqueue(new Uint8Array(65536).fill(0x20));
      },
    });
    const request = new Request("http://localhost/api/ingest", {
      method: "POST",
      headers: { "content-type": "application/json", authorization: `Bearer ${token}` },
      body,
      duplex: "half",
    } as RequestInit);
    const response = await POST({ request } as never);
    expect(response.status).toBe(413);
    expect(sent).toBeLessThan(MAX_REQUEST_BYTES + 4 * 65536);
  });

  it("accepts an empty batch without touching anything", async () => {
    const response = await post({ nodeId: NODE_ID, events: [] });
    expect(response.status).toBe(200);
    expect((await response.json()).inserted).toBe(0);
  });
});
