/**
 * Chunking, the message layer and TLVs, against the shared vectors.
 *
 * The firmware checks the same file in
 * `firmware/test/unit/test_bridge_codec.cpp`.
 */

import { describe, expect, it } from "vitest";

import {
  CHUNK_FIRST,
  CHUNK_LAST,
  CHUNK_TIMEOUT_MS,
  MAX_MESSAGE_BYTES,
  MSG_ID_MASK,
  MsgIdCounter,
  Reassembler,
  chunk,
  maxFragmentBytes,
} from "../src/protocol/chunker.js";
import {
  Reader,
  decodeEvent,
  decodeJournalEntry,
  decodeMessage,
  decodeTlvs,
  encodeJournalEntry,
  encodeMessage,
  encodeSetConfig,
  encodeTlvs,
} from "../src/protocol/codec.js";
import { BridgeError, ConfigTlv, EventCode, Opcode, TxResult, tierOf } from "../src/protocol/opcodes.js";
import { bytesToHex, vector } from "./vectors.js";

describe("chunking, section 1.1", () => {
  it("produces the single-chunk form the vectors document", () => {
    const chunks = chunk(vector("chunk_single_message"), 247, 5);
    expect(chunks).toHaveLength(1);
    expect(bytesToHex(chunks[0]!)).toBe(bytesToHex(vector("chunk_single_c0")));

    // 0xC5 = F | L | msgId 5.
    expect(chunks[0]![0]! & CHUNK_FIRST).toBeTruthy();
    expect(chunks[0]![0]! & CHUNK_LAST).toBeTruthy();
    expect(chunks[0]![0]! & MSG_ID_MASK).toBe(5);
    expect(chunks[0]![1]).toBe(0);
  });

  it("splits at the minimum MTU exactly as the vectors document", () => {
    // "Assume 23 until negotiation completes."
    expect(maxFragmentBytes(23)).toBe(18);

    const chunks = chunk(vector("chunk_three_message"), 23, 9);
    expect(chunks).toHaveLength(3);
    expect(bytesToHex(chunks[0]!)).toBe(bytesToHex(vector("chunk_three_c0")));
    expect(bytesToHex(chunks[1]!)).toBe(bytesToHex(vector("chunk_three_c1")));
    expect(bytesToHex(chunks[2]!)).toBe(bytesToHex(vector("chunk_three_c2")));

    // Only the first carries F, only the last carries L.
    expect(chunks[1]![0]! & (CHUNK_FIRST | CHUNK_LAST)).toBe(0);
  });

  it("reassembles the documented chunks back into the message", () => {
    const assembler = new Reassembler();
    expect(assembler.feed(vector("chunk_three_c0"), 0).kind).toBe("need-more");
    expect(assembler.feed(vector("chunk_three_c1"), 10).kind).toBe("need-more");

    const outcome = assembler.feed(vector("chunk_three_c2"), 20);
    expect(outcome.kind).toBe("complete");
    if (outcome.kind !== "complete") return;
    expect(bytesToHex(outcome.message)).toBe(bytesToHex(vector("chunk_three_message")));
    expect(outcome.msgId).toBe(9);
  });

  it("round-trips any message at any MTU", () => {
    const message = Uint8Array.from({ length: 1500 }, (_, i) => (i * 7 + 3) & 0xff);
    for (let mtu = 23; mtu <= 247; mtu += 7) {
      const assembler = new Reassembler();
      let final: ReturnType<Reassembler["feed"]> = { kind: "need-more" };
      let now = 0;
      for (const part of chunk(message, mtu, 17)) {
        final = assembler.feed(part, (now += 10));
      }
      expect(final.kind, `mtu ${mtu}`).toBe("complete");
      if (final.kind === "complete") {
        expect(bytesToHex(final.message)).toBe(bytesToHex(message));
      }
    }
  });

  it("discards on a dropped middle chunk rather than half-applying", () => {
    const assembler = new Reassembler();
    expect(assembler.feed(vector("chunk_three_c0"), 0).kind).toBe("need-more");
    const outcome = assembler.feed(vector("chunk_three_c2"), 10);
    expect(outcome.kind).toBe("discarded");
    if (outcome.kind === "discarded") {
      expect(outcome.reason).toMatch(/out of order/);
    }
    // Nothing partial is left for the next message to inherit.
    expect(assembler.feed(vector("chunk_single_c0"), 20).kind).toBe("complete");
  });

  it("discards when msgId changes mid-message", () => {
    const interloper = new Uint8Array(vector("chunk_three_c1"));
    interloper[0] = (interloper[0]! & ~MSG_ID_MASK) | 11;

    const assembler = new Reassembler();
    assembler.feed(vector("chunk_three_c0"), 0);
    const outcome = assembler.feed(interloper, 10);
    expect(outcome.kind).toBe("discarded");
    if (outcome.kind === "discarded") {
      expect(outcome.reason).toMatch(/msgId/);
    }
  });

  it("times out a partial message after five seconds", () => {
    const assembler = new Reassembler();
    assembler.feed(vector("chunk_three_c0"), 1000);
    expect(assembler.tick(1000 + CHUNK_TIMEOUT_MS - 1)).toBe(false);
    expect(assembler.tick(1000 + CHUNK_TIMEOUT_MS)).toBe(true);
    expect(assembler.inProgress).toBe(false);
  });

  it("refuses a message over 4096 bytes", () => {
    expect(() => chunk(new Uint8Array(MAX_MESSAGE_BYTES + 1), 247, 0)).toThrow(/4096/);
    expect(() => chunk(new Uint8Array(MAX_MESSAGE_BYTES), 247, 0)).not.toThrow();
  });

  it("wraps msgId within its six bits", () => {
    const counter = new MsgIdCounter();
    const seen = Array.from({ length: 65 }, () => counter.take());
    expect(seen[0]).toBe(0);
    expect(seen[63]).toBe(63);
    expect(seen[64]).toBe(0);
    expect(Math.max(...seen)).toBeLessThanOrEqual(MSG_ID_MASK);
  });
});

describe("message layer, section 1.2", () => {
  it("matches the shared RSP_ERR vector", () => {
    const encoded = encodeMessage(
      EventCode.ResponseError, 0x11,
      Uint8Array.from([Opcode.ProvisionKey, BridgeError.NotAuthorised]),
    );
    expect(bytesToHex(encoded)).toBe(bytesToHex(vector("rsp_err_not_authorised")));

    const decoded = decodeEvent(decodeMessage(vector("rsp_err_not_authorised")));
    expect(decoded.kind).toBe("response-error");
    if (decoded.kind !== "response-error") return;
    expect(decoded.failedOpcode).toBe(Opcode.ProvisionKey);
    expect(decoded.error).toBe(BridgeError.NotAuthorised);
  });

  it("decodes EVT_BUDGET into what the UI needs", () => {
    const decoded = decodeEvent(decodeMessage(vector("evt_budget")));
    expect(decoded.kind).toBe("budget");
    if (decoded.kind !== "budget") return;
    expect(decoded.band).toBe(0);
    expect(decoded.usedMs).toBe(42500);
    // The g3 allowance from CLAUDE.md 1.3: 10 % of an hour.
    expect(decoded.limitMs).toBe(360000);
    expect(decoded.nextTxUnix).toBe(1788003600);
  });

  it("decodes EVT_FRAME_TX_RESULT as a state transition, not a verdict", () => {
    const decoded = decodeEvent(decodeMessage(vector("evt_frame_tx_result")));
    expect(decoded.kind).toBe("frame-tx-result");
    if (decoded.kind !== "frame-tx-result") return;
    expect(decoded.counter).toBe(1024);
    expect(decoded.result).toBe(TxResult.Undelivered);
    expect(decoded.attempts).toBe(4);
    expect(decoded.rssi).toBe(-97);
    expect(decoded.snr).toBe(7);
  });

  it("carries txnId 0 on every event", () => {
    for (const name of ["evt_budget", "evt_frame_tx_result"]) {
      expect(decodeMessage(vector(name)).txnId, name).toBe(0);
    }
  });

  it("degrades on an unknown event rather than throwing", () => {
    // versioning-and-updates.md 2: "A client that sees an unknown event opcode
    // discards it and continues; it does not disconnect."
    const decoded = decodeEvent(decodeMessage(Uint8Array.from([0x9f, 0x00, 1, 2, 3])));
    expect(decoded.kind).toBe("unknown");
    if (decoded.kind !== "unknown") return;
    expect(decoded.opcode).toBe(0x9f);
    expect(Array.from(decoded.body)).toEqual([1, 2, 3]);
  });
});

describe("config TLVs, section 3", () => {
  it("matches the shared SET_CONFIG vector", () => {
    const encoded = encodeSetConfig(7, [
      { type: ConfigTlv.SniffIntervalMs, value: Uint8Array.from([0xd0, 0x07]) },
      { type: ConfigTlv.Band, value: Uint8Array.from([0x00]) },
      { type: ConfigTlv.TxPowerDbm, value: Uint8Array.from([22]) },
      { type: ConfigTlv.TelemetryIntervalS, value: Uint8Array.from([0x58, 0x02]) },
      { type: 0xf0, value: Uint8Array.from([0xbe, 0xef]) },
    ]);
    expect(bytesToHex(encoded)).toBe(bytesToHex(vector("set_config_body")));
  });

  it("returns unknown types instead of rejecting the write", () => {
    const tlvs = decodeTlvs(vector("set_config_tlv_run"));
    expect(tlvs.map((t) => t.type)).toEqual([
      ConfigTlv.SniffIntervalMs, ConfigTlv.Band, ConfigTlv.TxPowerDbm,
      ConfigTlv.TelemetryIntervalS, 0xf0,
    ]);
    // The unknown one is reported back as unapplied, never dropped silently.
    expect(Array.from(tlvs[4]!.value)).toEqual([0xbe, 0xef]);
  });

  it("keeps configVersion out of the TLV run", () => {
    const body = vector("set_config_body");
    const run = vector("set_config_tlv_run");
    expect(body.length).toBe(run.length + 4);
    expect(new Reader(body).u32()).toBe(7);
    expect(bytesToHex(body.subarray(4))).toBe(bytesToHex(run));
  });

  it("refuses a truncated run rather than half-reading it", () => {
    expect(() => decodeTlvs(Uint8Array.from([0x02, 0x04, 0xd0, 0x07]))).toThrow(/claims 4 bytes/);
  });

  it("round-trips", () => {
    const tlvs = decodeTlvs(vector("set_config_tlv_run"));
    expect(bytesToHex(encodeTlvs(tlvs))).toBe(bytesToHex(vector("set_config_tlv_run")));
  });
});

describe("auth tiers, section 2", () => {
  it("opens exactly three opcodes", () => {
    expect(tierOf(Opcode.GetInfo)).toBe("open");
    expect(tierOf(Opcode.GetStatus)).toBe("open");
    expect(tierOf(Opcode.GetBudget)).toBe("open");

    // The one that matters: an unbonded write must not set the network key.
    expect(tierOf(Opcode.ProvisionKey)).toBe("bonded");

    let open = 0;
    for (let opcode = 0; opcode <= 0xff; opcode += 1) {
      if (tierOf(opcode) === "open") open += 1;
    }
    expect(open).toBe(3);
  });
});

describe("EVT_JOURNAL, section 4 -- decision D15", () => {
  it("unwraps an entry and keeps the two counters apart", () => {
    const message = decodeMessage(vector("evt_journal_wrapping_tx_result"));
    expect(message.opcode).toBe(EventCode.Journal);
    // Unsolicited, even though GET_QUEUE asked for it: §1.2 gives txnId 0 to
    // every event, and a response carrying 0 would be unroutable.
    expect(message.txnId).toBe(0);

    const entry = decodeJournalEntry(message);
    expect(entry.counter).toBe(4098);
    expect(entry.opcode).toBe(EventCode.FrameTxResult);

    // The wrapped body is the other vector, byte for byte -- the wrapper does
    // not re-encode anything.
    const bare = decodeMessage(vector("evt_frame_tx_result"));
    expect(bytesToHex(entry.body)).toBe(bytesToHex(bare.body));

    /*
     * The point of D10, made checkable: the entry counter is 4098, the frame
     * counter inside the body is 1024. A client that read the body's counter as
     * the journal counter would acknowledge 1024 and free entries the phone
     * never stored -- and it would pass every other vector in the file.
     */
    const event = decodeEvent(bare);
    expect(event.kind).toBe("frame-tx-result");
    if (event.kind === "frame-tx-result") {
      expect(event.counter).toBe(1024);
      expect(event.counter).not.toBe(entry.counter);
    }
  });

  it("keeps an entry whose wrapped opcode it does not know", () => {
    /*
     * §4: an unknown wrapped opcode is stored and counted towards ACK_QUEUE
     * anyway. The counter is legible even when the body is not, and dropping the
     * entry would free journal space for something nobody ever saw. This is what
     * makes a future event type safe to add.
     */
    const entry = decodeJournalEntry(decodeMessage(vector("evt_journal_unknown_opcode")));
    expect(entry.counter).toBe(4099);
    expect(entry.opcode).toBe(0x8f);
    expect(bytesToHex(entry.body)).toBe("aabbcc");
  });

  it("round-trips", () => {
    const entry = decodeJournalEntry(decodeMessage(vector("evt_journal_wrapping_tx_result")));
    const rebuilt = encodeMessage(EventCode.Journal, 0, encodeJournalEntry(entry));
    expect(bytesToHex(rebuilt)).toBe(bytesToHex(vector("evt_journal_wrapping_tx_result")));
  });

  it("refuses a body that will not fit the len:u8 field", () => {
    expect(() =>
      encodeJournalEntry({ counter: 1, opcode: EventCode.FrameRx, body: new Uint8Array(256) }),
    ).toThrow(/256 bytes/);
  });
});
