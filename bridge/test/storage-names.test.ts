/*
 * The PWA's storage names outlived the project's name on purpose.
 *
 * Until the rename to trailtalk the app stored its pairing under
 * "maxl-bridge-settings" and its unpushed events in the IndexedDB database
 * "maxl-bridge". A phone that updates the app keeps both only if this version
 * still looks there -- a "consistent" rename of either would make every phone
 * pair from scratch, and would orphan events the server has never seen.
 */

import "fake-indexeddb/auto";
import { afterEach, describe, expect, it, vi } from "vitest";

import { loadSettings } from "../src/app/settings";
import { IndexedDbEventStore } from "../src/sync/indexeddb-store";

describe("storage written before the rename", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("finds the pairing an earlier version saved", () => {
    const stored = new Map<string, string>([
      ["maxl-bridge-settings", JSON.stringify({ nodeId: 7, ingestToken: "token-from-before" })],
    ]);
    vi.stubGlobal("localStorage", {
      getItem: (key: string) => stored.get(key) ?? null,
      setItem: (key: string, value: string) => void stored.set(key, value),
    });

    const settings = loadSettings();
    expect(settings.nodeId).toBe(7);
    expect(settings.ingestToken).toBe("token-from-before");
  });

  it("finds the events an earlier version held", async () => {
    const before = new IndexedDbEventStore("maxl-bridge");
    await before.append([{ counter: 41, opcode: 0x81, body: Uint8Array.from([1, 2, 3]) }]);
    before.close();

    const now = new IndexedDbEventStore();
    expect((await now.unpushed()).map((e) => e.counter)).toEqual([41]);
    now.close();
  });
});
