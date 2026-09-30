/*
 * The application. Everything below the surface already exists -- the protocol,
 * the session and its implementation of docs/bridge-protocol.md section 5, the
 * store. This file holds them together and keeps the view in step.
 *
 * There is deliberately no service worker sync, no wake lock and no periodic
 * background sync. CLAUDE.md 4.2 is explicit: "Do not attempt to work around the
 * foreground limitation with service workers, wake locks or periodic background
 * sync -- none of them keep a GATT connection alive. Background sync is what
 * Phase 9 is for." The service worker that is registered caches the application
 * shell so the app opens without a network, and does nothing else.
 */

import { decodeStatus, STATUS_BYTES } from "./protocol/payloads";
import { TxResult } from "./protocol/opcodes";
import { BridgeSession } from "./sync/session";
import { IndexedDbEventStore, IndexedDbOutbox } from "./sync/indexeddb-store";
import { fetchPendingConfig, pushEvents, serverReachable } from "./sync/server";
import { isSupported, WebBluetoothTransport } from "./transport/webble";
import { loadSettings, saveSettings, type Settings } from "./app/settings";
import { explain, render, type MessageLine, type ViewModel } from "./app/ui";

const root = document.querySelector<HTMLElement>("#app")!;
const store = new IndexedDbEventStore();
const outbox = new IndexedDbOutbox();

let settings: Settings = loadSettings();
let session: BridgeSession | null = null;
let transport: WebBluetoothTransport | null = null;

const support = isSupported();

/**
 * The user-facing wording, keyed on the stable code rather than on the English
 * string the transport returns for logs.
 *
 * CLAUDE.md 4.2: "iOS is not supported -- Apple has declined to implement Web
 * Bluetooth and that has not changed. Say so in the UI rather than failing
 * silently." Saying so is this function.
 */
function platformNote(code: string | undefined): string {
  switch (code) {
    case "ios":
      return (
        "There is no Web Bluetooth on iOS — Apple does not implement it, and that has not " +
        "changed. This app cannot talk to any node here. It needs an Android phone with " +
        "Chrome."
      );
    case "insecure-context":
      return (
        "Web Bluetooth requires a secure context. This page must be loaded over HTTPS " +
        "(localhost counts as secure)."
      );
    default:
      return "This browser has no Web Bluetooth. Chrome on Android is the supported way.";
  }
}

const model: ViewModel = {
  supported: support.supported,
  platformNote: support.supported ? null : platformNote(support.code),
  connection: "idle",
  errorText: null,
  settings,
  pendingLocally: 0,
  serverOnline: null,
  device: null,
  messages: [],
  queuedTexts: 0,
  queuedTextAttempts: 0,
  log: [],
};

function log(line: string): void {
  model.log.push(`${new Date().toLocaleTimeString("en-GB")}  ${line}`);
  if (model.log.length > 200) model.log.splice(0, model.log.length - 200);
}

function draw(): void {
  model.settings = settings;
  render(root, model, handlers);
}

async function refreshCounts(): Promise<void> {
  model.pendingLocally = (await store.unpushed()).length;
  const waiting = await outbox.pending();
  model.queuedTexts = waiting.length;
  model.queuedTextAttempts = waiting.reduce((most, entry) => Math.max(most, entry.attempts), 0);
  draw();
}

function readStatus(body: Uint8Array): void {
  if (body.length < STATUS_BYTES) return;
  const status = decodeStatus(body.slice(0, STATUS_BYTES));
  model.device = {
    ...(model.device ?? { name: settings.deviceName, bridgeProtocol: null, bonded: false }),
    name: model.device?.name ?? settings.deviceName,
    bridgeProtocol: model.device?.bridgeProtocol ?? null,
    bonded: transport?.bonded ?? false,
    batteryMv: status.batteryMv,
    uptimeS: status.uptimeS,
    queueDepth: status.queueDepth,
    budgetUsedMs: status.budgetUsedMs,
    budgetLimitMs: status.budgetLimitMs,
    budgetBand: status.band,
    timeValid: (status.flags & 0x01) !== 0,
    keyProvisioned: (status.flags & 0x02) !== 0,
  };
  settings.pendingOnDevice = status.queueDepth;
  saveSettings(settings);
}

function noteEvent(event: { kind: string } & Record<string, unknown>): void {
  if (event.kind === "frame-tx-result") {
    const result = event.result as number;
    const state =
      result === TxResult.Delivered
        ? "delivered"
        : result === TxResult.Undelivered
          ? "undelivered"
          : result === TxResult.Queued
            ? "queued"
            : "in_flight";
    const line: MessageLine = {
      at: Date.now(),
      direction: "out",
      peer: event.dst as number,
      text: null,
      state,
    };
    // One frame, several journal entries -- replace rather than append, so the
    // list shows a message and its current state, not the same message three
    // times (decision D10).
    const index = model.messages.findIndex(
      (existing) => existing.direction === "out" && existing.peer === line.peer,
    );
    if (index >= 0) model.messages[index] = line;
    else model.messages.unshift(line);
  } else if (event.kind === "frame-rx") {
    model.messages.unshift({
      at: Date.now(),
      direction: "in",
      peer: event.src as number,
      text: null,
      state: "received",
    });
  }
  model.messages = model.messages.slice(0, 30);
}

async function connect(): Promise<void> {
  model.connection = "connecting";
  model.errorText = null;
  draw();
  try {
    transport = await WebBluetoothTransport.connect();
    session = new BridgeSession(transport, store);
    session.onEvent((event) => {
      noteEvent(event as never);
      draw();
    });
    session.open();

    const info = await session.getInfo();
    model.device = {
      name: settings.deviceName,
      bridgeProtocol: info.bridgeProtocol,
      bonded: transport.bonded,
      batteryMv: null, uptimeS: null, queueDepth: null,
      budgetUsedMs: null, budgetLimitMs: null, budgetBand: null,
      timeValid: null, keyProvisioned: null,
    };
    readStatus(await session.getStatus());

    model.connection = "connected";
    log(`connected, BRIDGE_PROTO ${info.bridgeProtocol}, node ${info.deviceId}`);
    await refreshCounts();
    await sync();
  } catch (error) {
    model.connection = "error";
    model.errorText = explain(error);
    log(model.errorText);
    draw();
  }
}


async function sync(): Promise<void> {
  if (!session) return;
  if (settings.nodeId === null || !settings.ingestToken) {
    model.errorText = "Node ID and ingest token are missing — without them nothing can reach the server.";
    draw();
    return;
  }

  model.connection = "syncing";
  model.errorText = null;
  draw();

  try {
    const result = await session.run({
      nowUnix: Math.floor(Date.now() / 1000),
      deviceReportsNoTime: model.device?.timeValid === false,
      outbox,
      push: async (events) => {
        const outcome = await pushEvents(
          {
            baseUrl: settings.serverBaseUrl,
            nodeId: settings.nodeId!,
            token: settings.ingestToken!,
          },
          events,
        );
        log(`Server: ${outcome.inserted} new, ${outcome.duplicates} already present`);
      },
    });

    settings.lastSyncAt = Date.now();
    settings.lastSyncEvents = result.eventsFetched;
    saveSettings(settings);
    log(`Sync done: ${result.eventsFetched} events fetched`);

    if (result.textsSent > 0) {
      log(`${result.textsSent} queued message(s) sent`);
      // The UI knew them as waiting on the phone; the node has them now, and
      // from here on its own delivery states apply.
      model.messages = model.messages.map((message) =>
        message.state === "on_phone" ? { ...message, state: "in_flight" } : message,
      );
    }
    if (result.refusal) {
      // Not thrown, so the sync above still counts. A refusal is the node
      // answering, and the answer is what the user needs to read.
      model.errorText = explain(result.refusal);
      log(`Message refused: ${model.errorText}`);
    }

    /*
     * The other direction, and it goes last on purpose.
     *
     * Getting the journal off the device is what a sync is for; delivering a
     * config is a convenience. Doing it first would mean a SET_CONFIG that fails
     * -- because the node is out of range again, because the budget is
     * exhausted -- takes the journal transfer down with it.
     */
    const pending = await fetchPendingConfig({
      baseUrl: settings.serverBaseUrl,
      nodeId: settings.nodeId!,
      token: settings.ingestToken!,
    });
    if (pending !== null) {
      await session.setConfig(pending.configVersion, pending.tlvs);
      log(
        `Config version ${pending.configVersion} handed to the node — ` +
          `it is applied only once the node acknowledges it`,
      );
    }

    readStatus(await session.getStatus());
    model.connection = "connected";
  } catch (error) {
    model.connection = "connected";
    model.errorText = explain(error);
    log(`Sync aborted: ${model.errorText}`);
    // Nothing is lost: without a successful push there is no ACK_QUEUE, and the
    // device still holds every event (docs/bridge-protocol.md section 3).
  }
  model.serverOnline = await serverReachable(settings.serverBaseUrl);
  await refreshCounts();
}

const handlers = {
  onConnect: () => void connect(),
  onDisconnect: () => {
    void (async () => {
      await session?.close();
      await transport?.disconnect();
      session = null;
      transport = null;
      model.connection = "idle";
      model.device = null;
      log("disconnected");
      draw();
    })();
  },
  onSync: () => void sync(),
  onSetTime: () => {
    void (async () => {
      try {
        await session?.setTime(Math.floor(Date.now() / 1000));
        log("time set");
        if (session) readStatus(await session.getStatus());
      } catch (error) {
        model.errorText = explain(error);
      }
      draw();
    })();
  },
  onSendText: (dst: number, text: string) => {
    void (async () => {
      try {
        if (session) {
          await session.sendText(dst, text);
          model.messages.unshift({
            at: Date.now(), direction: "out", peer: dst, text, state: "in_flight",
          });
          log(`sent to ${dst}`);
        } else {
          /*
           * Nothing connected, so the message waits here. Before this existed
           * the send control was simply hidden while disconnected -- honest,
           * but it meant the user had to be holding the node to write to it,
           * and the whole point of CLAUDE.md 4.2 is that they are not.
           *
           * Durably, and only then shown: an entry the user can see but that
           * did not reach IndexedDB would vanish on the next reload, which is
           * exactly the failure this is supposed to prevent.
           */
          await outbox.add(dst, text, Date.now());
          model.messages.unshift({
            at: Date.now(), direction: "out", peer: dst, text, state: "on_phone",
          });
          log(`queued for ${dst}`);
        }
      } catch (error) {
        model.errorText = explain(error);
      }
      await refreshCounts();
      draw();
    })();
  },
  onSaveSettings: (nodeId: number, token: string, baseUrl: string) => {
    settings = {
      ...settings,
      nodeId: Number.isInteger(nodeId) ? nodeId : null,
      ingestToken: token || null,
      serverBaseUrl: baseUrl,
    };
    saveSettings(settings);
    log("settings saved");
    void refreshCounts();
  },
};

// The disconnected view has to be right before anything is connected -- that is
// gate 6.9, and it is the view the user sees most of the time.
draw();
void refreshCounts();
void serverReachable(settings.serverBaseUrl).then((online) => {
  model.serverOnline = online;
  draw();
});

if ("serviceWorker" in navigator) {
  // App shell only. See the note at the top of this file.
  navigator.serviceWorker.register("./sw.js").catch(() => {
    // A browser that refuses the worker still works online, which is the only
    // mode that can talk to a node anyway.
  });
}
