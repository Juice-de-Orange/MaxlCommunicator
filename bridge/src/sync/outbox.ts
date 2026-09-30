/**
 * Text the user wrote while nothing was connected.
 *
 * `CLAUDE.md` §4.2 designs the product around the foreground-only constraint:
 * "the device queues everything, and the user opens the app to sync". That
 * sentence covers one direction. In the other direction there is no device to
 * queue anything -- the phone is the only place a message can wait, and until
 * now the PWA had nowhere to put one. The send control was simply absent while
 * disconnected, which is honest but means the user has to be holding the node to
 * write to it.
 *
 * Durable rather than in-memory for the same reason the event store is: this is
 * a phone carried into places where phones die, and a message that survived
 * being typed but not being backgrounded would be the worst of both.
 */

export interface OutboxEntry {
  /** Assigned by the store, ascending. Ordering is FIFO and visible to the user. */
  readonly id: number;
  readonly dst: number;
  readonly text: string;
  /** When the user wrote it, so the UI can say how long it has been waiting. */
  readonly queuedAt: number;
  /** How many connections have tried and been refused. */
  readonly attempts: number;
}

export interface Outbox {
  add(dst: number, text: string, queuedAt: number): Promise<OutboxEntry>;

  /** Everything waiting, oldest first. */
  pending(): Promise<OutboxEntry[]>;

  /** It went out. */
  remove(id: number): Promise<void>;

  /** It was refused for a reason that may pass. Counts an attempt. */
  retryLater(id: number): Promise<void>;
}

export class MemoryOutbox implements Outbox {
  #entries: OutboxEntry[] = [];
  #nextId = 1;

  async add(dst: number, text: string, queuedAt: number): Promise<OutboxEntry> {
    const entry: OutboxEntry = { id: this.#nextId++, dst, text, queuedAt, attempts: 0 };
    this.#entries.push(entry);
    return entry;
  }

  async pending(): Promise<OutboxEntry[]> {
    return [...this.#entries].sort((a, b) => a.id - b.id);
  }

  async remove(id: number): Promise<void> {
    this.#entries = this.#entries.filter((entry) => entry.id !== id);
  }

  async retryLater(id: number): Promise<void> {
    this.#entries = this.#entries.map((entry) =>
      entry.id === id ? { ...entry, attempts: entry.attempts + 1 } : entry,
    );
  }
}
