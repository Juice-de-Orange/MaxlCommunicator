/**
 * The connection lifecycle, driven end to end against a mock device.
 *
 * `docs/bridge-protocol.md` §5 says the client "must implement exactly this
 * sequence". This checks that it does -- including the ordering in step 6, which
 * is the part that decides whether events survive a phone dying mid-sync.
 */

import { describe, expect, it } from "vitest";

import { EventCode, Opcode } from "../src/protocol/opcodes.js";
import { MockDevice } from "../src/transport/mock.js";
import { MemoryEventStore } from "../src/sync/store.js";
import { BridgeProtocolError, BridgeSession, VersionMismatchError } from "../src/sync/session.js";

/** A journal event body shaped like EVT_FRAME_RX (section 4). */
function journalEvent(counter: number): Uint8Array {
  const body = new Uint8Array(11 + 12);
  const view = new DataView(body.buffer);
  view.setUint32(0, counter, true);
  view.setUint16(4, 0x0002, true);
  body[6] = 4;              // POSITION
  view.setInt16(7, -97, true);
  body[9] = 7;              // snr
  body[10] = 12;            // payload length
  return body;
}

describe("connection lifecycle, section 5", () => {
  it("runs the documented sequence in order", async () => {
    const device = new MockDevice({
      queued: [1, 2, 3].map((counter) => ({ counter, body: journalEvent(counter) })),
    });
    const store = new MemoryEventStore();
    const session = new BridgeSession(device, store);

    const pushed: number[] = [];
    const result = await session.run({
      nowUnix: 1788000000,
      push: async (events) => { pushed.push(...events.map((e) => e.counter)); },
    });

    expect(result.info.bridgeProtocol).toBe(2);
    expect(result.eventsFetched).toBe(3);
    expect(pushed).toEqual([1, 2, 3]);

    // Step 2 first, then 3, then the journal. The order is the requirement.
    const opcodes = device.received.map((m) => m.opcode);
    expect(opcodes[0]).toBe(Opcode.GetInfo);
    expect(opcodes[1]).toBe(Opcode.GetStatus);
    expect(opcodes[2]).toBe(Opcode.GetBudget);
    expect(opcodes).toContain(Opcode.GetQueue);

    /*
     * ACK_QUEUE is back, and it is the assertion D15 was waiting for.
     *
     * It went away because the journal counter never left the device, so no
     * client could form a correct upToCounter and acknowledging on a guess freed
     * entries this phone may never have stored. EVT_JOURNAL carries the counter
     * now, so the ordering rule from §5 holds again: GET_QUEUE, then the server,
     * then ACK_QUEUE -- in that order and no other.
     */
    expect(opcodes).toContain(Opcode.AckQueue);
    expect(opcodes.indexOf(Opcode.AckQueue)).toBeGreaterThan(opcodes.indexOf(Opcode.GetQueue));
    expect(result.acknowledgedUpTo).toBe(3);
    expect(device.ackedUpTo).toBe(3);

    await session.close();
  });

  it("acknowledges only after the server has the events", async () => {
    /*
     * The rule from section 3, and the reason for it: "Acknowledging before the
     * server has the data would lose events whenever the phone dies between the
     * two steps, and this is a device you carry into places where the phone
     * dies."
     */
    const device = new MockDevice({
      queued: [10, 11].map((counter) => ({ counter, body: journalEvent(counter) })),
    });
    const store = new MemoryEventStore();
    const session = new BridgeSession(device, store);
    session.open();

    await session.getInfo();
    await expect(
      session.syncJournal(async () => { throw new Error("server unreachable"); }),
    ).rejects.toThrow(/server unreachable/);

    // No ACK_QUEUE went out, so the device still holds everything.
    expect(device.ackedUpTo).toBeNull();
    expect(device.received.map((m) => m.opcode)).not.toContain(Opcode.AckQueue);

    // A later connection fetches them again and this time gets through. The
    // local store already had them, so nothing is duplicated.
    const pushed: number[] = [];
    await session.syncJournal(async (events) => { pushed.push(...events.map((e) => e.counter)); });
    expect(pushed).toEqual([10, 11]);
    expect(store.all.map((e) => e.counter)).toEqual([10, 11]);

    // And now, on the successful push, the acknowledgement goes out -- with a
    // counter that came off the wire in EVT_JOURNAL rather than being guessed.
    expect(device.ackedUpTo).toBe(11);

    await session.close();
  });

  it("stores every event type, not only the received frames", async () => {
    /*
     * D15's other half, and this one is a bug rather than a design question.
     *
     * Until 2026-08-31 this client pushed only frame-rx into its store.
     * EVT_FRAME_TX_RESULT, EVT_STATUS, EVT_BUDGET, EVT_FIX, EVT_CONFIG_APPLIED
     * and EVT_LOG went to listeners and nowhere else -- and then ACK_QUEUE freed
     * them on the device, making the loss permanent.
     *
     * Section 4: "State 2 may be followed later by 0 or 1 for the same counter
     * -- the phone and the server must treat the journal as a log of state
     * transitions, not as a set of final outcomes." Those transitions are what
     * decision D10 solved the server's idempotency key for. They could never
     * arrive.
     */
    const txResult = new Uint8Array([
      0x2a, 0x00, 0x00, 0x00, // counter 42 (the FRAME counter, see D10)
      0x02, 0x00,             // dst
      0x03,                   // seq
      0x02,                   // result 2 = queued, which 0 or 1 must follow
      0x01,                   // attempts
      0x9f, 0xff,             // rssi -97
      0x07,                   // snr
    ]);

    const device = new MockDevice({
      queued: [{ counter: 1, opcode: EventCode.FrameTxResult, body: txResult }],
    });
    const store = new MemoryEventStore();
    const session = new BridgeSession(device, store);
    session.open();

    const pushed: number[] = [];
    await session.syncJournal(async (events) => { pushed.push(...events.map((e) => e.counter)); });

    expect(store.all).toHaveLength(1);
    expect(store.all[0]!.opcode).toBe(EventCode.FrameTxResult);
    expect(pushed).toHaveLength(1);

    await session.close();
  });

  it("re-fetching an already stored event does not duplicate it", async () => {
    // "erring towards re-sending is always the right call" -- which only holds
    // if the client is idempotent too.
    const device = new MockDevice({
      queued: [{ counter: 5, body: journalEvent(5) }],
    });
    const store = new MemoryEventStore();
    const session = new BridgeSession(device, store);
    session.open();

    await session.syncJournal(async () => {});
    await store.markPushed(0); // pretend the push record was lost
    await session.syncJournal(async () => {});

    expect(store.all).toHaveLength(1);
    await session.close();
  });

  it("sets the time when the node reports it has none", async () => {
    // CLAUDE.md 1.2: a node with no valid time is transmit-blocked, and SET_TIME
    // is one of the two ways out of that state.
    const device = new MockDevice({ timeValid: false });
    const session = new BridgeSession(device, new MemoryEventStore());

    const result = await session.run({
      nowUnix: 1788000000,
      deviceReportsNoTime: true,
      push: async () => {},
    });

    expect(result.setTime).toBe(true);
    expect(device.received.map((m) => m.opcode)).toContain(Opcode.SetTime);
    expect(device.timeValid).toBe(true);
    await session.close();
  });

  it("refuses a bonded command on an unbonded connection", async () => {
    // Section 2: enforced on the device. This checks the client surfaces it as
    // an error rather than as silence.
    const device = new MockDevice({ bonded: false });
    const session = new BridgeSession(device, new MemoryEventStore());
    session.open();

    // The open tier still works -- section 6.4 of the test plan.
    await expect(session.getInfo()).resolves.toMatchObject({ bridgeProtocol: 2 });
    await expect(session.getBudget()).resolves.toBeInstanceOf(Uint8Array);

    // The bonded tier does not -- section 6.3.
    await expect(session.sendText(2, "hello")).rejects.toBeInstanceOf(BridgeProtocolError);
    await expect(session.sendText(2, "hello")).rejects.toThrow(/ERR_NOT_AUTHORISED/);

    await session.close();
  });

  it("reports a protocol version mismatch as such, not as a dead node", async () => {
    /*
     * versioning-and-updates.md 2: a major bump means the client refuses to talk
     * and tells the user which side is behind. It learns this from GET_INFO,
     * which is step 2 of the sequence precisely so the check happens before
     * anything is written.
     */
    const newerDevice = new MockDevice({ bridgeProtocol: 99 });
    const newerSession = new BridgeSession(newerDevice, new MemoryEventStore());
    newerSession.open();
    await expect(newerSession.getInfo()).rejects.toBeInstanceOf(VersionMismatchError);
    await expect(newerSession.getInfo()).rejects.toThrow(/The app is behind/);
    await newerSession.close();

    const olderDevice = new MockDevice({ bridgeProtocol: 0 });
    const olderSession = new BridgeSession(olderDevice, new MemoryEventStore());
    olderSession.open();
    await expect(olderSession.getInfo()).rejects.toThrow(/The node is behind/);

    // And nothing was written past GET_INFO -- the whole point of checking first.
    expect(olderDevice.received.map((m) => m.opcode)).toEqual([Opcode.GetInfo]);
    await olderSession.close();
  });

  it("survives a dropped chunk on the way to the device", async () => {
    // Section 1.1: the receiver drops the partial message and the sender
    // re-sends the whole thing. The client must not hang waiting for a response
    // to a message that never arrived intact.
    const device = new MockDevice({ mtu: 23, dropWrittenChunk: 2 });
    const session = new BridgeSession(device, new MemoryEventStore());
    session.open();

    const longText = "A".repeat(48);
    const attempt = session.sendText(2, longText);
    const timeout = new Promise((resolve) => setTimeout(() => resolve("no response"), 50));
    expect(await Promise.race([attempt.then(() => "responded"), timeout])).toBe("no response");

    // The device saw nothing complete, which is the correct outcome.
    expect(device.received).toHaveLength(0);
    await session.close();
    await expect(attempt).rejects.toThrow(/disconnected/);
  });
});

describe("event journal store", () => {
  it("keeps events in counter order regardless of arrival order", async () => {
    const store = new MemoryEventStore();
    await store.append([{ counter: 30, opcode: 0x81, body: new Uint8Array(0) }]);
    await store.append([{ counter: 10, opcode: 0x81, body: new Uint8Array(0) }]);
    await store.append([{ counter: 20, opcode: 0x81, body: new Uint8Array(0) }]);

    expect(store.all.map((e) => e.counter)).toEqual([10, 20, 30]);
    expect(await store.highWaterMark()).toBe(30);
  });

  it("does not store the same counter twice", async () => {
    const store = new MemoryEventStore();
    const event = { counter: 7, opcode: 0x81, body: new Uint8Array(0) };
    await store.append([event]);
    await store.append([event]);
    expect(store.all).toHaveLength(1);
  });
});

describe("SET_CONFIG", () => {
  it("puts the version in front of the TLV blob, little endian", async () => {
    // docs/bridge-protocol.md section 3: `configVersion:u32, tlv[]`. The version
    // is what config_versions.applied_at is later matched against, so getting
    // its byte order wrong would leave every push looking unacknowledged
    // forever.
    const device = new MockDevice({});
    const session = new BridgeSession(device, new MemoryEventStore());
    session.open();

    const tlvs = Uint8Array.from([0x02, 0x02, 0xd0, 0x07]); // sniffIntervalMs = 2000
    await session.setConfig(0x01020304, tlvs);

    const sent = device.received.find((message) => message.opcode === Opcode.SetConfig);
    expect(sent).toBeDefined();
    expect([...sent!.body]).toEqual([0x04, 0x03, 0x02, 0x01, 0x02, 0x02, 0xd0, 0x07]);
  });

  it("carries an empty blob without inventing one", async () => {
    // A config that changed nothing is a real thing to send -- it is how a
    // client re-asks for an acknowledgement. It must not become a four-byte
    // message with a phantom TLV on the end.
    const device = new MockDevice({});
    const session = new BridgeSession(device, new MemoryEventStore());
    session.open();

    await session.setConfig(7, new Uint8Array(0));
    const sent = device.received.find((message) => message.opcode === Opcode.SetConfig);
    expect(sent!.body).toHaveLength(4);
  });
});
