/*
 * The status body (docs/bridge-protocol.md section 4).
 *
 * This layout did not exist until the dashboard needed it -- revision 1 defined
 * EVT_STATUS as "see GET_STATUS body" and never gave that body. These tests hold
 * the byte offsets, not just the round trip: a round trip passes just as happily
 * against a layout that has drifted from the document, and the document is what
 * the Kotlin client of phase 9 will be written from.
 */

import { describe, expect, it } from "vitest";

import {
  decodeStatus,
  encodeStatus,
  STATUS_BYTES,
  StatusFlag,
  statusHasFlag,
  type Status,
} from "../src/protocol/payloads";

const sample: Status = {
  batteryMv: 4021,
  uptimeS: 93_784,
  queueDepth: 3,
  band: 0,
  // Half of the g3 hourly allowance, which is 360 s.
  budgetUsedMs: 180_000,
  budgetLimitMs: 360_000,
  flags: StatusFlag.TimeValid | StatusFlag.KeyProvisioned,
};

describe("status body", () => {
  it("is exactly 17 bytes", () => {
    expect(encodeStatus(sample)).toHaveLength(STATUS_BYTES);
  });

  it("round-trips", () => {
    expect(decodeStatus(encodeStatus(sample))).toEqual(sample);
  });

  it("puts every field at the offset the document gives", () => {
    const bytes = encodeStatus(sample);
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    expect(view.getUint16(0, true)).toBe(4021); // batteryMv
    expect(view.getUint32(2, true)).toBe(93_784); // uptimeS
    expect(view.getUint8(6)).toBe(3); // queueDepth
    expect(view.getUint8(7)).toBe(0); // band, 0 = g3
    expect(view.getUint32(8, true)).toBe(180_000); // budgetUsedMs
    expect(view.getUint32(12, true)).toBe(360_000); // budgetLimitMs
    expect(view.getUint8(16)).toBe(0b011); // flags
  });

  it("rejects a body of the wrong length rather than reading past it", () => {
    expect(() => decodeStatus(new Uint8Array(STATUS_BYTES - 1))).toThrow(RangeError);
    expect(() => decodeStatus(new Uint8Array(STATUS_BYTES + 1))).toThrow(RangeError);
  });

  it("reports timeValid, which is what separates quiet from transmit-blocked", () => {
    expect(statusHasFlag(sample, StatusFlag.TimeValid)).toBe(true);
    expect(statusHasFlag(sample, StatusFlag.GnssPowered)).toBe(false);

    const noTime = decodeStatus(encodeStatus({ ...sample, flags: 0 }));
    expect(statusHasFlag(noTime, StatusFlag.TimeValid)).toBe(false);
  });

  it("carries the g1 allowance too, because the band is a field and not an assumption", () => {
    const g1 = decodeStatus(
      encodeStatus({ ...sample, band: 1, budgetUsedMs: 12_000, budgetLimitMs: 36_000 }),
    );
    expect(g1.band).toBe(1);
    expect(g1.budgetLimitMs).toBe(36_000);
  });
});
