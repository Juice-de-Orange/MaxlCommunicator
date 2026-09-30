/*
 * Write the projections for one device.
 *
 * This is a full rebuild -- every projection row for the device is deleted and
 * recomputed from the log. That is deliberate, and it is what makes gate 7.2
 * ("projections correct regardless of arrival order") true rather than hoped
 * for: an incremental update has to reason about whether the event it just got
 * belongs before or after everything it has already folded in, and that is
 * precisely the reasoning that goes wrong when a phone reconnects and re-sends a
 * week of journal in whatever order it happens to read it.
 *
 * The cost is O(events per device) on every ingest. At the scale CLAUDE.md
 * describes -- two nodes, a handful of frames an hour, an hourly airtime budget
 * of 360 s that caps the whole thing at about 163 frames per node per hour --
 * that is a few thousand rows a week. If this ever needs to serve a gateway and
 * fifty nodes, the answer is a watermark and an incremental fold, not a faster
 * rebuild. It is not that today.
 */

import { and, asc, eq, isNull } from "drizzle-orm";

import { project, type JournalEvent } from "../lib/projections";
import type { Database } from "./client";
import {
  configVersions,
  deviceSamples,
  deviceState,
  events,
  linkStats,
  messages,
  peerObservations,
} from "./schema";

export async function rebuildProjections(database: Database, deviceId: number): Promise<void> {
  const { db } = database;

  const rows = await db
    .select({
      journalCounter: events.journalCounter,
      direction: events.direction,
      opcode: events.opcode,
      body: events.body,
      receivedAt: events.receivedAt,
    })
    .from(events)
    .where(eq(events.deviceId, deviceId))
    .orderBy(asc(events.journalCounter));

  const journal: JournalEvent[] = rows.map((row) => ({
    journalCounter: row.journalCounter,
    direction: row.direction,
    opcode: row.opcode,
    body: row.body,
    receivedAt: row.receivedAt,
  }));

  const projection = project(journal);

  await db.transaction(async (tx) => {
    await tx.delete(messages).where(eq(messages.deviceId, deviceId));
    await tx.delete(linkStats).where(eq(linkStats.deviceId, deviceId));
    await tx.delete(peerObservations).where(eq(peerObservations.observerDeviceId, deviceId));
    await tx.delete(deviceSamples).where(eq(deviceSamples.deviceId, deviceId));

    if (projection.samples.length > 0) {
      await tx.insert(deviceSamples).values(
        projection.samples.map((sample) => ({
          deviceId,
          journalCounter: sample.journalCounter,
          batteryMv: sample.batteryMv,
          uptimeS: sample.uptimeS,
          queueDepth: sample.queueDepth,
          budgetBand: sample.budgetBand,
          budgetUsedMs: sample.budgetUsedMs,
          budgetLimitMs: sample.budgetLimitMs,
          occurredAt: sample.occurredAt,
        })),
      );
    }

    if (projection.messages.length > 0) {
      await tx.insert(messages).values(
        projection.messages.map((message) => ({
          deviceId,
          peerNodeId: message.peerNodeId,
          direction: message.direction,
          frameCounter: message.frameCounter,
          seq: message.seq,
          text: message.text,
          state: message.state,
          attempts: message.attempts,
          rssi: message.rssi,
          snr: message.snr,
          occurredAt: message.occurredAt,
          stateChangedAt: message.stateChangedAt,
        })),
      );
    }

    if (projection.linkStats.length > 0) {
      await tx.insert(linkStats).values(
        projection.linkStats.map((stat) => ({
          deviceId,
          peerNodeId: stat.peerNodeId,
          frameCounter: stat.frameCounter,
          rssi: stat.rssi,
          snr: stat.snr,
          sf: stat.sf,
          occurredAt: stat.occurredAt,
        })),
      );
    }

    if (projection.peerObservations.length > 0) {
      await tx.insert(peerObservations).values(
        projection.peerObservations.map((observation) => ({
          observerDeviceId: deviceId,
          peerNodeId: observation.peerNodeId,
          frameCounter: observation.frameCounter,
          frameType: observation.frameType,
          latDeg7: observation.latDeg7,
          lonDeg7: observation.lonDeg7,
          altM: observation.altM,
          hdop: observation.hdop,
          tempC100: observation.tempC100,
          humidity100: observation.humidity100,
          pressurePa: observation.pressurePa,
          batteryMv: observation.batteryMv,
          uptimeS: observation.uptimeS,
          occurredAt: observation.occurredAt,
        })),
      );
    }

    await tx
      .insert(deviceState)
      .values({ deviceId, ...projection.state })
      .onConflictDoUpdate({
        target: deviceState.deviceId,
        set: { ...projection.state },
      });

    /*
     * config_versions is not rebuilt -- it holds a row the dashboard wrote when
     * it pushed a config, and rebuilding would destroy pushed_at. Only
     * applied_at is filled in, and only from the device's own
     * EVT_CONFIG_APPLIED. CLAUDE.md 4.3: "a pushed config is not an applied
     * config."
     *
     * The isNull guard is what makes replaying the whole log harmless: the first
     * acknowledgement stands, and a re-sent event cannot move the timestamp.
     */
    for (const applied of projection.configApplications) {
      await tx
        .update(configVersions)
        .set({
          appliedAt: applied.appliedAt,
          appliedMask: applied.appliedMask,
          unappliedTypes: applied.unappliedTypes,
        })
        .where(
          and(
            eq(configVersions.deviceId, deviceId),
            eq(configVersions.configVersion, applied.configVersion),
            isNull(configVersions.appliedAt),
          ),
        );
    }
  });
}
