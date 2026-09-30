/**
 * Where journal events live before the server has them.
 *
 * An interface, with an in-memory implementation. IndexedDB is Phase 6 proper
 * and needs a browser; the ordering rule this seam exists to protect does not.
 *
 * `docs/bridge-protocol.md` §3: the phone "writes those events to its own
 * IndexedDB, pushes them to the server, and **only then** sends `ACK_QUEUE`".
 * Acknowledging first would lose events whenever the phone dies between the two
 * steps -- "and this is a device you carry into places where the phone dies".
 */

export interface JournalEvent {
  readonly counter: number;
  readonly opcode: number;
  readonly body: Uint8Array;
}

export interface EventStore {
  /** Persist a batch. Must be durable before it returns. */
  append(events: readonly JournalEvent[]): Promise<void>;

  /** The highest counter this store durably holds, or 0 if it holds nothing. */
  highWaterMark(): Promise<number>;

  /** Events not yet pushed to the server, oldest first. */
  unpushed(): Promise<JournalEvent[]>;

  /** Mark everything up to and including `counter` as pushed. */
  markPushed(counter: number): Promise<void>;
}

export class MemoryEventStore implements EventStore {
  #events: JournalEvent[] = [];
  #pushedUpTo = 0;

  async append(events: readonly JournalEvent[]): Promise<void> {
    for (const event of events) {
      // The server's idempotency key is (device, counter, direction), so a
      // duplicate is harmless there -- but storing it twice locally would make
      // `unpushed()` report work that is already done.
      if (!this.#events.some((existing) => existing.counter === event.counter)) {
        this.#events.push(event);
      }
    }
    this.#events.sort((a, b) => a.counter - b.counter);
  }

  async highWaterMark(): Promise<number> {
    return this.#events.reduce((highest, event) => Math.max(highest, event.counter), 0);
  }

  async unpushed(): Promise<JournalEvent[]> {
    return this.#events.filter((event) => event.counter > this.#pushedUpTo);
  }

  async markPushed(counter: number): Promise<void> {
    this.#pushedUpTo = Math.max(this.#pushedUpTo, counter);
  }

  /** Test helper: everything held, in counter order. */
  get all(): readonly JournalEvent[] {
    return this.#events;
  }
}
