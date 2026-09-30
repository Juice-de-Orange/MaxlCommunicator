/*
 * The projection is a pure function, and these tests treat it as one -- no
 * database, no HTTP. Gate 7.2 says "projections correct regardless of arrival
 * order"; the honest way to test that is to feed the same events in many
 * different orders and require the results to be identical, rather than to feed
 * them backwards once and call it covered.
 */

import { describe, expect, it } from "vitest";

import { project, type JournalEvent } from "../src/lib/projections";
import { frameRx, frameTxResult, realisticRun, status, type SynthEvent } from "./synth";
import { encodeText, FrameType } from "@protocol/payloads";

const toJournal = (events: SynthEvent[]): JournalEvent[] => events.map((event) => ({ ...event }));

/** Deterministic shuffle, so a failure is reproducible from the seed alone. */
function shuffled<T>(items: readonly T[], seed: number): T[] {
  const out = [...items];
  let state = seed >>> 0;
  for (let i = out.length - 1; i > 0; i -= 1) {
    state = (state * 1664525 + 1013904223) >>> 0;
    const j = state % (i + 1);
    [out[i], out[j]] = [out[j]!, out[i]!];
  }
  return out;
}

describe("projection over a realistic run", () => {
  const projection = project(toJournal(realisticRun()));

  it("takes the device's own position from EVT_FIX, not from what it heard", () => {
    // A POSITION frame received from a peer describes the peer. Attributing it
    // to the receiver would put both nodes on top of each other on the map.
    expect(projection.state.latDeg7).toBe(472_692_000);
    expect(projection.state.lonDeg7).toBe(114_041_000);
    expect(projection.peerObservations.some((o) => o.latDeg7 === 472_712_345)).toBe(true);
  });

  it("keeps the peer's telemetry as an observation of the peer", () => {
    const telemetry = projection.peerObservations.find((o) => o.tempC100 !== null);
    expect(telemetry).toBeDefined();
    expect(telemetry!.peerNodeId).toBe(0x0002);
    expect(telemetry!.tempC100).toBe(1842);
    expect(telemetry!.pressurePa).toBe(95_320);
    // ... and does not smear it onto the reporting device.
    expect(projection.state.tempC100).toBeNull();
  });

  it("folds queued and delivered for one frame into a single message", () => {
    // Two journal entries, one frame, one row. This is decision D10 working.
    const outgoing = projection.messages.filter((m) => m.direction === "tx");
    expect(outgoing).toHaveLength(2);

    const delivered = outgoing.find((m) => m.frameCounter === 501);
    expect(delivered?.state).toBe("delivered");
    expect(delivered?.attempts).toBe(1);
    expect(delivered?.rssi).toBe(-90);

    const gaveUp = outgoing.find((m) => m.frameCounter === 502);
    expect(gaveUp?.state).toBe("undelivered");
    expect(gaveUp?.attempts).toBe(3);
  });

  it("keeps the moment the message first appeared, not the moment it was delivered", () => {
    // CLAUDE.md 2.4 wants "queued until HH:MM" to be showable, which needs the
    // time the message entered the queue and not just its final transition.
    const delivered = projection.messages.find((m) => m.frameCounter === 501);
    expect(delivered!.occurredAt.getTime()).toBeLessThan(delivered!.stateChangedAt.getTime());
  });

  it("decodes an inbound text", () => {
    const inbound = projection.messages.find((m) => m.direction === "rx");
    expect(inbound?.text).toBe("at the summit");
    expect(inbound?.state).toBe("received");
    expect(inbound?.rssi).toBe(-88);
  });

  it("reports the budget, and the later report wins", () => {
    // EVT_BUDGET at journal 7 said 184000; the status body at journal 11 said
    // 190000. Later in the log, so it stands.
    expect(projection.state.budgetUsedMs).toBe(190_000);
    expect(projection.state.budgetLimitMs).toBe(360_000);
    expect(projection.state.budgetBand).toBe(0);
  });

  it("carries the flags the UI needs to explain a silent node", () => {
    expect(projection.state.timeValid).toBe(true);
    expect(projection.state.keyProvisioned).toBe(true);
    expect(projection.state.queueDepth).toBe(1);
  });

  it("records one link stat per received frame", () => {
    expect(projection.linkStats).toHaveLength(3);
    expect(projection.linkStats.map((s) => s.frameCounter)).toEqual([8801, 8802, 8803]);
  });

  it("surfaces the config acknowledgement with what the node did not apply", () => {
    expect(projection.configApplications).toHaveLength(1);
    const applied = projection.configApplications[0]!;
    expect(applied.configVersion).toBe(7);
    expect([...applied.unappliedTypes]).toEqual([0x09]);
  });
});

describe("gate 7.2 -- order independence", () => {
  const events = toJournal(realisticRun());
  const reference = project(events);

  it("gives the same projection for 50 different arrival orders", () => {
    for (let seed = 1; seed <= 50; seed += 1) {
      expect(project(shuffled(events, seed))).toEqual(reference);
    }
  });

  it("gives the same projection when the log arrives exactly backwards", () => {
    expect(project([...events].reverse())).toEqual(reference);
  });

  it("does not let a late-arriving 'queued' overwrite a delivered message", () => {
    // The failure this guards against is specific: the phone re-sends its
    // journal after a reconnect, the queued entry arrives after the delivered
    // one, and a projector that folds in arrival order marks a message that has
    // long since been received as still waiting for the duty cycle.
    const queued = frameTxResult({
      journal: 6, counter: 501, dst: 2, seq: 11, result: 2, attempts: 0,
      rssi: 0, snr: 0, at: new Date("2026-08-31T06:02:30Z"),
    });
    const delivered = frameTxResult({
      journal: 8, counter: 501, dst: 2, seq: 11, result: 0, attempts: 1,
      rssi: -90, snr: 6, at: new Date("2026-08-31T06:03:35Z"),
    });

    expect(project(toJournal([delivered, queued])).messages[0]!.state).toBe("delivered");
    expect(project(toJournal([queued, delivered])).messages[0]!.state).toBe("delivered");
  });
});

describe("projection robustness", () => {
  it("skips a body it cannot parse instead of losing the rest of the log", () => {
    // The log is the record; a projection is a convenience. One malformed body
    // must not take the other events with it.
    const broken: JournalEvent = {
      journalCounter: 4,
      direction: "rx",
      opcode: 0x81,
      body: new Uint8Array([0x01, 0x02]), // far too short for EVT_FRAME_RX
      receivedAt: new Date("2026-08-31T06:01:00Z"),
    };
    const good = toJournal([
      status({ journal: 1, batteryMv: 4000, uptimeS: 60, at: new Date("2026-08-31T06:00:00Z") }),
      frameRx({
        journal: 5, counter: 900, src: 2, type: FrameType.Text, rssi: -80, snr: 8,
        payload: encodeText("still here"), at: new Date("2026-08-31T06:02:00Z"),
      }),
    ]);

    const projection = project([...good, broken]);
    expect(projection.state.batteryMv).toBe(4000);
    expect(projection.messages[0]!.text).toBe("still here");
  });

  it("ignores an event opcode it has never heard of", () => {
    // docs/versioning-and-updates.md: a client that sees an unknown event
    // opcode discards it and continues. The server is a client here too.
    const future: JournalEvent = {
      journalCounter: 2,
      direction: "rx",
      opcode: 0x8f,
      body: new Uint8Array([1, 2, 3, 4]),
      receivedAt: new Date("2026-08-31T06:00:30Z"),
    };
    const projection = project([future]);
    expect(projection.messages).toHaveLength(0);
    expect(projection.state.highestCounter).toBe(2);
  });
});
