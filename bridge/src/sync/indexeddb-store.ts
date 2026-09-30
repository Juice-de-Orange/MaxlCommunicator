/*
 * EventStore on IndexedDB.
 *
 * The second implementation of the interface in store.ts, and the reason that
 * interface exists: MemoryEventStore keeps the session tests honest without a
 * browser, this one is what actually survives the tab being closed.
 *
 * The contract that matters is one line in store.ts -- "Persist a batch. Must be
 * durable before it returns." docs/bridge-protocol.md section 3 has the phone
 * acknowledge the device's journal only *after* the server has the data,
 * "because acknowledging before the server has the data would lose events
 * whenever the phone dies between the two steps, and this is a device you carry
 * into places where the phone dies".
 *
 * An IndexedDB write resolves its request as soon as the object store accepts
 * the value, which is before the transaction commits. Resolving on the request
 * would make append() a promise that means "probably" -- and would put an
 * ACK_QUEUE on the wire for data that a crash could still take away. So this
 * resolves on `transaction.oncomplete` and nowhere else.
 */

import type { Outbox, OutboxEntry } from "./outbox";
import type { EventStore, JournalEvent } from "./store";

const DB_NAME = "maxl-bridge";
/*
 * 2 adds the outbox. The upgrade handler creates only what is missing, so a
 * phone that already holds unpushed events keeps them across the bump -- which
 * matters, because those events exist precisely because the server has not seen
 * them yet.
 */
const DB_VERSION = 2;
const EVENTS = "events";
const META = "meta";
const OUTBOX = "outbox";
const PUSHED_KEY = "pushedUpTo";

interface StoredEvent {
  counter: number;
  opcode: number;
  body: Uint8Array;
}

function open(name: string): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(name, DB_VERSION);
    request.onupgradeneeded = () => {
      const db = request.result;
      if (!db.objectStoreNames.contains(EVENTS)) {
        // Keyed by the journal counter: monotonic, persistent and never reused
        // (CLAUDE.md 2.1), so it is both the natural key and the sort order.
        db.createObjectStore(EVENTS, { keyPath: "counter" });
      }
      if (!db.objectStoreNames.contains(META)) {
        db.createObjectStore(META);
      }
      if (!db.objectStoreNames.contains(OUTBOX)) {
        // autoIncrement rather than a counter of our own: the id only has to
        // order the queue and identify an entry, and IndexedDB's generator
        // already survives a reload, which is the whole point of the store.
        db.createObjectStore(OUTBOX, { keyPath: "id", autoIncrement: true });
      }
    };
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error("indexedDB.open failed"));
  });
}

/** Resolves when the transaction commits, not when the request succeeds. */
function committed(transaction: IDBTransaction): Promise<void> {
  return new Promise((resolve, reject) => {
    transaction.oncomplete = () => resolve();
    transaction.onabort = () => reject(transaction.error ?? new Error("transaction aborted"));
    transaction.onerror = () => reject(transaction.error ?? new Error("transaction failed"));
  });
}

function asPromise<T>(request: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error("request failed"));
  });
}

export class IndexedDbEventStore implements EventStore {
  #db: IDBDatabase | null = null;

  constructor(private readonly name: string = DB_NAME) {}

  async #handle(): Promise<IDBDatabase> {
    this.#db ??= await open(this.name);
    return this.#db;
  }

  async append(events: readonly JournalEvent[]): Promise<void> {
    if (events.length === 0) {
      return;
    }
    const db = await this.#handle();
    const transaction = db.transaction(EVENTS, "readwrite");
    const store = transaction.objectStore(EVENTS);
    for (const event of events) {
      // put, not add: a re-sent event is expected and harmless. The server's
      // idempotency key makes duplicates free on that side too, which is why
      // erring towards re-sending is always the right call.
      store.put({
        counter: event.counter,
        opcode: event.opcode,
        body: event.body,
      } satisfies StoredEvent);
    }
    await committed(transaction);
  }

  async highWaterMark(): Promise<number> {
    const db = await this.#handle();
    const store = db.transaction(EVENTS, "readonly").objectStore(EVENTS);
    const cursor = await asPromise(store.openCursor(null, "prev"));
    return cursor ? (cursor.key as number) : 0;
  }

  async unpushed(): Promise<JournalEvent[]> {
    const db = await this.#handle();
    const transaction = db.transaction([EVENTS, META], "readonly");
    const pushedUpTo =
      (await asPromise(transaction.objectStore(META).get(PUSHED_KEY))) ?? 0;
    const range =
      pushedUpTo > 0 ? IDBKeyRange.lowerBound(pushedUpTo, true) : undefined;
    const rows = (await asPromise(
      transaction.objectStore(EVENTS).getAll(range),
    )) as StoredEvent[];
    return rows.map((row) => ({ counter: row.counter, opcode: row.opcode, body: row.body }));
  }

  async markPushed(counter: number): Promise<void> {
    const db = await this.#handle();
    const transaction = db.transaction(META, "readwrite");
    const store = transaction.objectStore(META);
    const current = ((await asPromise(store.get(PUSHED_KEY))) as number | undefined) ?? 0;
    // Never moves backwards. A late reply about an older batch must not undo a
    // newer acknowledgement.
    store.put(Math.max(current, counter), PUSHED_KEY);
    await committed(transaction);
  }

  /** Everything held, oldest first. For the UI's pending count. */
  async all(): Promise<JournalEvent[]> {
    const db = await this.#handle();
    const store = db.transaction(EVENTS, "readonly").objectStore(EVENTS);
    const rows = (await asPromise(store.getAll())) as StoredEvent[];
    return rows.map((row) => ({ counter: row.counter, opcode: row.opcode, body: row.body }));
  }

  /** Drop everything the server has taken. Journal space on a phone is not
   *  scarce, so this is called rarely and only on an explicit request. */
  async prunePushed(): Promise<number> {
    const db = await this.#handle();
    const transaction = db.transaction([EVENTS, META], "readwrite");
    const pushedUpTo =
      ((await asPromise(transaction.objectStore(META).get(PUSHED_KEY))) as number | undefined) ?? 0;
    if (pushedUpTo <= 0) {
      await committed(transaction);
      return 0;
    }
    const events = transaction.objectStore(EVENTS);
    const keys = (await asPromise(
      events.getAllKeys(IDBKeyRange.upperBound(pushedUpTo)),
    )) as number[];
    for (const key of keys) {
      events.delete(key);
    }
    await committed(transaction);
    return keys.length;
  }

  close(): void {
    this.#db?.close();
    this.#db = null;
  }
}


/**
 * The outbox in IndexedDB. Same shape as [[MemoryOutbox]], same guarantees, and
 * this one survives the tab being closed -- which is the case it exists for.
 */
export class IndexedDbOutbox implements Outbox {
  #db: Promise<IDBDatabase> | null = null;

  constructor(private readonly name: string = DB_NAME) {}

  #open(): Promise<IDBDatabase> {
    this.#db ??= open(this.name);
    return this.#db;
  }

  async add(dst: number, text: string, queuedAt: number): Promise<OutboxEntry> {
    const db = await this.#open();
    const transaction = db.transaction(OUTBOX, "readwrite");
    const id = await asPromise(
      transaction.objectStore(OUTBOX).add({ dst, text, queuedAt, attempts: 0 }),
    );
    await committed(transaction);
    return { id: id as number, dst, text, queuedAt, attempts: 0 };
  }

  async pending(): Promise<OutboxEntry[]> {
    const db = await this.#open();
    const store = db.transaction(OUTBOX, "readonly").objectStore(OUTBOX);
    const entries = (await asPromise(store.getAll())) as OutboxEntry[];
    return entries.sort((a, b) => a.id - b.id);
  }

  async remove(id: number): Promise<void> {
    const db = await this.#open();
    const transaction = db.transaction(OUTBOX, "readwrite");
    transaction.objectStore(OUTBOX).delete(id);
    await committed(transaction);
  }

  async retryLater(id: number): Promise<void> {
    const db = await this.#open();
    const transaction = db.transaction(OUTBOX, "readwrite");
    const store = transaction.objectStore(OUTBOX);
    const existing = (await asPromise(store.get(id))) as OutboxEntry | undefined;
    if (existing) {
      store.put({ ...existing, attempts: existing.attempts + 1 });
    }
    await committed(transaction);
  }
}
