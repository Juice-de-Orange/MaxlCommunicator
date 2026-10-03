/*
 * Config push, both halves.
 *
 *   POST /api/config          the dashboard queues a config for a node
 *   GET  /api/config/pending  the bridge collects what has not been applied
 *
 * CLAUDE.md 4.3: "config_versions carries applied_at, set from a device
 * acknowledgement -- a pushed config is not an applied config." So POST writes a
 * row with pushed_at and nothing else; applied_at is filled in later, and only
 * from the device's own EVT_CONFIG_APPLIED, by the projection in
 * db/apply-projections.ts.
 *
 * The two halves authenticate differently on purpose. Pushing is an operator
 * action and takes the dashboard session; collecting is the bridge, and takes
 * the device's ingest token -- the same distinction CLAUDE.md 4.3 draws for
 * ingestion, and for the same reason: nothing on the bridge's path may assume a
 * browser.
 */

import type { APIRoute } from "astro";
import { and, desc, eq, isNull, sql } from "drizzle-orm";

import { getDatabase } from "../../db/client";
import { configVersions, devices } from "../../db/schema";
import { bearerToken, hasValidSession, hashIngestToken } from "../../lib/auth";
import {
  encodeConfigTlvs,
  validateConfig,
  type ConfigSettings,
} from "../../lib/config-tlv";
import { isNodeId, NODE_ID_RULE, parseNodeId } from "../../lib/node-id";

export const prerender = false;

function json(status: number, body: unknown): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json" },
  });
}

export const POST: APIRoute = async ({ request }) => {
  const secret = process.env.SESSION_SECRET;
  if (!secret || !hasValidSession(request.headers.get("cookie"), secret)) {
    return json(401, { error: "unauthorised" });
  }

  let payload: { nodeId?: number; settings?: ConfigSettings };
  try {
    payload = (await request.json()) as typeof payload;
  } catch {
    return json(400, { error: "bad_request", detail: "body is not JSON" });
  }
  if (typeof payload.nodeId !== "number" || typeof payload.settings !== "object"
      || payload.settings === null) {
    return json(400, { error: "bad_request", detail: "expected { nodeId, settings }" });
  }
  if (!isNodeId(payload.nodeId)) {
    return json(400, { error: "bad_request", detail: NODE_ID_RULE });
  }

  const problem = validateConfig(payload.settings);
  if (problem !== null) {
    return json(400, { error: "bad_param", detail: problem });
  }

  const tlvs = encodeConfigTlvs(payload.settings);
  if (tlvs.length === 0) {
    return json(400, { error: "bad_request", detail: "no settings to push" });
  }

  const { db } = getDatabase();
  const [device] = await db
    .select({ id: devices.id })
    .from(devices)
    .where(eq(devices.nodeId, payload.nodeId))
    .limit(1);
  if (!device) {
    return json(404, { error: "no_such_node" });
  }

  /*
   * The version is the dashboard's monotonic counter (docs/bridge-protocol.md,
   * SET_CONFIG). Derived from the highest already stored rather than from a
   * clock, so two pushes in the same second cannot collide -- and the unique
   * index on (device_id, config_version) is what would catch it if they did.
   */
  const [highest] = await db
    .select({ version: sql<number>`coalesce(max(${configVersions.configVersion}), 0)::int` })
    .from(configVersions)
    .where(eq(configVersions.deviceId, device.id));
  const nextVersion = (highest?.version ?? 0) + 1;

  await db.insert(configVersions).values({
    deviceId: device.id,
    configVersion: nextVersion,
    tlvs,
  });

  return json(200, {
    configVersion: nextVersion,
    tlvBytes: tlvs.length,
    // Said plainly, because it is the thing people get wrong about this endpoint.
    note: "queued, not applied -- applied_at is set only from the device's EVT_CONFIG_APPLIED",
  });
};

/**
 * What the bridge should deliver on its next connection: the newest config that
 * has not been acknowledged.
 *
 * Only the newest. If an operator pushed three configs while the node was out of
 * range, delivering all three would spend three SET_CONFIG round trips to arrive
 * at the state the last one describes -- and every one of those costs BLE
 * connection time, which docs/bridge-protocol.md section 6 names as the actual
 * bottleneck.
 */
export const GET: APIRoute = async ({ request, url }) => {
  const token = bearerToken(request);
  // Not a node id at all answers like an unknown node, as before -- and no
  // longer reaches the query, where 99999999999 was a 500.
  const nodeId = parseNodeId(url.searchParams.get("nodeId"));
  if (!token || nodeId === null) {
    return json(401, { error: "unauthorised" });
  }

  const { db } = getDatabase();
  const [device] = await db
    .select({ id: devices.id, hash: devices.ingestTokenHash })
    .from(devices)
    .where(eq(devices.nodeId, nodeId))
    .limit(1);
  if (!device || device.hash !== hashIngestToken(token)) {
    return json(401, { error: "unauthorised" });
  }

  const [pending] = await db
    .select({ configVersion: configVersions.configVersion, tlvs: configVersions.tlvs })
    .from(configVersions)
    .where(and(eq(configVersions.deviceId, device.id), isNull(configVersions.appliedAt)))
    .orderBy(desc(configVersions.configVersion))
    .limit(1);

  if (!pending) {
    return json(200, { pending: null });
  }
  return json(200, {
    pending: {
      configVersion: pending.configVersion,
      tlvs: Buffer.from(pending.tlvs).toString("base64"),
    },
  });
};
