/*
 * Everything the pages read. Kept out of the .astro files so the SQL is in one
 * place and so the pages stay about layout.
 */

import { and, desc, eq, gte, sql } from "drizzle-orm";

import { getDatabase } from "../db/client";
import {
  deviceSamples,
  deviceState,
  devices,
  events,
  linkStats,
  messages,
  peerObservations,
  configVersions,
} from "../db/schema";

export const DEFAULT_WINDOW_HOURS = 24;

export function windowStart(hours = DEFAULT_WINDOW_HOURS): Date {
  return new Date(Date.now() - hours * 3600 * 1000);
}

export async function listNodes() {
  const { db } = getDatabase();
  return db
    .select({
      id: devices.id,
      nodeId: devices.nodeId,
      name: devices.name,
      lastSeenAt: devices.lastSeenAt,
      state: deviceState,
    })
    .from(devices)
    .leftJoin(deviceState, eq(deviceState.deviceId, devices.id))
    .orderBy(devices.nodeId);
}

export async function nodeByNodeId(nodeId: number) {
  const { db } = getDatabase();
  const [row] = await db
    .select({
      id: devices.id,
      nodeId: devices.nodeId,
      name: devices.name,
      lastSeenAt: devices.lastSeenAt,
      state: deviceState,
    })
    .from(devices)
    .leftJoin(deviceState, eq(deviceState.deviceId, devices.id))
    .where(eq(devices.nodeId, nodeId))
    .limit(1);
  return row ?? null;
}

export async function samplesFor(deviceId: number, since: Date) {
  const { db } = getDatabase();
  return db
    .select()
    .from(deviceSamples)
    .where(and(eq(deviceSamples.deviceId, deviceId), gte(deviceSamples.occurredAt, since)))
    .orderBy(deviceSamples.occurredAt);
}

export async function observationsFor(deviceId: number, since: Date) {
  const { db } = getDatabase();
  return db
    .select()
    .from(peerObservations)
    .where(
      and(
        eq(peerObservations.observerDeviceId, deviceId),
        gte(peerObservations.occurredAt, since),
      ),
    )
    .orderBy(peerObservations.occurredAt);
}

export async function linkStatsFor(deviceId: number, since: Date) {
  const { db } = getDatabase();
  return db
    .select()
    .from(linkStats)
    .where(and(eq(linkStats.deviceId, deviceId), gte(linkStats.occurredAt, since)))
    .orderBy(linkStats.occurredAt);
}

export async function messagesFor(deviceId: number, limit = 100) {
  const { db } = getDatabase();
  return db
    .select()
    .from(messages)
    .where(eq(messages.deviceId, deviceId))
    .orderBy(desc(messages.occurredAt))
    .limit(limit);
}

export async function allMessages(limit = 200) {
  const { db } = getDatabase();
  return db
    .select({
      message: messages,
      nodeId: devices.nodeId,
      nodeName: devices.name,
    })
    .from(messages)
    .innerJoin(devices, eq(devices.id, messages.deviceId))
    .orderBy(desc(messages.occurredAt))
    .limit(limit);
}

export async function configVersionsFor(deviceId: number) {
  const { db } = getDatabase();
  return db
    .select()
    .from(configVersions)
    .where(eq(configVersions.deviceId, deviceId))
    .orderBy(desc(configVersions.configVersion))
    .limit(20);
}

export async function eventCountFor(deviceId: number) {
  const { db } = getDatabase();
  const [row] = await db
    .select({ n: sql<number>`count(*)::int` })
    .from(events)
    .where(eq(events.deviceId, deviceId));
  return row?.n ?? 0;
}

/**
 * The peers this device has actually heard, newest observation first. Used to
 * show a node that has not synced itself -- see the comment on the
 * peer_observations table.
 */
export async function peersHeardBy(deviceId: number, since: Date) {
  const rows = await observationsFor(deviceId, since);
  const byPeer = new Map<number, (typeof rows)[number]>();
  for (const row of rows) {
    const existing = byPeer.get(row.peerNodeId);
    if (!existing || row.occurredAt > existing.occurredAt) {
      byPeer.set(row.peerNodeId, row);
    }
  }
  return [...byPeer.entries()].map(([peerNodeId, latest]) => ({ peerNodeId, latest }));
}
