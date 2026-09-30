/*
 * Pushing the journal to the dashboard.
 *
 * The ordering rule from docs/bridge-protocol.md section 3 lives here: events go
 * to IndexedDB, then to the server, and only when the server has them does
 * ACK_QUEUE go back to the device. Nothing in this file may be reordered without
 * reading that paragraph first.
 *
 * The endpoint authenticates the device-bridge pair with a bearer token
 * (CLAUDE.md 4.3). It is not a login, it is not a session, and it is per node --
 * so a phone paired with two nodes holds two tokens.
 */

import type { JournalEvent } from "./store";

export interface IngestResult {
  received: number;
  inserted: number;
  duplicates: number;
  highWaterMark: number;
}

export interface ServerConfig {
  /** Same origin in the deployed build; overridable for development. */
  baseUrl?: string;
  nodeId: number;
  token: string;
}

/** docs/bridge-protocol.md: at most 512 events per request. */
const MAX_BATCH = 400;

function toBase64(bytes: Uint8Array): string {
  let binary = "";
  for (const byte of bytes) {
    binary += String.fromCharCode(byte);
  }
  return btoa(binary);
}

export class ServerPushError extends Error {
  constructor(readonly status: number, message: string) {
    super(message);
    this.name = "ServerPushError";
  }
}

/**
 * Push a batch. Resolves only when the server has confirmed it, because the
 * caller is about to acknowledge the device's journal on the strength of it.
 */
export async function pushEvents(
  config: ServerConfig,
  events: readonly JournalEvent[],
  receivedAt: Date = new Date(),
): Promise<IngestResult> {
  const base = config.baseUrl ?? "";
  const totals: IngestResult = { received: 0, inserted: 0, duplicates: 0, highWaterMark: 0 };

  for (let offset = 0; offset < events.length; offset += MAX_BATCH) {
    const slice = events.slice(offset, offset + MAX_BATCH);
    const response = await fetch(`${base}/api/ingest`, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        authorization: `Bearer ${config.token}`,
      },
      body: JSON.stringify({
        nodeId: config.nodeId,
        events: slice.map((event) => ({
          // The device journal's own counter (decision D10), which is also this
          // store's key -- not the frame counter inside the body.
          journalCounter: event.counter,
          // Everything the device journals is an event from the device.
          direction: "rx",
          opcode: event.opcode,
          body: toBase64(event.body),
          receivedAt: receivedAt.toISOString(),
        })),
      }),
    });

    if (!response.ok) {
      throw new ServerPushError(
        response.status,
        `ingest refused the batch: ${response.status} ${await response.text()}`,
      );
    }
    const result = (await response.json()) as IngestResult;
    totals.received += result.received;
    totals.inserted += result.inserted;
    totals.duplicates += result.duplicates;
    totals.highWaterMark = Math.max(totals.highWaterMark, Number(result.highWaterMark));
  }

  return totals;
}

/** Whether the dashboard is reachable at all, for the offline banner. */
export async function serverReachable(baseUrl = ""): Promise<boolean> {
  try {
    // HEAD on the login page: it needs no session and no token, and a 2xx or a
    // redirect both mean the origin is up.
    const response = await fetch(`${baseUrl}/login`, { method: "HEAD" });
    return response.status < 500;
  } catch {
    return false;
  }
}

export interface PendingConfig {
  configVersion: number;
  /** The TLV blob, exactly as the dashboard encoded it. */
  tlvs: Uint8Array;
}

/**
 * What the dashboard wants delivered to this node, if anything.
 *
 * Returns null both when nothing is pending and when the server cannot be
 * reached. A phone in a valley has no config to deliver either way, and
 * treating "unreachable" as an error would fail a sync whose whole purpose --
 * getting the journal off the device -- may already have succeeded.
 */
export async function fetchPendingConfig(config: ServerConfig): Promise<PendingConfig | null> {
  const base = config.baseUrl ?? "";
  try {
    const response = await fetch(`${base}/api/config?nodeId=${config.nodeId}`, {
      headers: { authorization: `Bearer ${config.token}` },
    });
    if (!response.ok) {
      return null;
    }
    const body = (await response.json()) as {
      pending: { configVersion: number; tlvs: string } | null;
    };
    if (body.pending === null) {
      return null;
    }
    const binary = atob(body.pending.tlvs);
    const tlvs = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i += 1) {
      tlvs[i] = binary.charCodeAt(i);
    }
    return { configVersion: body.pending.configVersion, tlvs };
  } catch {
    return null;
  }
}
