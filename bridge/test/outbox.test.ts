/**
 * The outbox: text written while nothing was connected.
 *
 * `CLAUDE.md` §4.2 builds the product around the foreground-only constraint --
 * "the device queues everything, and the user opens the app to sync". That
 * covers device to phone. In the other direction there is no device to queue
 * anything, so the phone is the only place a message can wait, and until
 * 2026-08-31 the PWA had nowhere to put one: the send control was hidden while
 * disconnected. The Kotlin client had this from its first commit, which is how
 * the gap became visible.
 */

import { describe, expect, it } from "vitest";

import { BridgeError, Opcode } from "../src/protocol/opcodes.js";
import { MemoryOutbox } from "../src/sync/outbox.js";
import { BridgeSession } from "../src/sync/session.js";
import { MemoryEventStore } from "../src/sync/store.js";
import { MockDevice } from "../src/transport/mock.js";

function session(device: MockDevice): BridgeSession {
  return new BridgeSession(device, new MemoryEventStore());
}

const run = { nowUnix: 1788000000, push: async () => {} };

describe("outbox", () => {
  it("keeps FIFO order", async () => {
    // The order the user wrote them in is the order they read in. Nothing else
    // would be defensible on a screen showing them as a list.
    const outbox = new MemoryOutbox();
    await outbox.add(2, "first", 1000);
    await outbox.add(3, "second", 1001);
    await outbox.add(2, "third", 1002);

    expect((await outbox.pending()).map((entry) => entry.text)).toEqual([
      "first", "second", "third",
    ]);
  });

  it("sends everything waiting, in order, at step 7", async () => {
    const outbox = new MemoryOutbox();
    await outbox.add(2, "one", 1000);
    await outbox.add(3, "two", 1001);
    const device = new MockDevice();

    const result = await session(device).run({ ...run, outbox });

    expect(result.textsSent).toBe(2);
    expect(result.textsHeld).toBe(0);
    expect(await outbox.pending()).toHaveLength(0);

    // Step 7 is last: the journal is drained before anything is spent on
    // sending. A connection that dies halfway has still collected the events.
    const opcodes = device.received.map((message) => message.opcode);
    expect(opcodes.indexOf(Opcode.SendText)).toBeGreaterThan(opcodes.lastIndexOf(Opcode.GetQueue));
  });

  it("keeps a text the node refused for budget", async () => {
    // ERR_BUDGET_EXHAUSTED is "not yet", and on a 10 % duty cycle it is the
    // ordinary answer, not a fault. Dropping it here would lose a message the
    // node never refused on its merits.
    const outbox = new MemoryOutbox();
    await outbox.add(2, "waiting", 1000);
    const device = new MockDevice({ refuseText: BridgeError.BudgetExhausted });

    const result = await session(device).run({ ...run, outbox });

    expect(result.textsSent).toBe(0);
    expect(result.textsHeld).toBe(1);
    expect(result.refusal?.code).toBe(BridgeError.BudgetExhausted);

    const waiting = await outbox.pending();
    expect(waiting[0]?.text).toBe("waiting");
    expect(waiting[0]?.attempts).toBe(1);
  });

  it("keeps a text the node refused because its queue is full", async () => {
    const outbox = new MemoryOutbox();
    await outbox.add(2, "waiting", 1000);
    const device = new MockDevice({ refuseText: BridgeError.QueueFull });

    const result = await session(device).run({ ...run, outbox });

    expect(result.textsHeld).toBe(1);
  });

  it("drops a text the node refused on its own merits", async () => {
    // ERR_BAD_PARAM will not become true later: the same bytes get the same
    // answer for ever, so keeping it spends a write on every sync.
    const outbox = new MemoryOutbox();
    await outbox.add(2, "broken", 1000);
    const device = new MockDevice({ refuseText: BridgeError.BadParam });

    const result = await session(device).run({ ...run, outbox });

    expect(result.textsHeld).toBe(0);
    expect(result.refusal?.code).toBe(BridgeError.BadParam);
  });

  it("keeps a text refused because the node has no valid clock", async () => {
    /*
     * The correction that matters. ERR_NO_TIME looks like a hard refusal and is
     * not one: the node is transmit-blocked because its RTC is invalid
     * (CLAUDE.md 1.2), and step 4 of this very sequence is one of the two ways
     * out of that state. Dropping the message here would lose it for a reason
     * that has nothing to do with the message -- and would do so most often
     * on a node fresh out of the drawer, which is exactly when the user is
     * least likely to notice.
     */
    const outbox = new MemoryOutbox();
    await outbox.add(2, "after setting the time", 1000);
    const device = new MockDevice({ refuseText: BridgeError.NoTime });

    const result = await session(device).run({ ...run, outbox });

    expect(result.textsHeld).toBe(1);
    expect(result.refusal?.code).toBe(BridgeError.NoTime);
  });

  it("keeps a text refused for want of bonding or a key", async () => {
    // Both are answered by doing something to the node, not by rewriting the
    // message. Same rule, and the same reason.
    for (const code of [BridgeError.NotAuthorised, BridgeError.NoKey]) {
      const outbox = new MemoryOutbox();
      await outbox.add(2, "waiting", 1000);
      const result = await session(new MockDevice({ refuseText: code })).run({ ...run, outbox });
      expect(result.textsHeld, `error 0x${code.toString(16)}`).toBe(1);
    }
  });

  it("counts attempts instead of giving up on a message", async () => {
    // The attempt counter is for showing the user, never for discarding. A cap
    // that dropped the entry would be the same data loss, reached more slowly.
    const outbox = new MemoryOutbox();
    await outbox.add(2, "stubborn", 1000);

    for (let i = 0; i < 5; i += 1) {
      const device = new MockDevice({ refuseText: BridgeError.BudgetExhausted });
      await session(device).run({ ...run, outbox });
    }

    const waiting = await outbox.pending();
    expect(waiting).toHaveLength(1);
    expect(waiting[0]?.attempts).toBe(5);
  });

  it("goes out on the next connection after a budget refusal", async () => {
    const outbox = new MemoryOutbox();
    await outbox.add(2, "later", 1000);

    const blocked = new MockDevice({ refuseText: BridgeError.BudgetExhausted });
    await session(blocked).run({ ...run, outbox });
    expect(await outbox.pending()).toHaveLength(1);

    const free = new MockDevice();
    const second = await session(free).run({ ...run, outbox });

    expect(second.textsSent).toBe(1);
    expect(await outbox.pending()).toHaveLength(0);
  });

  it("does not let a refusal abort the sync", async () => {
    // The journal was fetched before step 7 ran. Throwing here would discard a
    // completed piece of work because of an answer the node was entitled to
    // give -- and the next connection would fetch it all again.
    const outbox = new MemoryOutbox();
    await outbox.add(2, "refused", 1000);
    const device = new MockDevice({
      refuseText: BridgeError.BudgetExhausted,
      queued: [1, 2].map((counter) => ({ counter, body: new Uint8Array(11) })),
    });

    const result = await session(device).run({ ...run, outbox });

    expect(result.eventsFetched).toBe(2);
    expect(result.refusal).not.toBeNull();
  });

  it("reports nothing when there is no outbox at all", async () => {
    // The parameter is optional, and a caller that does not pass one must not
    // get counts implying it sent something.
    const result = await session(new MockDevice()).run(run);
    expect(result.textsSent).toBe(0);
    expect(result.textsHeld).toBe(0);
    expect(result.refusal).toBeNull();
  });
});
