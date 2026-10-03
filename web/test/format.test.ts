/*
 * Small presentation and parsing helpers whose mistakes are quiet: a time
 * without a zone reads as local time, and "1abc" read with parseInt is node 1.
 */

import { describe, expect, it } from "vitest";

import { absoluteTime } from "../src/lib/format";
import { isNodeId, parseNodeId } from "../src/lib/node-id";

describe("absoluteTime", () => {
  it("names the time zone it was rendered in", () => {
    const zone = new Intl.DateTimeFormat("en-GB", { timeZoneName: "short" })
      .formatToParts(new Date("2026-08-31T14:05:00Z"))
      .find((part) => part.type === "timeZoneName")!.value;

    expect(absoluteTime(new Date("2026-08-31T14:05:00Z")).endsWith(` ${zone}`)).toBe(true);
  });

  it("keeps the dash for no time at all", () => {
    expect(absoluteTime(null)).toBe("—");
  });
});

describe("node ids", () => {
  it("are the u16 of the frame header", () => {
    expect(isNodeId(0)).toBe(true);
    expect(isNodeId(0xffff)).toBe(true);
    expect(isNodeId(0x10000)).toBe(false);
    expect(isNodeId(-1)).toBe(false);
    expect(isNodeId(1.5)).toBe(false);
    expect(isNodeId("1")).toBe(false);
  });

  it("are parsed from digits only", () => {
    expect(parseNodeId("7")).toBe(7);
    expect(parseNodeId("65535")).toBe(65535);
    expect(parseNodeId("65536")).toBeNull();
    expect(parseNodeId("abc")).toBeNull();
    expect(parseNodeId("1abc")).toBeNull();
    expect(parseNodeId("1.5")).toBeNull();
    expect(parseNodeId("")).toBeNull();
    expect(parseNodeId(undefined)).toBeNull();
  });
});
