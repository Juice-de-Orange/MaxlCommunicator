/**
 * What can be checked about the Web Bluetooth transport without a browser.
 *
 * Not much, and that is by design: everything above the transport is tested
 * against `MockTransport`, so the untested surface here is the handful of lines
 * that call `navigator.bluetooth`. What IS worth checking is the support
 * detection, because `CLAUDE.md` §4.2 requires the UI to say plainly that iOS is
 * unsupported "rather than failing silently" -- and a wrong answer there is a
 * blank screen with no explanation.
 */

import { describe, expect, it } from "vitest";

import { ASSUMED_MTU, SERVICE_UUID, isSupported } from "../src/transport/webble.js";
import { MAX_MTU } from "../src/protocol/chunker.js";

describe("Web Bluetooth support detection", () => {
  it("reports unsupported under Node, with a reason", () => {
    // Node 22 has a minimal `navigator` with no `bluetooth`, so this exercises
    // the real branch rather than a stub.
    const support = isSupported();
    expect(support.supported).toBe(false);
    expect(support.reason).toBeTruthy();
  });

  it("names iOS specifically when it is the cause", () => {
    const original = Object.getOwnPropertyDescriptor(globalThis, "navigator");
    try {
      Object.defineProperty(globalThis, "navigator", {
        value: { userAgent: "Mozilla/5.0 (iPhone; CPU iPhone OS 18_2 like Mac OS X)" },
        configurable: true,
      });
      const support = isSupported();
      expect(support.supported).toBe(false);
      // Apple has declined to implement Web Bluetooth; the UI must say so.
      expect(support.reason).toMatch(/Apple/);
      expect(support.reason).toMatch(/Android/);
    } finally {
      if (original) Object.defineProperty(globalThis, "navigator", original);
    }
  });
});

describe("transport constants", () => {
  it("assumes an MTU the chunker considers legal", () => {
    expect(ASSUMED_MTU).toBeLessThanOrEqual(MAX_MTU);
  });

  it("uses a 128-bit service UUID", () => {
    // CLAUDE.md 4.1: "Custom 128-bit service UUID."
    expect(SERVICE_UUID).toMatch(/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/);
  });
});
