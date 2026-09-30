/*
 * Gate 6.9 -- "Foreground-only behaviour documented in UI: last-sync time and
 * pending count visible without connecting."
 *
 * This is a UX gate, and it is the one that decides whether the product is
 * usable at all. CLAUDE.md 4.2: the connection drops the moment the tab is
 * hidden, there is no background mode, and the design is built around that --
 * "the UI states plainly when the last sync happened and how many events are
 * pending on the device". If that only appears once connected, the user opens
 * the app, sees nothing, and concludes the device is broken.
 */

import { JSDOM } from "jsdom";
import { beforeEach, describe, expect, it, vi } from "vitest";

import { describeError, explain, render, type Handlers, type ViewModel } from "../src/app/ui";
import { BridgeProtocolError, VersionMismatchError } from "../src/sync/session";
import { BridgeError } from "../src/protocol/opcodes";

let root: HTMLElement;

const handlers: Handlers = {
  onConnect: vi.fn(),
  onDisconnect: vi.fn(),
  onSync: vi.fn(),
  onSendText: vi.fn(),
  onSaveSettings: vi.fn(),
  onSetTime: vi.fn(),
};

function baseModel(overrides: Partial<ViewModel> = {}): ViewModel {
  return {
    supported: true,
    platformNote: null,
    connection: "idle",
    errorText: null,
    settings: {
      nodeId: 1,
      ingestToken: "token",
      serverBaseUrl: "",
      lastSyncAt: Date.now() - 3 * 3600 * 1000,
      lastSyncEvents: 42,
      pendingOnDevice: 17,
      deviceName: "T-Echo A",
    },
    pendingLocally: 5,
    serverOnline: true,
    device: null,
    messages: [],
    queuedTexts: 0,
    queuedTextAttempts: 0,
    log: [],
    ...overrides,
  };
}

beforeEach(() => {
  const dom = new JSDOM("<!doctype html><div id='app'></div>");
  // The UI module only touches the element it is handed.
  global.HTMLElement = dom.window.HTMLElement as never;
  root = dom.window.document.querySelector("#app")!;
});

describe("gate 6.9 -- the disconnected view", () => {
  it("shows the last sync time without a connection", () => {
    render(root, baseModel({ connection: "idle", device: null }), handlers);
    expect(root.textContent).toContain("Last sync");
    expect(root.textContent).toContain("3 h ago");
  });

  it("shows how many events are still waiting on the device", () => {
    render(root, baseModel(), handlers);
    expect(root.textContent).toContain("Waiting on the device");
    expect(root.textContent).toContain("17");
  });

  it("distinguishes events held on the device from events held here", () => {
    // They are different problems. Events on the device need the user to walk
    // back into range; events here need a network. Collapsing them into one
    // number tells the user nothing about which.
    render(root, baseModel(), handlers);
    expect(root.textContent).toContain("Waiting on the device");
    expect(root.textContent).toContain("Not yet pushed to the server from here");
  });

  it("says plainly why the connection does not survive the background", () => {
    render(root, baseModel(), handlers);
    expect(root.textContent).toMatch(/background/);
    expect(root.textContent).toMatch(/not a fault/);
  });

  it("says so when it has never synced, rather than showing an empty field", () => {
    render(root, baseModel({ settings: { ...baseModel().settings, lastSyncAt: null } }), handlers);
    expect(root.textContent).toContain("never");
  });
});

describe("iOS and other unsupported platforms", () => {
  it("states the platform is unsupported instead of failing silently", () => {
    // CLAUDE.md 4.2: "iOS is not supported -- Apple has declined to implement
    // Web Bluetooth and that has not changed. Say so in the UI rather than
    // failing silently."
    render(
      root,
      baseModel({ supported: false, platformNote: "There is no Web Bluetooth on iOS." }),
      handlers,
    );
    expect(root.textContent).toContain("There is no Web Bluetooth on iOS.");
    // And no connect button that could not possibly work.
    expect(root.querySelector("#connect")).toBeNull();
  });

  it("still shows the sync state on an unsupported platform", () => {
    // The numbers came from a previous session on another device or from a
    // shared account; they are still true and still worth showing.
    render(root, baseModel({ supported: false, platformNote: "not supported" }), handlers);
    expect(root.textContent).toContain("Waiting on the device");
  });
});

describe("configuration", () => {
  it("will not offer to connect before a node id and token exist", () => {
    const model = baseModel({
      settings: { ...baseModel().settings, nodeId: null, ingestToken: null },
    });
    render(root, model, handlers);
    expect(root.querySelector<HTMLButtonElement>("#connect")?.disabled).toBe(true);
  });

  it("offers to queue a message while nothing is connected", () => {
    // The send control used to be rendered only when connected, so writing to
    // the node meant holding it -- the opposite of what CLAUDE.md 4.2 designs
    // for. Gate 6.9's rule applies here too: the disconnected view is the one
    // the user sees most of the time, and it has to be useful.
    render(root, baseModel({ connection: "idle" }), handlers);

    expect(root.querySelector("#send")).not.toBeNull();
    expect(root.querySelector("#send")?.textContent).toMatch(/Queue/);
    expect(root.textContent).toMatch(/the message stays on the phone/);
  });

  it("says how many messages are waiting on the phone", () => {
    render(root, baseModel({ connection: "idle", queuedTexts: 2 }), handlers);
    expect(root.textContent).toMatch(/2 messages are waiting/);
  });

  it("shows a message the node keeps refusing", () => {
    render(root, baseModel({ connection: "idle", queuedTexts: 1, queuedTextAttempts: 4 }), handlers);
    expect(root.textContent).toMatch(/refused 4 times/);
  });

  it("stays quiet about one or two refusals", () => {
    // At a 10 % duty cycle a couple of refusals is the ordinary course of
    // things, and saying so every time would train the user to ignore it.
    render(root, baseModel({ connection: "idle", queuedTexts: 1, queuedTextAttempts: 2 }), handlers);
    expect(root.textContent).not.toMatch(/refused/);
  });

  it("calls it sending once a node is connected", () => {
    render(root, baseModel({ connection: "connected" }), handlers);
    expect(root.querySelector("#send")?.textContent).toMatch(/Send/);
  });

  it("does not echo the token into the page as readable text", () => {
    render(root, baseModel(), handlers);
    const field = root.querySelector<HTMLInputElement>("#token");
    expect(field?.getAttribute("type")).toBe("password");
  });
});

describe("device errors are explained, not printed", () => {
  it("turns a budget refusal into something a person can act on", () => {
    const text = describeError(BridgeError.BudgetExhausted);
    expect(text).toContain("ERR_BUDGET_EXHAUSTED");
    expect(text).toMatch(/radio budget/);
  });

  it("explains a transmit block as a clock problem", () => {
    expect(describeError(BridgeError.NoTime)).toMatch(/time/);
  });

  it("falls back to the raw code for something it has never seen", () => {
    expect(describeError(0x7f)).toBe("0x7f");
  });
});

describe("explain() -- the sentence a failed sync leaves on screen", () => {
  it("names both sides of a version mismatch, per versioning-and-updates.md 2", () => {
    const text = explain(new VersionMismatchError(1));
    expect(text).toContain("bridge protocol 1");
    expect(text).toContain("flashed in the same session");
  });

  it("routes device refusals through describeError", () => {
    const text = explain(new BridgeProtocolError(0x03, BridgeError.NoTime));
    expect(text).toContain("ERR_NO_TIME");
  });

  it("passes ordinary errors and non-errors through readably", () => {
    expect(explain(new Error("GATT gone"))).toBe("GATT gone");
    expect(explain("broken")).toBe("broken");
  });
});
