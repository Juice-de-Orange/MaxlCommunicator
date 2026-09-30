/**
 * Radio payload encodings. `CLAUDE.md` §2.2.
 *
 * The phone parses these because `EVT_FRAME_RX` hands over the radio payload
 * verbatim (`docs/bridge-protocol.md` §4) -- the device does not interpret it.
 * That is why `test-vectors/radio_payloads.json` is shared with the firmware
 * rather than each side having its own idea of the layout.
 *
 * Fixed-width binary, little endian. No JSON on the air interface: every byte
 * is a byte of airtime, and airtime is the resource the whole design is built
 * around (`CLAUDE.md` §1.4).
 */

import { Reader, Writer } from "./codec.js";

export const POSITION_BYTES = 12;
export const TELEMETRY_BYTES = 14;
export const ACK_BYTES = 4;
export const MAX_TEXT_BYTES = 48;

/** Frame types, `CLAUDE.md` §2.2. */
export const FrameType = {
  Beacon: 0,
  Data: 1,
  Ack: 2,
  Telemetry: 3,
  Position: 4,
  Text: 5,
  Config: 6,
} as const;
export type FrameType = (typeof FrameType)[keyof typeof FrameType];

export interface Position {
  /** Degrees x 1e7. */
  readonly latitudeE7: number;
  readonly longitudeE7: number;
  readonly altitudeM: number;

  /**
   * Horizontal dilution of precision, in TENTHS. 9 is 0.9; 13 is 1.3.
   *
   * CLAUDE.md 2.2 said "hdop uint8" and stopped there, so the scale was never
   * written down and this byte was passed through uninterpreted. Whole units
   * would report every usable fix as 1 and every bad one as 2, which is a field
   * that looks like it carries information and does not.
   *
   * 255 means both "25.5 or worse" and "not known" -- unusable either way, and
   * the important part is that an unknown value reads as the WORST one. At 0 a
   * missing field would render as a perfect fix.
   *
   * Decided 2026-08-31, see docs/decisions/0001-open-decisions.md D11 and
   * docs/protocol.md. No `ver` bump: the byte layout did not change, only its
   * meaning was written down. Anything displaying this must divide by 10.
   */
  readonly hdop: number;

  readonly fixAgeS: number;
}

export function encodePosition(position: Position): Uint8Array {
  return new Writer()
    .i32(position.latitudeE7)
    .i32(position.longitudeE7)
    .i16(position.altitudeM)
    .u8(position.hdop)
    .u8(position.fixAgeS)
    .finish();
}

export function decodePosition(bytes: Uint8Array): Position {
  if (bytes.length !== POSITION_BYTES) {
    throw new RangeError(`POSITION is ${POSITION_BYTES} bytes, got ${bytes.length}`);
  }
  const reader = new Reader(bytes);
  return {
    latitudeE7: reader.i32(),
    longitudeE7: reader.i32(),
    altitudeM: reader.i16(),
    hdop: reader.u8(),
    fixAgeS: reader.u8(),
  };
}

/** Degrees, for display. The wire carries integers precisely so this is the only
 *  place a float appears. */
export function positionToDegrees(position: Position): { lat: number; lon: number } {
  return { lat: position.latitudeE7 / 1e7, lon: position.longitudeE7 / 1e7 };
}

export interface Telemetry {
  /** Hundredths of a degree Celsius. */
  readonly temperatureCentiC: number;
  /** Hundredths of a per cent RH. */
  readonly humidityCentiPercent: number;
  readonly pressurePa: number;
  readonly batteryMv: number;
  readonly uptimeS: number;
}

export function encodeTelemetry(telemetry: Telemetry): Uint8Array {
  return new Writer()
    .i16(telemetry.temperatureCentiC)
    .u16(telemetry.humidityCentiPercent)
    .u32(telemetry.pressurePa)
    .u16(telemetry.batteryMv)
    .u32(telemetry.uptimeS)
    .finish();
}

export function decodeTelemetry(bytes: Uint8Array): Telemetry {
  if (bytes.length !== TELEMETRY_BYTES) {
    throw new RangeError(`TELEMETRY is ${TELEMETRY_BYTES} bytes, got ${bytes.length}`);
  }
  const reader = new Reader(bytes);
  return {
    temperatureCentiC: reader.i16(),
    humidityCentiPercent: reader.u16(),
    pressurePa: reader.u32(),
    batteryMv: reader.u16(),
    uptimeS: reader.u32(),
  };
}

export interface Ack {
  readonly seq: number;
  readonly rssiDbm: number;
  readonly snrDb: number;
}

export function encodeAck(ack: Ack): Uint8Array {
  return new Writer().u8(ack.seq).i16(ack.rssiDbm).i8(ack.snrDb).finish();
}

export function decodeAck(bytes: Uint8Array): Ack {
  if (bytes.length !== ACK_BYTES) {
    throw new RangeError(`ACK is ${ACK_BYTES} bytes, got ${bytes.length}`);
  }
  const reader = new Reader(bytes);
  return { seq: reader.u8(), rssiDbm: reader.i16(), snrDb: reader.i8() };
}

/**
 * Encode TEXT.
 *
 * The limit is 48 **bytes**, not characters. "Grüße" is five characters and
 * seven bytes; a client that truncated by character would overrun the frame on
 * the first umlaut, and the device would reject it.
 */
export function encodeText(text: string): Uint8Array {
  const utf8 = new TextEncoder().encode(text);
  if (utf8.length > MAX_TEXT_BYTES) {
    throw new RangeError(`text is ${utf8.length} UTF-8 bytes; the limit is ${MAX_TEXT_BYTES}`);
  }
  return utf8;
}

export function decodeText(bytes: Uint8Array): string {
  // Not validated as UTF-8: a message dropped because a byte sequence offended
  // us is worse than one rendered with a replacement character.
  return new TextDecoder("utf-8").decode(bytes);
}

/**
 * The status body from `docs/bridge-protocol.md` §4 -- returned by `GET_STATUS`
 * and carried unchanged by `EVT_STATUS`.
 *
 * Revision 1 of the document defined the event as "see GET_STATUS body" and then
 * never gave that body. The layout below was written into the document when the
 * dashboard needed it: the budget is what the STATUS screen (`CLAUDE.md` §3.3)
 * and `EVT_BUDGET` both report, and an unspecified body meant two independent
 * clients would each have invented their own.
 */
export const STATUS_BYTES = 17;

export const StatusFlag = {
  TimeValid: 1 << 0,
  KeyProvisioned: 1 << 1,
  GnssPowered: 1 << 2,
} as const;
export type StatusFlag = (typeof StatusFlag)[keyof typeof StatusFlag];

export interface Status {
  readonly batteryMv: number;
  readonly uptimeS: number;
  readonly queueDepth: number;
  /** 0 = g3, 1 = g1. Which band the two budget figures refer to. */
  readonly band: number;
  readonly budgetUsedMs: number;
  readonly budgetLimitMs: number;
  readonly flags: number;
}

export function encodeStatus(status: Status): Uint8Array {
  return new Writer()
    .u16(status.batteryMv)
    .u32(status.uptimeS)
    .u8(status.queueDepth)
    .u8(status.band)
    .u32(status.budgetUsedMs)
    .u32(status.budgetLimitMs)
    .u8(status.flags)
    .finish();
}

export function decodeStatus(bytes: Uint8Array): Status {
  if (bytes.length !== STATUS_BYTES) {
    throw new RangeError(`status body is ${STATUS_BYTES} bytes, got ${bytes.length}`);
  }
  const reader = new Reader(bytes);
  return {
    batteryMv: reader.u16(),
    uptimeS: reader.u32(),
    queueDepth: reader.u8(),
    band: reader.u8(),
    budgetUsedMs: reader.u32(),
    budgetLimitMs: reader.u32(),
    flags: reader.u8(),
  };
}

/** `timeValid` is the difference between "the node is quiet" and "the node is
 *  transmit-blocked and cannot tell you when that ends" (`CLAUDE.md` §1.2). */
export function statusHasFlag(status: Status, flag: StatusFlag): boolean {
  return (status.flags & flag) !== 0;
}
