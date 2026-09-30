/**
 * The message layer and its bodies. `docs/bridge-protocol.md` §1.2, §3, §4.
 *
 * ```
 * byte 0  : opcode
 * byte 1  : txnId    echoed in the response; events carry 0
 * byte 2..: body
 * ```
 *
 * "All multi-byte integers are little endian. Strings are UTF-8,
 * length-prefixed, never null-terminated."
 */

import { BridgeError, ConfigTlv, EventCode, Opcode, TxResult } from "./opcodes.js";

export const MESSAGE_HEADER_BYTES = 2;

export interface Message {
  readonly opcode: number;
  readonly txnId: number;
  readonly body: Uint8Array;
}

export function encodeMessage(opcode: number, txnId: number, body: Uint8Array = new Uint8Array(0)): Uint8Array {
  const out = new Uint8Array(MESSAGE_HEADER_BYTES + body.length);
  out[0] = opcode & 0xff;
  out[1] = txnId & 0xff;
  out.set(body, MESSAGE_HEADER_BYTES);
  return out;
}

export function decodeMessage(bytes: Uint8Array): Message {
  if (bytes.length < MESSAGE_HEADER_BYTES) {
    throw new RangeError(`message is ${bytes.length} bytes; the header alone is ${MESSAGE_HEADER_BYTES}`);
  }
  return {
    opcode: bytes[0]!,
    txnId: bytes[1]!,
    body: bytes.subarray(MESSAGE_HEADER_BYTES),
  };
}

/** A little-endian reader that refuses to run off the end of its buffer. */
export class Reader {
  #view: DataView;
  #offset = 0;

  constructor(private readonly bytes: Uint8Array) {
    this.#view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }

  get remaining(): number {
    return this.bytes.length - this.#offset;
  }

  #need(count: number): void {
    if (this.remaining < count) {
      throw new RangeError(`need ${count} more byte(s), have ${this.remaining}`);
    }
  }

  u8(): number { this.#need(1); return this.#view.getUint8(this.#offset++); }
  i8(): number { this.#need(1); return this.#view.getInt8(this.#offset++); }

  u16(): number { this.#need(2); const v = this.#view.getUint16(this.#offset, true); this.#offset += 2; return v; }
  i16(): number { this.#need(2); const v = this.#view.getInt16(this.#offset, true); this.#offset += 2; return v; }

  u32(): number { this.#need(4); const v = this.#view.getUint32(this.#offset, true); this.#offset += 4; return v; }
  i32(): number { this.#need(4); const v = this.#view.getInt32(this.#offset, true); this.#offset += 4; return v; }

  take(count: number): Uint8Array {
    this.#need(count);
    const slice = this.bytes.subarray(this.#offset, this.#offset + count);
    this.#offset += count;
    return slice;
  }

  utf8(count: number): string {
    return new TextDecoder("utf-8").decode(this.take(count));
  }
}

/** A little-endian writer. */
export class Writer {
  #bytes: number[] = [];

  u8(value: number): this { this.#bytes.push(value & 0xff); return this; }
  i8(value: number): this { return this.u8(value < 0 ? value + 0x100 : value); }

  u16(value: number): this { this.#bytes.push(value & 0xff, (value >>> 8) & 0xff); return this; }
  i16(value: number): this { return this.u16(value < 0 ? value + 0x10000 : value); }

  u32(value: number): this {
    this.#bytes.push(value & 0xff, (value >>> 8) & 0xff, (value >>> 16) & 0xff, (value >>> 24) & 0xff);
    return this;
  }
  i32(value: number): this { return this.u32(value < 0 ? value + 0x100000000 : value); }

  bytes(values: Uint8Array): this { for (const b of values) this.#bytes.push(b); return this; }

  finish(): Uint8Array { return Uint8Array.from(this.#bytes); }
}

// --- TLVs, §3 -------------------------------------------------------------

export interface Tlv {
  readonly type: number;
  readonly value: Uint8Array;
}

/**
 * Walk a TLV run.
 *
 * Unknown types are returned like any other. §3: "Unknown types are ignored and
 * reported back in `EVT_CONFIG_APPLIED` as unapplied, rather than failing the
 * whole write" -- so the caller has to be able to see them.
 *
 * A run that is truncated mid-value throws: a half-read config is not a config.
 */
export function decodeTlvs(body: Uint8Array): Tlv[] {
  const tlvs: Tlv[] = [];
  let cursor = 0;
  while (cursor < body.length) {
    if (cursor + 2 > body.length) {
      throw new RangeError(`TLV run truncated: ${body.length - cursor} byte(s) left, need 2`);
    }
    const type = body[cursor]!;
    const length = body[cursor + 1]!;
    if (cursor + 2 + length > body.length) {
      throw new RangeError(`TLV 0x${type.toString(16)} claims ${length} bytes, ${body.length - cursor - 2} remain`);
    }
    tlvs.push({ type, value: body.subarray(cursor + 2, cursor + 2 + length) });
    cursor += 2 + length;
  }
  return tlvs;
}

export function encodeTlvs(tlvs: readonly Tlv[]): Uint8Array {
  const writer = new Writer();
  for (const tlv of tlvs) {
    if (tlv.value.length > 0xff) {
      throw new RangeError(`TLV 0x${tlv.type.toString(16)} value is ${tlv.value.length} bytes; the length field is one byte`);
    }
    writer.u8(tlv.type).u8(tlv.value.length).bytes(tlv.value);
  }
  return writer.finish();
}

// --- commands, §3 ---------------------------------------------------------

export function encodeSendText(dst: number, text: string): Uint8Array {
  const utf8 = new TextEncoder().encode(text);
  if (utf8.length > 48) {
    // The limit is BYTES, not characters (CLAUDE.md 2.2). A client that
    // truncated by character would overrun it on the first umlaut.
    throw new RangeError(`text is ${utf8.length} UTF-8 bytes; the limit is 48`);
  }
  return new Writer().u16(dst).u8(utf8.length).bytes(utf8).finish();
}

export function encodeGetQueue(sinceCounter: number, maxEvents: number): Uint8Array {
  return new Writer().u32(sinceCounter).u8(maxEvents).finish();
}

export function encodeAckQueue(upToCounter: number): Uint8Array {
  return new Writer().u32(upToCounter).finish();
}

/**
 * One journal entry: its counter, and the event it wraps (section 4, D15).
 *
 * `opcode` is the wrapped event's code, not `EVT_JOURNAL`. `body` is that
 * event's own body, byte for byte -- so it can go straight to `decodeEvent`
 * through `journalMessage()` below.
 */
export interface JournalEnvelope {
  readonly counter: number;
  readonly opcode: number;
  readonly body: Uint8Array;
}

/** The body of an `EVT_JOURNAL` message. Used by the device side and by tests. */
export function encodeJournalEntry(entry: JournalEnvelope): Uint8Array {
  if (entry.body.length > 0xff) {
    throw new Error(`journal body of ${entry.body.length} bytes exceeds the len:u8 field`);
  }
  return new Writer().u32(entry.counter).u8(entry.opcode).u8(entry.body.length).bytes(entry.body).finish();
}

/**
 * Unwrap an `EVT_JOURNAL` message.
 *
 * Throws on a truncated one rather than returning something plausible: an entry
 * whose length field disagrees with its body is a framing error, and treating it
 * as a short event would store a counter against bytes that are not what the
 * device sent.
 */
export function decodeJournalEntry(message: Message): JournalEnvelope {
  const reader = new Reader(message.body);
  const counter = reader.u32();
  const opcode = reader.u8();
  const length = reader.u8();
  const body = reader.take(length);
  return { counter, opcode, body };
}

/**
 * The wrapped event as a message in its own right, for `decodeEvent`.
 *
 * `txnId` is 0 because that is what it was when the device first had it: an
 * event, solicited or not, is never a response (section 1.2).
 */
export function journalMessage(entry: JournalEnvelope): Message {
  return { opcode: entry.opcode, txnId: 0, body: entry.body };
}

export function encodeSetTime(unixSeconds: number): Uint8Array {
  return new Writer().u32(unixSeconds).finish();
}

export function encodeSetConfig(configVersion: number, tlvs: readonly Tlv[]): Uint8Array {
  return new Writer().u32(configVersion).bytes(encodeTlvs(tlvs)).finish();
}

/** §3: `magic:u32 = 0x5245534D`. */
export const FACTORY_RESET_MAGIC = 0x5245534d;

export function encodeFactoryReset(): Uint8Array {
  return new Writer().u32(FACTORY_RESET_MAGIC).finish();
}

// --- events, §4 -----------------------------------------------------------

export interface FrameRxEvent {
  readonly kind: "frame-rx";
  readonly counter: number;
  readonly src: number;
  readonly type: number;
  readonly rssi: number;
  readonly snr: number;
  readonly payload: Uint8Array;
}

export interface FrameTxResultEvent {
  readonly kind: "frame-tx-result";
  readonly counter: number;
  readonly dst: number;
  readonly seq: number;
  readonly result: number;
  readonly attempts: number;
  readonly rssi: number;
  readonly snr: number;
}

export interface BudgetEvent {
  readonly kind: "budget";
  readonly band: number;
  readonly usedMs: number;
  readonly limitMs: number;
  readonly nextTxUnix: number;
}

export interface ResponseErrorEvent {
  readonly kind: "response-error";
  readonly txnId: number;
  readonly failedOpcode: number;
  readonly error: number;
}

export interface UnknownEvent {
  readonly kind: "unknown";
  readonly opcode: number;
  readonly body: Uint8Array;
}

export type DecodedEvent =
  | FrameRxEvent
  | FrameTxResultEvent
  | BudgetEvent
  | ResponseErrorEvent
  | UnknownEvent;

/**
 * Decode an event.
 *
 * Unknown opcodes come back as `unknown` rather than throwing.
 * `docs/versioning-and-updates.md` §2: "A client that sees an unknown event
 * opcode discards it and continues; it does not disconnect." The device is
 * upgraded independently of the phone, and refusing to talk to a newer node
 * because it emitted an event we have not heard of would be the wrong failure.
 */
export function decodeEvent(message: Message): DecodedEvent {
  const reader = new Reader(message.body);
  switch (message.opcode) {
    case EventCode.FrameRx: {
      const counter = reader.u32();
      const src = reader.u16();
      const type = reader.u8();
      const rssi = reader.i16();
      const snr = reader.i8();
      const length = reader.u8();
      return { kind: "frame-rx", counter, src, type, rssi, snr, payload: reader.take(length) };
    }
    case EventCode.FrameTxResult:
      return {
        kind: "frame-tx-result",
        counter: reader.u32(),
        dst: reader.u16(),
        seq: reader.u8(),
        result: reader.u8(),
        attempts: reader.u8(),
        rssi: reader.i16(),
        snr: reader.i8(),
      };
    case EventCode.Budget:
      return {
        kind: "budget",
        band: reader.u8(),
        usedMs: reader.u32(),
        limitMs: reader.u32(),
        nextTxUnix: reader.u32(),
      };
    case EventCode.ResponseError:
      return {
        kind: "response-error",
        txnId: message.txnId,
        failedOpcode: reader.u8(),
        error: reader.u8(),
      };
    default:
      return { kind: "unknown", opcode: message.opcode, body: message.body };
  }
}

export { BridgeError, ConfigTlv, EventCode, Opcode, TxResult };
