/*
 * The whole user interface.
 *
 * Two things here are product decisions rather than layout, and both come
 * straight from CLAUDE.md 4.2:
 *
 *   1. **The disconnected view is the primary view.** "The connection drops when
 *      the tab is hidden and there is no background mode ... the device queues
 *      everything, and the user opens the app to sync. The UI states plainly
 *      when the last sync happened and how many events are pending on the
 *      device." So the last sync and the pending count are rendered before
 *      anything is connected, from what the last session stored -- that is gate
 *      6.9, and it is why they live in settings rather than in memory.
 *
 *   2. **iOS is told, not failed.** "Apple has declined to implement Web
 *      Bluetooth and that has not changed. Say so in the UI rather than failing
 *      silently."
 */

import { BRIDGE_ERROR_NAMES } from "../protocol/opcodes";
import { BridgeProtocolError, VersionMismatchError } from "../sync/session";
import type { Settings } from "./settings";

export type ConnectionState = "idle" | "connecting" | "connected" | "syncing" | "error";

export interface DeviceSnapshot {
  name: string | null;
  bridgeProtocol: number | null;
  bonded: boolean;
  batteryMv: number | null;
  uptimeS: number | null;
  queueDepth: number | null;
  budgetUsedMs: number | null;
  budgetLimitMs: number | null;
  budgetBand: number | null;
  timeValid: boolean | null;
  keyProvisioned: boolean | null;
}

export interface MessageLine {
  at: number;
  direction: "in" | "out";
  peer: number | null;
  text: string | null;
  state: string;
}

export interface ViewModel {
  supported: boolean;
  platformNote: string | null;
  connection: ConnectionState;
  errorText: string | null;
  settings: Settings;
  pendingLocally: number;
  serverOnline: boolean | null;
  device: DeviceSnapshot | null;
  messages: MessageLine[];
  /** Texts written while disconnected, still waiting on this phone. */
  queuedTexts: number;
  /** The most connections any one of them has been refused over. */
  queuedTextAttempts: number;
  log: string[];
}

export interface Handlers {
  onConnect(): void;
  onDisconnect(): void;
  onSync(): void;
  onSendText(dst: number, text: string): void;
  onSaveSettings(nodeId: number, token: string, baseUrl: string): void;
  onSetTime(): void;
}

const escape = (value: string) =>
  value.replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]!,
  );

function relative(at: number | null): string {
  if (!at) return "never";
  const seconds = Math.round((Date.now() - at) / 1000);
  if (seconds < 60) return "just now";
  if (seconds < 3600) return `${Math.round(seconds / 60)} min ago`;
  if (seconds < 86400) return `${Math.round(seconds / 3600)} h ago`;
  return `${Math.round(seconds / 86400)} d ago`;
}

const STATE_LABEL: Record<string, string> = {
  /*
   * Three waiting states, and they are not the same thing to the user. §2.4
   * already distinguishes the device's two -- "queued until HH:MM" for the
   * budget and "in flight" for a retry. This adds the one before either: the
   * message has not reached the node at all and is sitting on this phone,
   * which is what the whole foreground-only design (CLAUDE.md 4.2) produces.
   */
  on_phone: "waiting on this phone",
  queued: "waiting for budget",
  in_flight: "in flight",
  delivered: "delivered",
  undelivered: "not delivered",
  received: "received",
};

function syncCard(model: ViewModel): string {
  const { settings, pendingLocally, serverOnline } = model;
  return `
    <section class="card">
      <h2>Status</h2>
      <div class="row">
        <span class="secondary">Last sync with the node</span>
        <span class="tabular">${relative(settings.lastSyncAt)}</span>
      </div>
      <div class="row">
        <span class="secondary">Events last time</span>
        <span class="tabular">${settings.lastSyncEvents}</span>
      </div>
      <div class="row">
        <span class="secondary">Waiting on the device</span>
        <span class="tabular">${settings.pendingOnDevice}</span>
      </div>
      <div class="row">
        <span class="secondary">Not yet pushed to the server from here</span>
        <span class="tabular">${pendingLocally}</span>
      </div>
      <div class="row">
        <span class="secondary">Dashboard reachable</span>
        <span class="pill ${serverOnline === true ? "on" : "off"}">
          ${serverOnline === null ? "unknown" : serverOnline ? "yes" : "no"}
        </span>
      </div>
      <p class="muted" style="margin-top:0.75rem">
        The connection to the node drops as soon as this tab goes to the background — that is
        how Web Bluetooth works, not a fault. The device keeps collecting; these numbers
        hold without a connection too.
      </p>
    </section>`;
}

function deviceCard(device: DeviceSnapshot): string {
  const budget =
    device.budgetUsedMs !== null && device.budgetLimitMs
      ? `${(device.budgetUsedMs / 1000).toFixed(0)} s of ${(device.budgetLimitMs / 1000).toFixed(0)} s`
      : "—";
  const warnings: string[] = [];
  if (device.timeValid === false) {
    warnings.push(
      `<p class="notice bad">No valid time — the node is fully blocked from transmitting.
       <button id="set-time">Set time</button></p>`,
    );
  }
  if (device.keyProvisioned === false) {
    warnings.push(`<p class="notice">No network key provisioned.</p>`);
  }
  return `
    <section class="card">
      <div class="row">
        <h2>${escape(device.name ?? "Node")}</h2>
        <span class="pill ${device.bonded ? "on" : "off"}">${device.bonded ? "bonded" : "not bonded"}</span>
      </div>
      <div class="row"><span class="secondary">Battery</span>
        <span class="tabular">${device.batteryMv ? `${(device.batteryMv / 1000).toFixed(2)} V` : "—"}</span></div>
      <div class="row"><span class="secondary">Queue</span>
        <span class="tabular">${device.queueDepth ?? "—"}</span></div>
      <div class="row"><span class="secondary">Radio budget ${device.budgetBand === 1 ? "g1" : "g3"}</span>
        <span class="tabular">${budget}</span></div>
      <div class="row"><span class="secondary">Bridge protocol</span>
        <span class="tabular">${device.bridgeProtocol ?? "—"}</span></div>
      ${warnings.join("")}
    </section>`;
}

export function render(root: HTMLElement, model: ViewModel, handlers: Handlers): void {
  const configured = model.settings.nodeId !== null && model.settings.ingestToken !== null;

  root.innerHTML = `
    <header class="row" style="margin-bottom:1rem">
      <h1>MaxlCommunicator</h1>
      <span class="pill ${model.connection === "connected" || model.connection === "syncing" ? "on" : "off"}">
        ${
          { idle: "disconnected", connecting: "connecting…", connected: "connected",
            syncing: "syncing…", error: "error" }[model.connection]
        }
      </span>
    </header>

    ${
      model.platformNote
        ? `<section class="card"><p class="notice bad">${escape(model.platformNote)}</p></section>`
        : ""
    }

    ${syncCard(model)}

    ${
      model.errorText
        ? `<section class="card"><p class="notice bad">${escape(model.errorText)}</p></section>`
        : ""
    }

    ${model.device ? deviceCard(model.device) : ""}

    ${
      model.supported
        ? `<section class="card">
      ${
        model.connection === "connected" || model.connection === "syncing"
            ? `<button id="sync" class="primary wide">Sync now</button>
               <button id="disconnect" class="wide" style="margin-top:0.5rem">Disconnect</button>`
            : `<button id="connect" class="primary wide" ${configured ? "" : "disabled"}>
                 Connect to node
               </button>
               ${configured ? "" : `<p class="muted" style="margin-top:0.5rem">Enter the node ID and token below first.</p>`}`
      }
    </section>`
        : ""
    }

    ${
      model.messages.length > 0
        ? `<section class="card">
             <h2>Messages</h2>
             ${model.messages
               .map(
                 (message) => `
               <div class="msg">
                 <div class="row">
                   <span>${message.text ? escape(message.text) : '<span class="muted">no content</span>'}</span>
                   <span class="muted tabular">${new Date(message.at).toLocaleTimeString("en-GB", { hour: "2-digit", minute: "2-digit" })}</span>
                 </div>
                 <div class="muted">
                   ${message.direction === "in" ? "from" : "to"} ${message.peer ?? "—"} ·
                   <span class="state-${message.state}">${STATE_LABEL[message.state] ?? message.state}</span>
                 </div>
               </div>`,
               )
               .join("")}
           </section>`
        : ""
    }

    <section class="card">
      <h2>Send message</h2>
      <label>Recipient (node ID)<input id="dst" type="number" min="0" max="65534" value="2" /></label>
      <label>Text (at most 48 bytes)<input id="text" maxlength="48" placeholder="…" /></label>
      <button id="send" class="primary wide">
        ${model.connection === "connected" ? "Send" : "Queue"}
      </button>
      <p class="muted" style="margin-top:0.5rem">
        ${
          model.connection === "connected"
            ? `Goes through the device's budget tracker. If the message has to wait, it says so here —
               it is not lost.`
            : `Without a connection the message stays on the phone and goes out on the next sync.
               It survives closing the app, too.`
        }
      </p>
      ${
        model.queuedTexts > 0
          ? `<p class="muted">${model.queuedTexts} ${model.queuedTexts === 1 ? "message is" : "messages are"} waiting on the phone.</p>`
          : ""
      }
      ${
        // Shown, never acted on: the count is here so a message the node keeps
        // refusing becomes visible instead of quietly cycling for ever. Nothing
        // discards on it -- that would be the data loss, only slower.
        model.queuedTextAttempts > 2
          ? `<p class="muted">One of them has already been refused ${model.queuedTextAttempts} times —
             the reason is shown above. It keeps being retried until it gets through.</p>`
          : ""
      }
    </section>

    <section class="card">
      <h2>Settings</h2>
      <label>Node ID
        <input id="node-id" type="number" min="0" max="65534" value="${model.settings.nodeId ?? ""}" /></label>
      <label>Ingest token of the node
        <input id="token" type="password" value="${escape(model.settings.ingestToken ?? "")}"
               placeholder="from npm run device:add" /></label>
      <label>Dashboard address (empty = same origin)
        <input id="base-url" value="${escape(model.settings.serverBaseUrl)}" placeholder="" /></label>
      <button id="save" class="wide">Save</button>
      <p class="muted" style="margin-top:0.5rem">
        The token authenticates the device-and-bridge pair, not you. It belongs to exactly
        one node.
      </p>
    </section>

    ${
      model.log.length > 0
        ? `<section class="card"><h2>Log</h2><div class="log">${escape(model.log.slice(-40).join("\n"))}</div></section>`
        : ""
    }`;

  root.querySelector("#connect")?.addEventListener("click", handlers.onConnect);
  root.querySelector("#disconnect")?.addEventListener("click", handlers.onDisconnect);
  root.querySelector("#sync")?.addEventListener("click", handlers.onSync);
  root.querySelector("#set-time")?.addEventListener("click", handlers.onSetTime);

  root.querySelector("#send")?.addEventListener("click", () => {
    const dst = Number.parseInt((root.querySelector("#dst") as HTMLInputElement).value, 10);
    const text = (root.querySelector("#text") as HTMLInputElement).value.trim();
    if (Number.isInteger(dst) && text.length > 0) {
      handlers.onSendText(dst, text);
    }
  });

  root.querySelector("#save")?.addEventListener("click", () => {
    const nodeId = Number.parseInt((root.querySelector("#node-id") as HTMLInputElement).value, 10);
    const token = (root.querySelector("#token") as HTMLInputElement).value.trim();
    const baseUrl = (root.querySelector("#base-url") as HTMLInputElement).value.trim();
    handlers.onSaveSettings(nodeId, token, baseUrl);
  });
}

/**
 * Turn any failure of a sync into the sentence the user sees.
 *
 * Lived untested in main.ts until 2026-09-01 -- and the version-mismatch text
 * is exactly what someone holding a half-flashed pair reads, so it is the one
 * string that must never regress silently (versioning-and-updates.md 2: "the
 * client refuses the conversation and tells the user which side is behind").
 */
export function explain(error: unknown): string {
  if (error instanceof VersionMismatchError) {
    return `The node speaks bridge protocol ${error.deviceVersion}, this app a different one. ` +
      `Both sides must be flashed in the same session.`;
  }
  if (error instanceof BridgeProtocolError) {
    return describeError(error.code);
  }
  if (error instanceof Error) {
    return error.message;
  }
  return String(error);
}

/** Turn a device error code into something a person can act on. */
export function describeError(code: number): string {
  const name = BRIDGE_ERROR_NAMES[code] ?? `0x${code.toString(16)}`;
  const advice: Record<string, string> = {
    ERR_NOT_AUTHORISED: "The node requires a bonded connection with passkey.",
    ERR_NO_KEY: "No network key is provisioned.",
    ERR_BUDGET_EXHAUSTED: "The radio budget is exhausted — the node reports when it can continue.",
    ERR_NO_TIME: "The node has no valid time and is blocked from transmitting.",
    ERR_QUEUE_FULL: "The node's queue is full.",
    ERR_STORAGE: "A flash write on the node failed.",
  };
  return advice[name] ? `${name}: ${advice[name]}` : name;
}
