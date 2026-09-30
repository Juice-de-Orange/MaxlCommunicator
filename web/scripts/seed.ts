/*
 * Fill a local database with a plausible day so the dashboard can be looked at
 * without two radios on the bench.
 *
 *   npm run seed
 *
 * The events are built with the bridge's encoders and the layouts in
 * docs/bridge-protocol.md, and pushed through the real HTTP endpoint rather than
 * inserted directly -- seeding around the ingestion path would leave the one
 * thing worth exercising untested, and would happily produce data the endpoint
 * would have rejected.
 */

import { EventCode } from "@protocol/opcodes";
import {
  encodePosition,
  encodeStatus,
  encodeTelemetry,
  encodeText,
  FrameType,
} from "@protocol/payloads";

const BASE_URL = process.env.SEED_URL ?? "http://127.0.0.1:4321";

interface Row {
  journalCounter: number;
  direction: "rx" | "tx";
  opcode: number;
  body: string;
  receivedAt: string;
}

const b64 = (bytes: Uint8Array) => Buffer.from(bytes).toString("base64");

function frameRx(counter: number, src: number, type: number, rssi: number, snr: number,
                 payload: Uint8Array): Uint8Array {
  const bytes = new Uint8Array(11 + payload.length);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, counter, true);
  view.setUint16(4, src, true);
  view.setUint8(6, type);
  view.setInt16(7, rssi, true);
  view.setInt8(9, snr);
  view.setUint8(10, payload.length);
  bytes.set(payload, 11);
  return bytes;
}

function txResult(counter: number, dst: number, seq: number, result: number, attempts: number,
                  rssi: number, snr: number): Uint8Array {
  const bytes = new Uint8Array(12);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, counter, true);
  view.setUint16(4, dst, true);
  view.setUint8(6, seq);
  view.setUint8(7, result);
  view.setUint8(8, attempts);
  view.setInt16(9, rssi, true);
  view.setInt8(11, snr);
  return bytes;
}

/**
 * One node's day. Telemetry every ten minutes, a position every half hour, a
 * handful of messages, and a budget that climbs through the afternoon and gets
 * close enough to the g3 limit to be worth drawing.
 */
function dayFor(selfNode: number, peerNode: number, start: Date): Row[] {
  const rows: Row[] = [];
  let journal = 1;
  let frame = 1000 * selfNode;
  const at = (minutes: number) => new Date(start.getTime() + minutes * 60_000).toISOString();

  // CLAUDE.md 1.3: g3 allows 360 s of airtime an hour.
  const limitMs = 360_000;

  for (let minute = 0; minute < 24 * 60; minute += 10) {
    const hour = minute / 60;
    // A day that starts cold, warms into the afternoon and cools off.
    const tempC = 6 + 11 * Math.sin(((hour - 6) / 24) * 2 * Math.PI);
    const humidity = 72 - 18 * Math.sin(((hour - 6) / 24) * 2 * Math.PI);
    const pressure = 95_200 + 140 * Math.sin((hour / 24) * 2 * Math.PI);
    // A cell that drains and does not recover -- this device is in a pocket.
    const batteryMv = Math.round(4180 - (minute / (24 * 60)) * 260);
    // Airtime spent in the rolling hour. Busiest around the middle of the day.
    const usedMs = Math.round(
      limitMs * 0.15 + limitMs * 0.55 * Math.max(0, Math.sin(((hour - 4) / 20) * Math.PI)),
    );

    rows.push({
      journalCounter: journal++,
      direction: "rx",
      opcode: EventCode.Status,
      body: b64(
        encodeStatus({
          batteryMv,
          uptimeS: minute * 60,
          queueDepth: minute % 180 === 0 ? 2 : 0,
          band: 0,
          budgetUsedMs: usedMs,
          budgetLimitMs: limitMs,
          flags: 0b011,
        }),
      ),
      receivedAt: at(minute),
    });

    // The peer's telemetry, heard over the air.
    rows.push({
      journalCounter: journal++,
      direction: "rx",
      opcode: EventCode.FrameRx,
      body: b64(
        frameRx(
          frame++,
          peerNode,
          FrameType.Telemetry,
          // RSSI wanders with distance and weather; SNR follows it loosely.
          Math.round(-88 - 14 * Math.sin((minute / 220) * Math.PI)),
          Math.round(7 + 4 * Math.cos((minute / 190) * Math.PI)),
          encodeTelemetry({
            temperatureCentiC: Math.round(tempC * 100),
            humidityCentiPercent: Math.round(humidity * 100),
            pressurePa: Math.round(pressure),
            batteryMv: batteryMv - 90,
            uptimeS: minute * 60 + 3600,
          }),
        ),
      ),
      receivedAt: at(minute + 1),
    });

    if (minute % 30 === 0) {
      // A walk: roughly north-east over the day, gaining height.
      const step = minute / 30;
      rows.push({
        journalCounter: journal++,
        direction: "rx",
        opcode: EventCode.Fix,
        body: b64(
          (() => {
            const bytes = new Uint8Array(13);
            const view = new DataView(bytes.buffer);
            view.setInt32(0, 472_680_000 + step * 1400, true);
            view.setInt32(4, 114_030_000 + step * 900, true);
            view.setInt16(8, Math.round(574 + step * 9), true);
            view.setUint8(10, 8);
            view.setUint8(11, 2);
            view.setUint8(12, 9);
            return bytes;
          })(),
        ),
        receivedAt: at(minute + 2),
      });

      rows.push({
        journalCounter: journal++,
        direction: "rx",
        opcode: EventCode.FrameRx,
        body: b64(
          frameRx(
            frame++,
            peerNode,
            FrameType.Position,
            Math.round(-90 - 12 * Math.cos((minute / 240) * Math.PI)),
            6,
            encodePosition({
              latitudeE7: 472_692_000 + step * 1100,
              longitudeE7: 114_041_000 + step * 1300,
              altitudeM: Math.round(574 + step * 7),
              hdop: 9,
              fixAgeS: 5,
            }),
          ),
        ),
        receivedAt: at(minute + 3),
      });
    }
  }

  // A few messages, including one the budget held and one that gave up -- the
  // three states CLAUDE.md 2.4 requires the UI to distinguish.
  const texts = [
    [95, "on the way to the ridge"],
    [340, "break at the hut"],
    [700, "weather turning, heading back"],
  ] as const;
  for (const [minute, text] of texts) {
    rows.push({
      journalCounter: journal++,
      direction: "rx",
      opcode: EventCode.FrameRx,
      body: b64(frameRx(frame++, peerNode, FrameType.Text, -86, 8, encodeText(text))),
      receivedAt: at(minute),
    });
  }

  const outgoing = [
    { minute: 120, counter: frame++, seq: 21, queued: true, finalResult: 0, attempts: 1 },
    { minute: 410, counter: frame++, seq: 22, queued: false, finalResult: 0, attempts: 2 },
    { minute: 880, counter: frame++, seq: 23, queued: true, finalResult: 1, attempts: 3 },
  ];
  for (const message of outgoing) {
    if (message.queued) {
      rows.push({
        journalCounter: journal++,
        direction: "tx",
        opcode: EventCode.FrameTxResult,
        body: b64(txResult(message.counter, peerNode, message.seq, 2, 0, 0, 0)),
        receivedAt: at(message.minute),
      });
    }
    rows.push({
      journalCounter: journal++,
      direction: "tx",
      opcode: EventCode.FrameTxResult,
      body: b64(
        txResult(message.counter, peerNode, message.seq, message.finalResult,
                 message.attempts, message.finalResult === 0 ? -92 : 0, 5),
      ),
      receivedAt: at(message.minute + 3),
    });
  }

  return rows.sort((a, b) => a.journalCounter - b.journalCounter);
}

async function push(nodeId: number, token: string, rows: Row[]): Promise<void> {
  // The endpoint caps a batch at 512 events, and a seeder that ignores its own
  // API's limits is a seeder that hides a bug in it.
  for (let offset = 0; offset < rows.length; offset += 400) {
    const slice = rows.slice(offset, offset + 400);
    const response = await fetch(`${BASE_URL}/api/ingest`, {
      method: "POST",
      headers: { "content-type": "application/json", authorization: `Bearer ${token}` },
      body: JSON.stringify({ nodeId, events: slice }),
    });
    if (!response.ok) {
      throw new Error(`ingest failed: ${response.status} ${await response.text()}`);
    }
    const body = await response.json();
    console.log(
      `  node ${nodeId}: +${body.inserted} inserted, ${body.duplicates} already there`,
    );
  }
}

const tokenA = process.env.SEED_TOKEN_A;
const tokenB = process.env.SEED_TOKEN_B;
if (!tokenA) {
  console.error(
    "SEED_TOKEN_A is not set.\n" +
      "  npm run device:add -- --node-id 1 --name 'T-Echo A'\n" +
      "  npm run device:add -- --node-id 2 --name 'T-Echo B'\n" +
      "  SEED_TOKEN_A=... SEED_TOKEN_B=... npm run seed",
  );
  process.exit(2);
}

const start = new Date(Date.now() - 24 * 3600 * 1000);
start.setUTCMinutes(0, 0, 0);

console.log(`seeding ${BASE_URL} with 24 h ending now`);
await push(1, tokenA, dayFor(1, 2, start));
if (tokenB) {
  await push(2, tokenB, dayFor(2, 1, start));
} else {
  console.log("  SEED_TOKEN_B not set -- node 2 will only appear as a peer observation");
}
console.log("done");
