/**
 * The connection lifecycle. `docs/bridge-protocol.md` §5.
 *
 * "The client must implement exactly this sequence, because the foreground-only
 * constraint means every connection is short and must be productive":
 *
 *   1. Connect, negotiate MTU.
 *   2. `GET_INFO` -- check `BRIDGE_PROTO` compatibility before anything else.
 *   3. `GET_STATUS`, `GET_BUDGET` -- populate the UI within a second.
 *   4. `SET_TIME` if the node reports no valid time.
 *   5. `GET_QUEUE` in a loop until it returns fewer events than `maxEvents`.
 *   6. Push to server. On success, `ACK_QUEUE`.
 *   7. Flush any outbound `SEND_TEXT` queued while offline.
 *   8. Stay connected while the app is in the foreground.
 *
 * Step 6's ordering is the one that matters and the one this class exists to
 * make hard to get wrong: the server has the data before the device is told it
 * may forget it. Erring towards re-sending is always the right call, because the
 * server's idempotency key makes a duplicate harmless (`CLAUDE.md` §4.3).
 */

import { MsgIdCounter, Reassembler, chunk } from "../protocol/chunker.js";
import {
  Reader,
  decodeEvent,
  decodeJournalEntry,
  journalMessage,
  decodeMessage,
  encodeAckQueue,
  encodeGetQueue,
  encodeMessage,
  encodeSendText,
  encodeSetTime,
  type DecodedEvent,
  type Message,
} from "../protocol/codec.js";
import { BRIDGE_ERROR_NAMES, BRIDGE_PROTOCOL_VERSION, BridgeError, EventCode, Opcode } from "../protocol/opcodes.js";
import type { Transport } from "../transport/types.js";
import type { Outbox } from "./outbox.js";
import type { EventStore, JournalEvent } from "./store.js";

export class BridgeProtocolError extends Error {
  constructor(readonly failedOpcode: number, readonly code: number) {
    super(`${BRIDGE_ERROR_NAMES[code] ?? `error 0x${code.toString(16)}`} for opcode 0x${failedOpcode.toString(16)}`);
    this.name = "BridgeProtocolError";
  }
}

export class VersionMismatchError extends Error {
  constructor(readonly deviceVersion: number) {
    super(
      `device speaks bridge protocol ${deviceVersion}, this client speaks ${BRIDGE_PROTOCOL_VERSION}. ` +
        `${deviceVersion > BRIDGE_PROTOCOL_VERSION ? "The app is behind." : "The node is behind."}`,
    );
    this.name = "VersionMismatchError";
  }
}

export interface DeviceInfo {
  readonly bridgeProtocol: number;
  readonly deviceId: number;
}

export interface SyncResult {
  readonly info: DeviceInfo;
  readonly eventsFetched: number;
  readonly eventsPushed: number;
  readonly acknowledgedUpTo: number | null;
  readonly setTime: boolean;
  /** Texts that went out of the outbox during this connection. */
  readonly textsSent: number;
  /** Texts still waiting afterwards -- refused, or written since. */
  readonly textsHeld: number;
  /**
   * What the node said about the last text it refused, or null.
   *
   * A field rather than a thrown error: a refusal is the node telling the user
   * something true, and it must not abort a sync that has already fetched the
   * journal.
   */
  readonly refusal: BridgeProtocolError | null;
}

/** How many events to ask for per `GET_QUEUE`. */
const QUEUE_BATCH = 32;

/**
 * The refusals that are about the text itself rather than about the node.
 *
 * Deliberately a short list with everything else on the other side: a new error
 * code the device gains later is far more likely to be a state than a verdict on
 * these particular bytes, and guessing wrong in that direction only costs a
 * retry. Guessing wrong in the other direction loses what the user wrote.
 */
const ABOUT_THE_MESSAGE: ReadonlySet<number> = new Set([
  BridgeError.BadLength,
  BridgeError.BadParam,
  BridgeError.Unsupported,
]);

/**
 * How a journal counter reaches this client, since 2026-08-31.
 *
 * It does not have to be guessed any more. `GET_QUEUE` answers with
 * `EVT_JOURNAL` (D15), which carries the journal counter in front of the event
 * it wraps -- so every stored entry has the counter `ACK_QUEUE` speaks in and
 * the one the server's idempotency key needs.
 *
 * What was here before was a synthetic key at 0x40000000 for the six event types
 * that carry no counter of their own, and no `ACK_QUEUE` at all, because
 * acknowledging on a guess frees entries on the device that this phone may never
 * have stored.
 */

export class BridgeSession {
  #transport: Transport;
  #store: EventStore;
  #assembler = new Reassembler();
  #outIds = new MsgIdCounter();
  #unsubscribe: (() => void) | null = null;
  #nextTxn = 1;
  #pending = new Map<number, { resolve: (m: Message) => void; reject: (e: Error) => void }>();
  #eventListeners = new Set<(event: DecodedEvent) => void>();
  #inbox: JournalEvent[] = [];
  #acknowledgedUpTo: number | null = null;
  #now = 0;

  constructor(transport: Transport, store: EventStore) {
    this.#transport = transport;
    this.#store = store;
  }

  open(): void {
    if (this.#unsubscribe) return;
    this.#unsubscribe = this.#transport.subscribe((part) => this.#onChunk(part));
  }

  async close(): Promise<void> {
    this.#unsubscribe?.();
    this.#unsubscribe = null;
    this.#assembler.reset();
    for (const waiter of this.#pending.values()) {
      waiter.reject(new Error("disconnected"));
    }
    this.#pending.clear();
  }

  onEvent(listener: (event: DecodedEvent) => void): () => void {
    this.#eventListeners.add(listener);
    return () => this.#eventListeners.delete(listener);
  }

  #onChunk(part: Uint8Array): void {
    this.#now += 1;
    const outcome = this.#assembler.feed(part, this.#now);
    if (outcome.kind !== "complete") return;

    const message = decodeMessage(outcome.message);

    // §1.2: "Every command produces exactly one response ... carrying the same
    // txnId. Events are unsolicited and carry txnId = 0."
    if (message.txnId !== 0 && this.#pending.has(message.txnId)) {
      const waiter = this.#pending.get(message.txnId)!;
      this.#pending.delete(message.txnId);
      if (message.opcode === EventCode.ResponseError) {
        const reader = new Reader(message.body);
        waiter.reject(new BridgeProtocolError(reader.u8(), reader.u8()));
      } else {
        waiter.resolve(message);
      }
      return;
    }

    /*
     * Two shapes, two meanings (docs/bridge-protocol.md §4).
     *
     * Wrapped in EVT_JOURNAL: this came out of the journal in answer to
     * GET_QUEUE, it carries its counter, and it is the DURABLE copy. It is
     * stored, and it is what ACK_QUEUE later frees.
     *
     * Bare: the device emitted it because something just happened. It carries no
     * counter, so there is nothing to acknowledge and nothing that could be
     * deduplicated against the wrapped copy that follows on the next GET_QUEUE.
     * It drives the live UI and is not stored.
     *
     * Storing the bare copy is what this client used to do -- with a synthetic
     * key, because it had no real one. That is decision D15, and it is why
     * BRIDGE_PROTO went to 2.
     */
    if (message.opcode === EventCode.Journal) {
      const entry = decodeJournalEntry(message);
      this.#inbox.push({
        counter: entry.counter,
        opcode: entry.opcode,
        body: new Uint8Array(entry.body),
      });
      /*
       * Listeners see the wrapped event too, decoded as itself.
       *
       * An unknown wrapped opcode still reaches the store above -- §4 requires
       * the entry to be kept and counted towards ACK_QUEUE, because the counter
       * is legible even when the body is not. Only the listeners lose it, and
       * they lose it as `kind: "unknown"`, which is what they already do for
       * anything they do not know.
       */
      for (const listener of this.#eventListeners) {
        listener(decodeEvent(journalMessage(entry)));
      }
      return;
    }

    const event = decodeEvent(message);
    for (const listener of this.#eventListeners) {
      listener(event);
    }
  }

  async #request(opcode: number, body?: Uint8Array): Promise<Message> {
    const txnId = this.#nextTxn;
    // txnId wraps freely (§1.2), but never lands on 0 -- that value means
    // "unsolicited event" and a response carrying it would be unroutable.
    this.#nextTxn = this.#nextTxn === 0xff ? 1 : this.#nextTxn + 1;

    const message = encodeMessage(opcode, txnId, body);
    const response = new Promise<Message>((resolve, reject) => {
      this.#pending.set(txnId, { resolve, reject });
    });

    for (const part of chunk(message, this.#transport.mtu, this.#outIds.take())) {
      await this.#transport.write(part);
    }
    return response;
  }

  async getInfo(): Promise<DeviceInfo> {
    const response = await this.#request(Opcode.GetInfo);
    const reader = new Reader(response.body);
    reader.u8(); // the echoed opcode
    const info = { bridgeProtocol: reader.u8(), deviceId: reader.u16() };
    // §5 step 2: check compatibility "before anything else".
    if (info.bridgeProtocol !== BRIDGE_PROTOCOL_VERSION) {
      throw new VersionMismatchError(info.bridgeProtocol);
    }
    return info;
  }

  async getStatus(): Promise<Uint8Array> {
    return (await this.#request(Opcode.GetStatus)).body;
  }

  async getBudget(): Promise<Uint8Array> {
    return (await this.#request(Opcode.GetBudget)).body;
  }

  async setTime(unixSeconds: number): Promise<void> {
    await this.#request(Opcode.SetTime, encodeSetTime(unixSeconds));
  }

  /**
   * Push a configuration (docs/bridge-protocol.md `SET_CONFIG`).
   *
   * The response says the node accepted the write; it does not say what it
   * applied. That arrives separately as `EVT_CONFIG_APPLIED`, and the dashboard
   * sets `applied_at` from that event and from nothing else -- so this method
   * deliberately does not report success as "applied".
   */
  async setConfig(configVersion: number, tlvs: Uint8Array): Promise<void> {
    const body = new Uint8Array(4 + tlvs.length);
    new DataView(body.buffer).setUint32(0, configVersion, true);
    body.set(tlvs, 4);
    await this.#request(Opcode.SetConfig, body);
  }

  async sendText(dst: number, text: string): Promise<void> {
    await this.#request(Opcode.SendText, encodeSendText(dst, text));
  }

  /**
   * Steps 5 and 6, in that order and no other.
   *
   * `push` is called with the events; only if it resolves does `ACK_QUEUE` go
   * out. If it rejects, the device keeps them and the next connection fetches
   * them again -- which is exactly the behaviour §3 asks for.
   */
  async syncJournal(push: (events: readonly JournalEvent[]) => Promise<void>): Promise<number> {
    this.#acknowledgedUpTo = null;
    let since = await this.#store.highWaterMark();
    let fetched = 0;

    // "GET_QUEUE in a loop until it returns fewer events than maxEvents."
    for (;;) {
      this.#inbox = [];
      const response = await this.#request(Opcode.GetQueue, encodeGetQueue(since, QUEUE_BATCH));
      const reader = new Reader(response.body);
      reader.u8(); // echoed opcode
      const count = reader.u8();

      if (this.#inbox.length > 0) {
        await this.#store.append(this.#inbox);
        fetched += this.#inbox.length;
        since = await this.#store.highWaterMark();
      }
      if (count < QUEUE_BATCH) break;
    }

    const pending = await this.#store.unpushed();
    if (pending.length === 0) {
      return fetched;
    }

    // The server first. If this throws, nothing is acknowledged and nothing is
    // lost -- the device still holds everything.
    await push(pending);

    const highest = pending[pending.length - 1]!.counter;
    await this.#store.markPushed(highest);

    /*
     * And only now ACK_QUEUE, with a counter that came off the wire.
     *
     * The order is the whole point of §5 steps 5 and 6: the server has the
     * events, the local store has them marked, and only then does the device get
     * permission to free them. Acknowledging earlier would lose everything
     * between the two steps, on a device carried into places where the phone
     * dies.
     *
     * `highest` is a journal counter now, not a guess. Until 2026-08-31 no
     * counter reached this client at all and this call was deliberately absent
     * -- decision D15, resolved by EVT_JOURNAL.
     */
    await this.#request(Opcode.AckQueue, encodeAckQueue(highest));
    this.#acknowledgedUpTo = highest;

    return fetched;
  }

  /**
   * Step 7, and the only step whose failures are not fatal.
   *
   * Three outcomes per text, and the line between the last two is not which
   * error it is but what the error is ABOUT.
   *
   * - it went out, and leaves the outbox;
   * - the refusal is about the MESSAGE -- ERR_BAD_LENGTH, ERR_BAD_PARAM,
   *   ERR_UNSUPPORTED. The same bytes will produce the same answer for ever, so
   *   keeping it spends a write on every sync until somebody notices. It goes,
   *   and the refusal is reported;
   * - the refusal is about the NODE'S STATE -- everything else. It stays, and
   *   the next connection offers it again. ERR_BUDGET_EXHAUSTED is the ordinary
   *   answer at 10 %; ERR_NO_TIME is undone by step 4 of this very sequence;
   *   ERR_NOT_AUTHORISED and ERR_NO_KEY are undone by bonding and provisioning.
   *   Dropping one of these would throw away something the user wrote for a
   *   reason that has nothing to do with what they wrote.
   *
   * A transport failure is different again and is left to propagate: the
   * connection is gone, so the remaining texts have not been refused by
   * anybody and must stay exactly where they are.
   */
  async #flushOutbox(
    outbox: Outbox | undefined,
  ): Promise<{ textsSent: number; textsHeld: number; refusal: BridgeProtocolError | null }> {
    if (!outbox) {
      return { textsSent: 0, textsHeld: 0, refusal: null };
    }

    let sent = 0;
    let refusal: BridgeProtocolError | null = null;

    for (const entry of await outbox.pending()) {
      try {
        await this.sendText(entry.dst, entry.text);
        await outbox.remove(entry.id);
        sent += 1;
      } catch (error) {
        if (!(error instanceof BridgeProtocolError)) {
          throw error;
        }
        refusal = error;
        if (ABOUT_THE_MESSAGE.has(error.code)) {
          await outbox.remove(entry.id);
        } else {
          await outbox.retryLater(entry.id);
        }
      }
    }

    return { textsSent: sent, textsHeld: (await outbox.pending()).length, refusal };
  }

  /** The whole of §5, in order. */
  async run(options: {
    readonly push: (events: readonly JournalEvent[]) => Promise<void>;
    readonly nowUnix: number;
    readonly deviceReportsNoTime?: boolean;
    readonly outbox?: Outbox;
  }): Promise<SyncResult> {
    this.open();

    const info = await this.getInfo();           // 2
    await this.getStatus();                       // 3
    await this.getBudget();

    let setTime = false;
    if (options.deviceReportsNoTime) {            // 4
      await this.setTime(options.nowUnix);
      setTime = true;
    }

    const before = (await this.#store.unpushed()).length;
    const fetched = await this.syncJournal(options.push);   // 5, 6
    const after = (await this.#store.unpushed()).length;

    const flushed = await this.#flushOutbox(options.outbox);   // 7

    return {
      info,
      eventsFetched: fetched,
      eventsPushed: Math.max(0, before + fetched - after),
      acknowledgedUpTo: this.#acknowledgedUpTo,
      setTime,
      ...flushed,
    };
  }
}
