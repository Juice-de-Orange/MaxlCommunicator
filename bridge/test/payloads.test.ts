/**
 * Radio payload encodings against the shared vectors.
 *
 * The firmware checks the same file in `firmware/test/unit/test_payloads.cpp`.
 * Each side builds its own object from the field values documented in the JSON;
 * if either has misread a width or an endianness, its encoding will not match.
 */

import { describe, expect, it } from "vitest";

import {
  decodeAck,
  decodePosition,
  decodeTelemetry,
  encodeAck,
  encodePosition,
  encodeTelemetry,
  encodeText,
  positionToDegrees,
  MAX_TEXT_BYTES,
} from "../src/protocol/payloads.js";
import { bytesToHex, vector } from "./vectors.js";

describe("POSITION", () => {
  it("matches the shared vectors", () => {
    expect(bytesToHex(encodePosition({
      latitudeE7: 472692000, longitudeE7: 114041000, altitudeM: 574, hdop: 12, fixAgeS: 3,
    }))).toBe(bytesToHex(vector("position_innsbruck")));

    // Southern and western hemisphere plus altitude below sea level: three
    // separate sign conversions in one frame.
    expect(bytesToHex(encodePosition({
      latitudeE7: -338688000, longitudeE7: -1732090000, altitudeM: -15, hdop: 31, fixAgeS: 250,
    }))).toBe(bytesToHex(vector("position_southern_negative")));

    expect(bytesToHex(encodePosition({
      latitudeE7: 0, longitudeE7: 0, altitudeM: 0, hdop: 0, fixAgeS: 0,
    }))).toBe(bytesToHex(vector("position_zero")));
  });

  it("decodes the vectors back to their documented fields", () => {
    const position = decodePosition(vector("position_southern_negative"));
    expect(position.latitudeE7).toBe(-338688000);
    expect(position.longitudeE7).toBe(-1732090000);
    expect(position.altitudeM).toBe(-15);
    expect(position.hdop).toBe(31);
    expect(position.fixAgeS).toBe(250);
  });

  it("converts to degrees only for display", () => {
    const { lat, lon } = positionToDegrees(decodePosition(vector("position_innsbruck")));
    expect(lat).toBeCloseTo(47.2692, 4);
    expect(lon).toBeCloseTo(11.4041, 4);
  });

  it("rejects a payload of the wrong length", () => {
    expect(() => decodePosition(new Uint8Array(11))).toThrow();
    expect(() => decodePosition(new Uint8Array(13))).toThrow();
  });
});

describe("TELEMETRY", () => {
  it("matches the shared vectors", () => {
    expect(bytesToHex(encodeTelemetry({
      temperatureCentiC: 2135, humidityCentiPercent: 4820,
      pressurePa: 95230, batteryMv: 3987, uptimeS: 86400,
    }))).toBe(bytesToHex(vector("telemetry_typical")));

    // -12.75 C is the only signed field; uptime at u32 max is the widest.
    expect(bytesToHex(encodeTelemetry({
      temperatureCentiC: -1275, humidityCentiPercent: 9155,
      pressurePa: 101325, batteryMv: 3312, uptimeS: 4294967295,
    }))).toBe(bytesToHex(vector("telemetry_freezing")));
  });

  it("round-trips the widest values", () => {
    const decoded = decodeTelemetry(vector("telemetry_freezing"));
    expect(decoded.temperatureCentiC).toBe(-1275);
    expect(decoded.uptimeS).toBe(4294967295);
  });
});

describe("ACK", () => {
  it("matches the shared vectors", () => {
    expect(bytesToHex(encodeAck({ seq: 42, rssiDbm: -97, snrDb: 7 })))
      .toBe(bytesToHex(vector("ack_typical")));
    expect(bytesToHex(encodeAck({ seq: 255, rssiDbm: -128, snrDb: -13 })))
      .toBe(bytesToHex(vector("ack_weak_link")));
  });

  it("decodes the negative SNR that drives SF escalation", () => {
    const ack = decodeAck(vector("ack_weak_link"));
    expect(ack.snrDb).toBe(-13);
    expect(ack.rssiDbm).toBe(-128);
  });
});

describe("TEXT", () => {
  it("is limited by bytes, not characters", () => {
    // 14 characters, 16 bytes. A client that truncated by character would
    // overrun the frame on the first umlaut.
    const umlaut = vector("text_utf8_umlaut");
    expect(umlaut.length).toBe(16);
    expect(bytesToHex(encodeText("Grüße vom Berg"))).toBe(bytesToHex(umlaut));

    expect(vector("text_max_48").length).toBe(MAX_TEXT_BYTES);
    expect(() => encodeText("A".repeat(MAX_TEXT_BYTES + 1))).toThrow(/49 UTF-8 bytes/);

    // 24 two-byte characters is 48 bytes: at the limit, not over it.
    expect(encodeText("ü".repeat(24)).length).toBe(48);
    expect(() => encodeText("ü".repeat(25))).toThrow();
  });
});
