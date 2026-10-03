/*
 * Gate 7.4 -- "Budget chart against device-reported budget: agree within 5 %".
 *
 * The chart draws whatever device_samples holds, so the question is really
 * whether device_samples holds what the device said. Two ways to be wrong, and
 * the gate exists because both are easy:
 *
 *   - the value could be transformed on the way in (units, a rolling average, a
 *     smoothing pass) and quietly stop being the device's number;
 *   - EVT_BUDGET and the status body both carry the budget, and picking the
 *     wrong one -- or letting an older report win -- gives a plausible series
 *     that is not the one the node reported.
 *
 * So this compares the stored series point by point against the values that were
 * encoded into the events, in milliseconds, with no rounding anywhere.
 */

import { beforeAll, beforeEach, afterAll, describe, expect, it } from "vitest";
import { asc, eq } from "drizzle-orm";

import type { Database } from "../src/db/client";
import {
  deviceSamples,
  deviceState,
  devices,
} from "../src/db/schema";
import { generateIngestToken, hashIngestToken } from "../src/lib/auth";
import { POST } from "../src/pages/api/ingest";
import { budget, status, type SynthEvent } from "./synth";
import { openTestDatabase, wipe } from "./database";

const NODE_ID = 0x0042;
const LIMIT_MS = 360_000; // g3, CLAUDE.md 1.3

let database: Database;
let token: string;
let deviceRowId: number;

async function post(events: SynthEvent[]): Promise<Response> {
  const request = new Request("http://localhost/api/ingest", {
    method: "POST",
    headers: { "content-type": "application/json", authorization: `Bearer ${token}` },
    body: JSON.stringify({
      nodeId: NODE_ID,
      events: events.map((event) => ({
        journalCounter: event.journalCounter,
        direction: event.direction,
        opcode: event.opcode,
        body: Buffer.from(event.body).toString("base64"),
        receivedAt: event.receivedAt.toISOString(),
      })),
    }),
  });
  return POST({ request } as never);
}

beforeAll(() => {
  database = openTestDatabase();
});

beforeEach(async () => {
  const { db } = database;
  await wipe(database);

  token = generateIngestToken();
  const [row] = await db
    .insert(devices)
    .values({ nodeId: NODE_ID, name: "budget node", ingestTokenHash: hashIngestToken(token) })
    .returning({ id: devices.id });
  deviceRowId = row!.id;
});

afterAll(async () => {
  await wipe(database);
  await database.sql.end();
});

describe("gate 7.4 -- the budget series is what the device reported", () => {
  /** A day of budget reports that saturate and recover, as CLAUDE.md 1.2 describes. */
  function budgetDay(): { events: SynthEvent[]; expected: number[] } {
    const base = Date.UTC(2026, 7, 31, 0, 0, 0);
    const events: SynthEvent[] = [];
    const expected: number[] = [];
    let journal = 1;

    for (let quarter = 0; quarter < 96; quarter += 1) {
      const at = new Date(base + quarter * 15 * 60_000);
      // A shape with a genuine saturation in it: the hourly total reaches the
      // allowance around the middle of the day and is refused there.
      const used = Math.min(
        LIMIT_MS,
        Math.round(LIMIT_MS * (0.1 + 1.15 * Math.max(0, Math.sin((quarter / 96) * Math.PI)))),
      );
      expected.push(used);

      // Alternate between the two sources that carry the budget. Both are
      // legitimate; the projection must take the later one either way.
      if (quarter % 2 === 0) {
        events.push(
          budget({
            journal: journal++, band: 0, usedMs: used, limitMs: LIMIT_MS,
            nextTxUnix: used >= LIMIT_MS ? Math.floor(at.getTime() / 1000) + 120 : 0,
            at,
          }),
        );
      } else {
        events.push(
          status({
            journal: journal++, batteryMv: 4000, uptimeS: quarter * 900,
            queueDepth: 0, band: 0, budgetUsedMs: used, budgetLimitMs: LIMIT_MS, at,
          }),
        );
      }
    }
    return { events, expected };
  }

  it("stores every reported value unchanged, within far better than 5 %", async () => {
    const { events: journal, expected } = budgetDay();
    const response = await post(journal);
    expect(response.status).toBe(200);

    const stored = await database.db
      .select({ usedMs: deviceSamples.budgetUsedMs, limitMs: deviceSamples.budgetLimitMs })
      .from(deviceSamples)
      .where(eq(deviceSamples.deviceId, deviceRowId))
      .orderBy(asc(deviceSamples.journalCounter));

    expect(stored).toHaveLength(expected.length);

    let worstRelative = 0;
    for (const [index, sample] of stored.entries()) {
      expect(sample.limitMs).toBe(LIMIT_MS);
      const reported = expected[index]!;
      const relative = reported === 0 ? 0 : Math.abs(sample.usedMs! - reported) / reported;
      worstRelative = Math.max(worstRelative, relative);
    }

    // The gate allows 5 %. Nothing transforms the value, so the honest result is
    // exact -- and a test that merely checked "within 5 %" would pass just as
    // happily against a smoothing pass that has no business being there.
    expect(worstRelative).toBe(0);
  });

  it("never records more airtime than the band allows", async () => {
    // CLAUDE.md 1.3: g3 is 360 s an hour. A stored value above the limit would
    // mean either the device broke compliance or this server mangled the number,
    // and both are worth failing on.
    const { events: journal } = budgetDay();
    await post(journal);

    const stored = await database.db
      .select({ usedMs: deviceSamples.budgetUsedMs })
      .from(deviceSamples)
      .where(eq(deviceSamples.deviceId, deviceRowId));

    for (const sample of stored) {
      expect(sample.usedMs!).toBeLessThanOrEqual(LIMIT_MS);
    }
  });

  it("shows the newest report on the node list, not whichever arrived last", async () => {
    const base = Date.UTC(2026, 7, 31, 12, 0, 0);
    const older = budget({
      journal: 10, band: 0, usedMs: 120_000, limitMs: LIMIT_MS, nextTxUnix: 0,
      at: new Date(base),
    });
    const newer = status({
      journal: 11, batteryMv: 3990, uptimeS: 100, queueDepth: 0, band: 0,
      budgetUsedMs: 250_000, budgetLimitMs: LIMIT_MS, at: new Date(base + 900_000),
    });

    // Delivered out of order on purpose.
    await post([newer]);
    await post([older]);

    const [state] = await database.db
      .select()
      .from(deviceState)
      .where(eq(deviceState.deviceId, deviceRowId));
    expect(state!.budgetUsedMs).toBe(250_000);
  });

  it("carries the g1 allowance when the node reports the fallback band", async () => {
    // The limit is a field, not an assumption. A dashboard that hard-coded 360 s
    // would draw a node on g1 as using a sixth of its budget when it is at 60 %.
    await post([
      budget({
        journal: 1, band: 1, usedMs: 21_600, limitMs: 36_000, nextTxUnix: 0,
        at: new Date(Date.UTC(2026, 7, 31, 8, 0, 0)),
      }),
    ]);
    const [state] = await database.db
      .select()
      .from(deviceState)
      .where(eq(deviceState.deviceId, deviceRowId));
    expect(state!.budgetBand).toBe(1);
    expect(state!.budgetLimitMs).toBe(36_000);
    expect((state!.budgetUsedMs! / state!.budgetLimitMs!) * 100).toBeCloseTo(60, 5);
  });
});
