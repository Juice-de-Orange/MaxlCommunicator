/*
 * POST /api/ingest -- the only way events enter this system.
 *
 * CLAUDE.md 4.3: "the ingestion endpoint authenticates a device-bridge pair, not
 * a web session". There is no cookie here and no browser assumption; the Kotlin
 * client of phase 9 posts to exactly this with exactly this bearer token.
 *
 * Idempotency is the whole contract. docs/bridge-protocol.md section 3 has the
 * phone acknowledge the device's journal only after the server has the data, so
 * a phone that dies mid-push re-sends on reconnect -- "erring towards re-sending
 * is always the right call", which is only true if a re-send is free. The unique
 * index on (device_id, journal_counter, direction) is what makes it free (D10), and the
 * response reports inserted and duplicate counts separately so the client can
 * tell the difference between "you already had these" and "nothing happened".
 */

import type { APIRoute } from "astro";
import { and, eq, sql } from "drizzle-orm";

import { getDatabase } from "../../db/client";
import { rebuildProjections } from "../../db/apply-projections";
import { devices, events } from "../../db/schema";
import { bearerToken, hashIngestToken } from "../../lib/auth";

export const prerender = false;

interface IncomingEvent {
  /** Journal entry counter (D10), not the frame counter. */
  journalCounter: number;
  direction: "rx" | "tx";
  opcode: number;
  /** Base64. The body stays binary all the way to the bytea column. */
  body: string;
  receivedAt: string;
  deviceTime?: string;
}

interface IncomingBatch {
  nodeId: number;
  events: IncomingEvent[];
}

/** docs/bridge-protocol.md section 1.1: a reassembled message is at most 4096 B. */
const MAX_BODY_BYTES = 4096;
const MAX_EVENTS_PER_BATCH = 512;

function bad(status: number, error: string, detail?: string): Response {
  return new Response(JSON.stringify({ error, detail }), {
    status,
    headers: { "content-type": "application/json" },
  });
}

function parseEvent(raw: unknown, index: number): IncomingEvent | string {
  if (typeof raw !== "object" || raw === null) {
    return `events[${index}] is not an object`;
  }
  const value = raw as Record<string, unknown>;

  const journalCounter = value.journalCounter;
  // u32, and it must be an integer: a float here would round in the unique
  // index and silently merge two distinct frames.
  if (typeof journalCounter !== "number" || !Number.isInteger(journalCounter)
      || journalCounter < 0 || journalCounter > 0xffffffff) {
    return `events[${index}].journalCounter must be a u32`;
  }
  if (value.direction !== "rx" && value.direction !== "tx") {
    return `events[${index}].direction must be "rx" or "tx"`;
  }
  const opcode = value.opcode;
  if (typeof opcode !== "number" || !Number.isInteger(opcode) || opcode < 0 || opcode > 0xff) {
    return `events[${index}].opcode must be a byte`;
  }
  if (typeof value.body !== "string") {
    return `events[${index}].body must be base64`;
  }
  if (typeof value.receivedAt !== "string" || Number.isNaN(Date.parse(value.receivedAt))) {
    return `events[${index}].receivedAt must be an ISO 8601 timestamp`;
  }
  if (value.deviceTime !== undefined
      && (typeof value.deviceTime !== "string" || Number.isNaN(Date.parse(value.deviceTime)))) {
    return `events[${index}].deviceTime must be an ISO 8601 timestamp`;
  }
  return {
    journalCounter,
    direction: value.direction,
    opcode,
    body: value.body,
    receivedAt: value.receivedAt,
    deviceTime: value.deviceTime as string | undefined,
  };
}

export const POST: APIRoute = async ({ request }) => {
  const token = bearerToken(request);
  if (!token) {
    return bad(401, "unauthorised", "Authorization: Bearer <ingest token> is required");
  }

  let batch: IncomingBatch;
  try {
    batch = (await request.json()) as IncomingBatch;
  } catch {
    return bad(400, "bad_request", "body is not JSON");
  }

  if (typeof batch?.nodeId !== "number" || !Array.isArray(batch.events)) {
    return bad(400, "bad_request", "expected { nodeId: number, events: [...] }");
  }
  if (batch.events.length > MAX_EVENTS_PER_BATCH) {
    return bad(413, "batch_too_large", `at most ${MAX_EVENTS_PER_BATCH} events per request`);
  }

  const parsed: IncomingEvent[] = [];
  for (const [index, raw] of batch.events.entries()) {
    const result = parseEvent(raw, index);
    if (typeof result === "string") {
      return bad(400, "bad_request", result);
    }
    parsed.push(result);
  }

  const database = getDatabase();
  const { db } = database;

  const [device] = await db
    .select({ id: devices.id, hash: devices.ingestTokenHash })
    .from(devices)
    .where(eq(devices.nodeId, batch.nodeId))
    .limit(1);

  // One answer for "no such node" and "wrong token". Telling an unauthenticated
  // caller which node ids exist is telling them what to attack.
  if (!device || device.hash !== hashIngestToken(token)) {
    return bad(401, "unauthorised");
  }

  const rows = parsed.map((event) => ({
    deviceId: device.id,
    journalCounter: event.journalCounter,
    direction: event.direction,
    opcode: event.opcode,
    body: Uint8Array.from(Buffer.from(event.body, "base64")),
    receivedAt: new Date(event.receivedAt),
    deviceTime: event.deviceTime ? new Date(event.deviceTime) : null,
  }));

  const oversized = rows.find((row) => row.body.length > MAX_BODY_BYTES);
  if (oversized) {
    return bad(413, "body_too_large", `event bodies are at most ${MAX_BODY_BYTES} bytes`);
  }

  let inserted = 0;
  if (rows.length > 0) {
    const written = await db
      .insert(events)
      .values(rows)
      // Gate 7.1. The same batch posted three times must leave the row count
      // unchanged, and this is the clause that does it.
      .onConflictDoNothing({
        target: [events.deviceId, events.journalCounter, events.direction],
      })
      .returning({ id: events.id });
    inserted = written.length;
  }

  if (inserted > 0) {
    await rebuildProjections(database, device.id);
    await db
      .update(devices)
      .set({ lastSeenAt: new Date() })
      .where(eq(devices.id, device.id));
  }

  const [row] = await db
    .select({
      highWaterMark: sql<number>`coalesce(max(${events.journalCounter}), 0)::bigint`,
    })
    .from(events)
    .where(and(eq(events.deviceId, device.id)));

  /*
   * bigint, because a frame counter is a u32 and int4 tops out at 2147483647 --
   * a node that has been running long enough would silently overflow the cast.
   * postgres.js hands a bigint back as a string, so it is converted here rather
   * than leaving every client to discover that "5" > 10 is false.
   */
  const highWaterMark = Number(row?.highWaterMark ?? 0);

  return new Response(
    JSON.stringify({
      received: rows.length,
      inserted,
      duplicates: rows.length - inserted,
      highWaterMark,
    }),
    { status: 200, headers: { "content-type": "application/json" } },
  );
};
