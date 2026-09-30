/*
 * The IndexedDB store against a real IndexedDB implementation (fake-indexeddb),
 * not a stub of one. The properties worth testing here are transaction
 * semantics, and a stub has none.
 */

import "fake-indexeddb/auto";
import { beforeEach, describe, expect, it } from "vitest";

import { IndexedDbEventStore } from "../src/sync/indexeddb-store";
import { MemoryEventStore, type EventStore, type JournalEvent } from "../src/sync/store";

const event = (counter: number, opcode = 0x81): JournalEvent => ({
  counter,
  opcode,
  body: Uint8Array.from([counter & 0xff, 0xaa, 0xbb]),
});

let dbCounter = 0;
let store: IndexedDbEventStore;

beforeEach(() => {
  // A fresh database per test: IndexedDB is durable by design, which is the
  // whole point and also means state leaks between tests unless it is renamed.
  dbCounter += 1;
  store = new IndexedDbEventStore(`maxl-test-${dbCounter}`);
});

describe("IndexedDbEventStore", () => {
  it("keeps events across a close and reopen", async () => {
    await store.append([event(1), event(2), event(3)]);
    store.close();

    const reopened = new IndexedDbEventStore(`maxl-test-${dbCounter}`);
    expect(await reopened.highWaterMark()).toBe(3);
    expect((await reopened.all()).map((e) => e.counter)).toEqual([1, 2, 3]);
  });

  it("returns from append only once the transaction has committed", async () => {
    // The contract in store.ts is "durable before it returns", and that is not
    // the same as "the request succeeded". If append resolved on the request,
    // this read -- issued immediately after, in a fresh transaction -- could
    // still miss the write.
    await store.append([event(7)]);
    const fresh = new IndexedDbEventStore(`maxl-test-${dbCounter}`);
    expect(await fresh.highWaterMark()).toBe(7);
  });

  it("treats a re-sent event as harmless", async () => {
    await store.append([event(1), event(2)]);
    await store.append([event(2), event(3)]);
    expect((await store.all()).map((e) => e.counter)).toEqual([1, 2, 3]);
  });

  it("reports everything as unpushed until something is acknowledged", async () => {
    await store.append([event(1), event(2), event(3)]);
    expect((await store.unpushed()).map((e) => e.counter)).toEqual([1, 2, 3]);

    await store.markPushed(2);
    expect((await store.unpushed()).map((e) => e.counter)).toEqual([3]);
  });

  it("never moves the pushed mark backwards", async () => {
    // A reply about an older batch arriving after a newer acknowledgement must
    // not make the client re-push events the server already has -- harmless on
    // the server, but it would make the pending count in the UI lie.
    await store.append([event(1), event(2), event(3)]);
    await store.markPushed(3);
    await store.markPushed(1);
    expect(await store.unpushed()).toHaveLength(0);
  });

  it("keeps the body bytes intact", async () => {
    const original = { counter: 9, opcode: 0x82, body: Uint8Array.from([0, 255, 128, 1]) };
    await store.append([original]);
    const [read] = await store.all();
    expect([...read!.body]).toEqual([0, 255, 128, 1]);
    expect(read!.opcode).toBe(0x82);
  });

  it("prunes only what the server has taken", async () => {
    await store.append([event(1), event(2), event(3), event(4)]);
    await store.markPushed(2);
    expect(await store.prunePushed()).toBe(2);
    expect((await store.all()).map((e) => e.counter)).toEqual([3, 4]);
    // And the high water mark still reflects what was received, so GET_QUEUE
    // does not ask the device to re-send from the beginning.
    expect(await store.highWaterMark()).toBe(4);
  });

  it("prunes nothing when nothing has been acknowledged", async () => {
    await store.append([event(1), event(2)]);
    expect(await store.prunePushed()).toBe(0);
    expect(await store.all()).toHaveLength(2);
  });
});

/*
 * The same sequence against both implementations. The in-memory store is what
 * the session tests run on, so if the two ever disagree, those tests are proving
 * something about a store that is not the one shipping.
 */
describe("both stores behave the same", () => {
  async function exercise(subject: EventStore) {
    await subject.append([event(5), event(6)]);
    await subject.append([event(6), event(7)]);
    const before = (await subject.unpushed()).map((e) => e.counter);
    await subject.markPushed(6);
    const after = (await subject.unpushed()).map((e) => e.counter);
    return { high: await subject.highWaterMark(), before, after };
  }

  it("agree on high water mark and unpushed events", async () => {
    expect(await exercise(store)).toEqual(await exercise(new MemoryEventStore()));
  });
});
