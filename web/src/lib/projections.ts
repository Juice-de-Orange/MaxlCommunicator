/*
 * Projections over the event log.
 *
 * CLAUDE.md 4.3: "all current state ... is derived by projection, never mutated
 * in place". Everything in this file is a pure function of a list of events, and
 * that is not a stylistic preference -- it is what gate 7.2 tests. "Projections
 * correct regardless of arrival order" is only provable if arrival order is not
 * an input, so the first thing project() does is sort by frame counter, which is
 * the device's own causal order (CLAUDE.md 2.1: monotonic, never reused).
 *
 * No database types appear here. The tests exercise this module directly, with
 * no PostgreSQL in the room, and the writer in db/apply-projections.ts is the
 * only thing that knows about tables.
 */

import { decodeEvent, type DecodedEvent } from "@protocol/codec";
import { EventCode, TxResult } from "@protocol/opcodes";
import {
  decodePosition,
  decodeStatus,
  decodeTelemetry,
  decodeText,
  FrameType,
  POSITION_BYTES,
  STATUS_BYTES,
  TELEMETRY_BYTES,
} from "@protocol/payloads";

export interface JournalEvent {
  /** Identifies this journal entry, not the frame it talks about (D10). */
  readonly journalCounter: number;
  readonly direction: "rx" | "tx";
  readonly opcode: number;
  readonly body: Uint8Array;
  readonly receivedAt: Date;
}

export type MessageState =
  | "queued"
  | "in_flight"
  | "delivered"
  | "undelivered"
  | "dropped"
  | "received";

export interface MessageProjection {
  frameCounter: number;
  direction: "rx" | "tx";
  peerNodeId: number | null;
  seq: number | null;
  text: string | null;
  state: MessageState;
  attempts: number | null;
  rssi: number | null;
  snr: number | null;
  occurredAt: Date;
  stateChangedAt: Date;
}

export interface LinkStatProjection {
  frameCounter: number;
  peerNodeId: number;
  rssi: number | null;
  snr: number | null;
  sf: number | null;
  occurredAt: Date;
}

export interface PeerObservationProjection {
  frameCounter: number;
  peerNodeId: number;
  frameType: number;
  latDeg7: number | null;
  lonDeg7: number | null;
  altM: number | null;
  hdop: number | null;
  tempC100: number | null;
  humidity100: number | null;
  pressurePa: number | null;
  batteryMv: number | null;
  uptimeS: number | null;
  occurredAt: Date;
}

export interface DeviceStateProjection {
  lastEventAt: Date | null;
  highestCounter: number | null;
  latDeg7: number | null;
  lonDeg7: number | null;
  altM: number | null;
  hdop: number | null;
  positionAt: Date | null;
  tempC100: number | null;
  humidity100: number | null;
  pressurePa: number | null;
  batteryMv: number | null;
  uptimeS: number | null;
  telemetryAt: Date | null;
  budgetBand: number | null;
  budgetUsedMs: number | null;
  budgetLimitMs: number | null;
  budgetNextTxAt: Date | null;
  budgetAt: Date | null;
  queueDepth: number | null;
  /** CLAUDE.md 1.2: a node with no valid time is transmit-blocked. The UI has to
   *  be able to say that rather than showing a device that just will not send. */
  timeValid: boolean | null;
  keyProvisioned: boolean | null;
}

export interface ConfigApplication {
  configVersion: number;
  appliedMask: number;
  unappliedTypes: Uint8Array;
  appliedAt: Date;
}

export interface DeviceSampleProjection {
  journalCounter: number;
  batteryMv: number | null;
  uptimeS: number | null;
  queueDepth: number | null;
  budgetBand: number | null;
  budgetUsedMs: number | null;
  budgetLimitMs: number | null;
  occurredAt: Date;
}

export interface Projection {
  state: DeviceStateProjection;
  samples: DeviceSampleProjection[];
  messages: MessageProjection[];
  linkStats: LinkStatProjection[];
  peerObservations: PeerObservationProjection[];
  configApplications: ConfigApplication[];
}

function emptyState(): DeviceStateProjection {
  return {
    lastEventAt: null,
    highestCounter: null,
    latDeg7: null,
    lonDeg7: null,
    altM: null,
    hdop: null,
    positionAt: null,
    tempC100: null,
    humidity100: null,
    pressurePa: null,
    batteryMv: null,
    uptimeS: null,
    telemetryAt: null,
    budgetBand: null,
    budgetUsedMs: null,
    budgetLimitMs: null,
    budgetNextTxAt: null,
    budgetAt: null,
    queueDepth: null,
    timeValid: null,
    keyProvisioned: null,
  };
}

/**
 * docs/bridge-protocol.md section 4: result 2 (queued) "may be followed later by
 * 0 or 1 for the same counter -- the phone and the server must treat the journal
 * as a log of state transitions, not as a set of final outcomes".
 *
 * So a later transition wins, but only if it is actually later in the log. A
 * `delivered` must never be overwritten by a `queued` that arrived out of order
 * over the network, which is exactly what gate 7.2 is asking about -- and is why
 * this compares counters rather than trusting the sequence of insertions.
 */
function txResultToState(result: number): MessageState {
  switch (result) {
    case TxResult.Delivered:
      return "delivered";
    case TxResult.Undelivered:
      return "undelivered";
    case TxResult.Queued:
      return "queued";
    case TxResult.DroppedByUser:
      return "dropped";
    default:
      return "in_flight";
  }
}

/**
 * EVT_STATUS carries the status body from docs/bridge-protocol.md section 4 --
 * a layout that had to be written into that document before this could be
 * decoded at all. Shared with the bridge rather than re-read here, for the same
 * reason the frame decoders are.
 */
function readStatus(body: Uint8Array) {
  if (body.length < STATUS_BYTES) {
    return null;
  }
  return decodeStatus(body.slice(0, STATUS_BYTES));
}

/**
 * EVT_FIX: lat i32, lon i32, alt i16, hdop u8, fixAge u8, satellites u8.
 *
 * `hdop` is in TENTHS -- 9 is 0.9, 13 is 1.3, and 255 means both "25.5 or worse"
 * and "not known". Stored raw here; anything that displays it divides by 10.
 * See docs/decisions/0001-open-decisions.md D11.
 */
function readFix(
  body: Uint8Array,
): { latDeg7: number; lonDeg7: number; altM: number; hdop: number } | null {
  if (body.length < 12) {
    return null;
  }
  const view = new DataView(body.buffer, body.byteOffset, body.byteLength);
  return {
    latDeg7: view.getInt32(0, true),
    lonDeg7: view.getInt32(4, true),
    altM: view.getInt16(8, true),
    hdop: view.getUint8(10),
  };
}

/** EVT_CONFIG_APPLIED: configVersion u32, appliedMask u32, unappliedTypes[]. */
function readConfigApplied(
  body: Uint8Array,
): { configVersion: number; appliedMask: number; unappliedTypes: Uint8Array } | null {
  if (body.length < 8) {
    return null;
  }
  const view = new DataView(body.buffer, body.byteOffset, body.byteLength);
  return {
    configVersion: view.getUint32(0, true),
    appliedMask: view.getUint32(4, true),
    unappliedTypes: body.slice(8),
  };
}

export function project(events: readonly JournalEvent[]): Projection {
  // The whole order-independence guarantee rests on this line. Journal counters
  // are monotonic and persistent per device (CLAUDE.md 2.1), so sorting by them
  // reconstructs the order the device wrote the entries in, whatever order they
  // reached the server. The direction is a tiebreaker only so the result is
  // fully deterministic; rx and tx never share a counter in practice.
  const ordered = [...events].sort(
    (a, b) => a.journalCounter - b.journalCounter || a.direction.localeCompare(b.direction),
  );

  const state = emptyState();
  const messages = new Map<string, MessageProjection>();
  const linkStats: LinkStatProjection[] = [];
  const peerObservations: PeerObservationProjection[] = [];
  const samples: DeviceSampleProjection[] = [];
  const configApplications: ConfigApplication[] = [];

  for (const event of ordered) {
    if (state.lastEventAt === null || event.receivedAt > state.lastEventAt) {
      state.lastEventAt = event.receivedAt;
    }
    if (state.highestCounter === null || event.journalCounter > state.highestCounter) {
      state.highestCounter = event.journalCounter;
    }

    // decodeEvent works on a Message, which is opcode + txnId + body. Events are
    // unsolicited and carry txnId 0 (docs/bridge-protocol.md section 1.2).
    let decoded: DecodedEvent;
    try {
      decoded = decodeEvent({ opcode: event.opcode, txnId: 0, body: event.body });
    } catch {
      // A body this server cannot parse is still a row in the log. Skipping it
      // here loses a projection, not an event -- and the log is the record.
      continue;
    }

    switch (decoded.kind) {
      case "frame-rx": {
        const { src, type: frameType, rssi, snr, payload } = decoded;
        // decoded.counter is the frame's counter; event.journalCounter is the
        // entry's. Everything that identifies a frame uses the former.
        const frameCounter = decoded.counter;

        linkStats.push({
          frameCounter,
          peerNodeId: src,
          rssi,
          snr,
          sf: null, // EVT_FRAME_RX does not carry it; BEACON and PEERS do
          occurredAt: event.receivedAt,
        });

        const observation: PeerObservationProjection = {
          frameCounter,
          peerNodeId: src,
          frameType,
          latDeg7: null,
          lonDeg7: null,
          altM: null,
          hdop: null,
          tempC100: null,
          humidity100: null,
          pressurePa: null,
          batteryMv: null,
          uptimeS: null,
          occurredAt: event.receivedAt,
        };

        if (frameType === FrameType.Position && payload.length >= POSITION_BYTES) {
          const position = decodePosition(payload.slice(0, POSITION_BYTES));
          observation.latDeg7 = position.latitudeE7;
          observation.lonDeg7 = position.longitudeE7;
          observation.altM = position.altitudeM;
          observation.hdop = position.hdop;
        } else if (frameType === FrameType.Telemetry && payload.length >= TELEMETRY_BYTES) {
          const telemetry = decodeTelemetry(payload.slice(0, TELEMETRY_BYTES));
          observation.tempC100 = telemetry.temperatureCentiC;
          observation.humidity100 = telemetry.humidityCentiPercent;
          observation.pressurePa = telemetry.pressurePa;
          observation.batteryMv = telemetry.batteryMv;
          observation.uptimeS = telemetry.uptimeS;
        } else if (frameType === FrameType.Text) {
          messages.set(`rx:${frameCounter}`, {
            frameCounter,
            direction: "rx",
            peerNodeId: src,
            seq: null,
            text: decodeText(payload),
            state: "received",
            attempts: null,
            rssi,
            snr,
            occurredAt: event.receivedAt,
            stateChangedAt: event.receivedAt,
          });
        }
        peerObservations.push(observation);
        break;
      }

      case "frame-tx-result": {
        const key = `tx:${decoded.counter}`;
        const existing = messages.get(key);
        const next: MessageProjection = {
          frameCounter: decoded.counter,
          direction: "tx",
          peerNodeId: decoded.dst,
          seq: decoded.seq,
          // The device journal has no room for the text of an outgoing message
          // -- EVT_FRAME_TX_RESULT is counters and radio numbers. See open
          // decision D10; the peer's EVT_FRAME_RX carries it, and correlating
          // the two is what fills this in.
          text: existing?.text ?? null,
          state: txResultToState(decoded.result),
          attempts: decoded.attempts,
          rssi: decoded.rssi,
          snr: decoded.snr,
          occurredAt: existing?.occurredAt ?? event.receivedAt,
          stateChangedAt: event.receivedAt,
        };
        messages.set(key, next);
        break;
      }

      case "budget": {
        state.budgetBand = decoded.band;
        state.budgetUsedMs = decoded.usedMs;
        state.budgetLimitMs = decoded.limitMs;
        state.budgetNextTxAt =
          decoded.nextTxUnix > 0 ? new Date(decoded.nextTxUnix * 1000) : null;
        state.budgetAt = event.receivedAt;
        samples.push({
          journalCounter: event.journalCounter,
          batteryMv: null,
          uptimeS: null,
          queueDepth: null,
          budgetBand: decoded.band,
          budgetUsedMs: decoded.usedMs,
          budgetLimitMs: decoded.limitMs,
          occurredAt: event.receivedAt,
        });
        break;
      }

      default: {
        // Events the shared decoder does not model as a distinct kind. Their
        // bodies are still specified in docs/bridge-protocol.md section 4 and
        // are read here rather than in the bridge, because only the dashboard
        // needs them.
        if (event.opcode === EventCode.Status) {
          const status = readStatus(event.body);
          if (status !== null) {
            state.batteryMv = status.batteryMv;
            state.uptimeS = status.uptimeS;
            state.telemetryAt = event.receivedAt;
            state.queueDepth = status.queueDepth;
            state.timeValid = (status.flags & 0x01) !== 0;
            state.keyProvisioned = (status.flags & 0x02) !== 0;
            // EVT_BUDGET is emitted when the budget changes; the status body
            // repeats it so a cold read has something to show. Neither is
            // authoritative over the other -- whichever came later in the log
            // wins, and the loop is already walking in counter order.
            state.budgetBand = status.band;
            state.budgetUsedMs = status.budgetUsedMs;
            state.budgetLimitMs = status.budgetLimitMs;
            state.budgetAt = event.receivedAt;
            samples.push({
              journalCounter: event.journalCounter,
              batteryMv: status.batteryMv,
              uptimeS: status.uptimeS,
              queueDepth: status.queueDepth,
              budgetBand: status.band,
              budgetUsedMs: status.budgetUsedMs,
              budgetLimitMs: status.budgetLimitMs,
              occurredAt: event.receivedAt,
            });
          }
        } else if (event.opcode === EventCode.Fix) {
          const fix = readFix(event.body);
          if (fix !== null) {
            state.latDeg7 = fix.latDeg7;
            state.lonDeg7 = fix.lonDeg7;
            state.altM = fix.altM;
            state.hdop = fix.hdop;
            state.positionAt = event.receivedAt;
          }
        } else if (event.opcode === EventCode.ConfigApplied) {
          const applied = readConfigApplied(event.body);
          if (applied !== null) {
            configApplications.push({ ...applied, appliedAt: event.receivedAt });
          }
        }
        break;
      }
    }
  }

  return {
    state,
    samples,
    messages: [...messages.values()].sort((a, b) => a.frameCounter - b.frameCounter),
    linkStats,
    peerObservations,
    configApplications,
  };
}
