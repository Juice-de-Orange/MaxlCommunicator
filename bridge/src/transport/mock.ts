/**
 * A transport backed by a simulated device.
 *
 * This is what makes the connection lifecycle in `docs/bridge-protocol.md` §5
 * testable without hardware. The mock device answers commands the way the
 * firmware is specified to -- including refusing bonded-tier commands on an
 * unbonded connection, which is the rule that stops a network key being settable
 * by anyone in Bluetooth range.
 */

import { Reassembler, MsgIdCounter, chunk } from "../protocol/chunker.js";
import { Writer, decodeMessage, encodeJournalEntry, encodeMessage } from "../protocol/codec.js";
import { BRIDGE_PROTOCOL_VERSION, BridgeError, EventCode, Opcode, tierOf } from "../protocol/opcodes.js";
import type { Transport } from "./types.js";

export interface MockDeviceOptions {
  readonly mtu?: number;
  readonly bonded?: boolean;
  /** Journal events the device is holding, keyed by frame counter. */
  /**
   * The journal the device is holding.
   *
   * `opcode` defaults to EVT_FRAME_RX, which is what most tests want -- but it
   * is settable, because a device's journal is not only received frames. Six of
   * the seven event types were being dropped by this client until 2026-08-31
   * (decision D15), and a mock that could only produce the seventh could not
   * have shown it.
   */
  readonly queued?: readonly { counter: number; opcode?: number; body: Uint8Array }[];
  /** When false, the node reports no valid time and refuses to transmit. */
  readonly timeValid?: boolean;
  /** Drop the nth chunk written by the client, to exercise §1.1 recovery. */
  readonly dropWrittenChunk?: number;
  /** What `GET_INFO` reports. Set it away from 1 to stage a version mismatch. */
  readonly bridgeProtocol?: number;
  /**
   * Refuse every `SEND_TEXT` with this error code.
   *
   * A node that is out of budget is the ordinary case on a 10 % duty cycle, not
   * an exotic one, and the client has to treat it differently from a refusal it
   * will never get past.
   */
  readonly refuseText?: number;
}

export class MockDevice implements Transport {
  readonly mtu: number;
  bonded: boolean;

  /** Everything the client sent, decoded. Assertions read this. */
  readonly received: { opcode: number; txnId: number; body: Uint8Array }[] = [];

  /// The last config version this mock device was told to apply.
  appliedConfigVersion: number | null = null;
  /** Counter passed to the last ACK_QUEUE, or null if it never arrived. */
  ackedUpTo: number | null = null;
  timeValid: boolean;
  readonly bridgeProtocol: number;

  #assembler = new Reassembler();
  #outIds = new MsgIdCounter();
  #handlers = new Set<(chunk: Uint8Array) => void>();
  #queued: { counter: number; opcode?: number; body: Uint8Array }[];
  #writeCount = 0;
  #dropWrittenChunk: number;
  #now = 0;

  #refuseText: number | undefined;

  constructor(options: MockDeviceOptions = {}) {
    this.mtu = options.mtu ?? 247;
    this.bonded = options.bonded ?? true;
    this.timeValid = options.timeValid ?? true;
    this.bridgeProtocol = options.bridgeProtocol ?? BRIDGE_PROTOCOL_VERSION;
    this.#queued = [...(options.queued ?? [])];
    this.#dropWrittenChunk = options.dropWrittenChunk ?? -1;
    this.#refuseText = options.refuseText;
  }

  async write(chunkBytes: Uint8Array): Promise<void> {
    this.#now += 1;
    this.#writeCount += 1;
    if (this.#writeCount === this.#dropWrittenChunk) {
      return; // the chunk never arrives
    }

    const outcome = this.#assembler.feed(chunkBytes, this.#now);
    if (outcome.kind !== "complete") {
      return;
    }
    const message = decodeMessage(outcome.message);
    this.received.push({ opcode: message.opcode, txnId: message.txnId, body: new Uint8Array(message.body) });
    await this.#handle(message.opcode, message.txnId, message.body);
  }

  subscribe(handler: (chunk: Uint8Array) => void): () => void {
    this.#handlers.add(handler);
    return () => this.#handlers.delete(handler);
  }

  async disconnect(): Promise<void> {
    this.#handlers.clear();
    this.#assembler.reset();
  }

  /** Emit an unsolicited event, as the device does over `TX`. */
  emit(opcode: number, body: Uint8Array): void {
    this.#send(encodeMessage(opcode, 0, body));
  }

  #send(message: Uint8Array): void {
    for (const part of chunk(message, this.mtu, this.#outIds.take())) {
      for (const handler of this.#handlers) {
        handler(part);
      }
    }
  }

  #error(opcode: number, txnId: number, error: number): void {
    this.#send(encodeMessage(EventCode.ResponseError, txnId, Uint8Array.from([opcode, error])));
  }

  #ok(opcode: number, txnId: number, body: Uint8Array = new Uint8Array(0)): void {
    this.#send(encodeMessage(EventCode.ResponseOk, txnId, Uint8Array.from([opcode, ...body])));
  }

  async #handle(opcode: number, txnId: number, body: Uint8Array): Promise<void> {
    // §2: the tier is enforced HERE, on the device, not by the client.
    if (tierOf(opcode) === "bonded" && !this.bonded) {
      this.#error(opcode, txnId, BridgeError.NotAuthorised);
      return;
    }

    switch (opcode) {
      case Opcode.GetInfo:
        this.#ok(opcode, txnId, new Writer().u8(this.bridgeProtocol).u16(0x0001).finish());
        return;

      case Opcode.GetStatus:
        this.#ok(opcode, txnId, new Writer().u16(3987).u32(86400).u8(this.#queued.length).finish());
        return;

      case Opcode.GetBudget:
        this.#ok(opcode, txnId, new Writer().u8(0).u32(42500).u32(360000).u32(0).finish());
        return;

      case Opcode.SetTime:
        this.timeValid = true;
        this.#ok(opcode, txnId);
        return;

      case Opcode.SetConfig: {
        // A real node answers RSP_OK and then emits EVT_CONFIG_APPLIED
        // separately -- the response says it accepted the write, not what it
        // applied. CLAUDE.md 4.3: "a pushed config is not an applied config."
        if (body.length < 4) {
          this.#error(opcode, txnId, BridgeError.BadLength);
          return;
        }
        this.appliedConfigVersion = new DataView(
          body.buffer,
          body.byteOffset,
          body.byteLength,
        ).getUint32(0, true);
        this.#ok(opcode, txnId);
        return;
      }

      case Opcode.GetQueue: {
        const sinceCounter = new DataView(body.buffer, body.byteOffset).getUint32(0, true);
        const maxEvents = body[4] ?? 0;
        const batch = this.#queued.filter((e) => e.counter > sinceCounter).slice(0, maxEvents);
        // The response says how many follow; the events themselves arrive as
        // notifications, which is how the real device streams a journal.
        this.#ok(opcode, txnId, Uint8Array.from([batch.length]));
        for (const event of batch) {
          // Wrapped, because it comes out of the journal: section 4, D15. The
          // counter the client acknowledges with exists only here.
          this.emit(
            EventCode.Journal,
            encodeJournalEntry({
              counter: event.counter,
              opcode: event.opcode ?? EventCode.FrameRx,
              body: event.body,
            }),
          );
        }
        return;
      }

      case Opcode.AckQueue: {
        const upTo = new DataView(body.buffer, body.byteOffset).getUint32(0, true);
        this.ackedUpTo = upTo;
        // "The device frees journal space up to that counter." (§3)
        this.#queued = this.#queued.filter((e) => e.counter > upTo);
        this.#ok(opcode, txnId);
        return;
      }

      case Opcode.SendText:
        if (this.#refuseText !== undefined) {
          this.#error(opcode, txnId, this.#refuseText);
          return;
        }
        if (!this.timeValid) {
          // CLAUDE.md 1.2: no valid time means transmit-blocked.
          this.#error(opcode, txnId, BridgeError.NoTime);
          return;
        }
        this.#ok(opcode, txnId);
        return;

      default:
        // §4: "unknown opcode -- the client should degrade, not fail".
        this.#error(opcode, txnId, BridgeError.Unsupported);
        return;
    }
  }
}
