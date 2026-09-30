/*
 * CLAUDE.md 4.3: "Append-only event log. Every frame received from any device is
 * inserted as an immutable event row; all current state (last position, latest
 * telemetry, delivery status) is derived by projection, never mutated in place."
 *
 * That splits this file in two, and the split is the whole design:
 *
 *   events            the log. Insert only. Nothing updates or deletes a row.
 *   messages,         projections. Outputs of a pure function over the log, and
 *   link_stats,       droppable at any time -- rebuilding them from `events`
 *   device_state      must give exactly what is there now.
 *
 * config_versions is the one table that is neither: the dashboard writes a row
 * when it pushes a config, and `applied_at` is filled in later from the device's
 * own EVT_CONFIG_APPLIED. It is a record of an exchange, not a projection.
 */

import { relations, sql } from "drizzle-orm";
import {
  bigint,
  boolean,
  customType,
  index,
  integer,
  pgEnum,
  pgTable,
  serial,
  smallint,
  text,
  timestamp,
  uniqueIndex,
} from "drizzle-orm/pg-core";

/**
 * Frame payloads are binary and stay binary. CLAUDE.md 1.4: "Payloads are binary
 * and minimal. No JSON, no Protobuf, no text protocol on the air interface" --
 * decoding them into columns here would make this schema a second, divergent
 * definition of the wire format. The decoder lives in the bridge and is shared.
 */
export const bytea = customType<{ data: Uint8Array; driverData: Buffer }>({
  dataType() {
    return "bytea";
  },
  toDriver(value) {
    return Buffer.from(value);
  },
  fromDriver(value) {
    return new Uint8Array(value);
  },
});

export const eventDirection = pgEnum("event_direction", ["rx", "tx"]);

/** Delivery states from CLAUDE.md 2.4, which the UI must keep distinct. */
export const messageState = pgEnum("message_state", [
  "queued",
  "in_flight",
  "delivered",
  "undelivered",
  "dropped",
  "received",
]);

export const devices = pgTable("devices", {
  id: serial("id").primaryKey(),

  /**
   * The 16-bit id from the frame header (CLAUDE.md 2.1), not this table's
   * primary key. Two different numbers for the same thing is a bug waiting to
   * happen, so the wire value is named for what it is everywhere it appears.
   */
  nodeId: integer("node_id").notNull().unique(),

  name: text("name").notNull(),

  /**
   * CLAUDE.md 4.3: "the ingestion endpoint authenticates a device-bridge pair,
   * not a web session". The token itself is shown once when it is generated and
   * never stored -- a dashboard database that can be read is not a dashboard
   * database that can impersonate every node.
   */
  ingestTokenHash: text("ingest_token_hash").notNull(),

  createdAt: timestamp("created_at", { withTimezone: true }).notNull().defaultNow(),
  lastSeenAt: timestamp("last_seen_at", { withTimezone: true }),
});

export const events = pgTable(
  "events",
  {
    id: bigint("id", { mode: "number" }).primaryKey().generatedAlwaysAsIdentity(),
    deviceId: integer("device_id")
      .notNull()
      .references(() => devices.id, { onDelete: "restrict" }),

    /**
     * The journal entry counter -- NOT the frame counter. Both come from the
     * monotonic, persistent, never-reused supply in CLAUDE.md 2.1, but one
     * identifies a transmitted frame and the other an entry in the device's
     * journal. A single frame produces several entries (queued, then delivered
     * or undelivered), and keying this table on the frame counter would make the
     * unique index below discard exactly the state transitions
     * docs/bridge-protocol.md section 4 requires it to carry. The frame counter
     * travels in the body. See docs/decisions/0001-open-decisions.md D10.
     *
     * 32 bits unsigned, so it does not fit a signed 32-bit column.
     */
    journalCounter: bigint("journal_counter", { mode: "number" }).notNull(),

    direction: eventDirection("direction").notNull(),

    /** The bridge protocol opcode (docs/bridge-protocol.md section 4). */
    opcode: smallint("opcode").notNull(),

    /** Undecoded event body, exactly as it left the device. */
    body: bytea("body").notNull(),

    /** The device's own clock when it journalled the event, where it had one. */
    deviceTime: timestamp("device_time", { withTimezone: true }),

    /** When the phone received it from the node. */
    receivedAt: timestamp("received_at", { withTimezone: true }).notNull(),

    /** When this server stored it. Never equal to received_at, often much later. */
    ingestedAt: timestamp("ingested_at", { withTimezone: true }).notNull().defaultNow(),
  },
  (table) => [
    /*
     * CLAUDE.md 4.3: "events carries UNIQUE (device_id, journal_counter,
     * direction). The bridge re-sends on reconnect and would otherwise duplicate
     * rows." (The quote once said frame_counter; D10 renamed the key, and this
     * copy lagged the spec by a day.)
     *
     * This is also what lets docs/bridge-protocol.md section 3 be safe: the phone
     * acknowledges the device's journal only after the server has the data, so
     * erring towards re-sending is always right -- and it is right because this
     * index makes a re-send free.
     */
    uniqueIndex("events_device_counter_direction").on(
      table.deviceId,
      table.journalCounter,
      table.direction,
    ),
    index("events_device_received").on(table.deviceId, table.receivedAt),
    index("events_opcode").on(table.opcode),
  ],
);

/**
 * Projection. Rebuildable from `events` at any time -- see lib/projections.ts.
 * A row here is a message and its current delivery state; the transitions that
 * produced it stay in the log.
 */
export const messages = pgTable(
  "messages",
  {
    id: serial("id").primaryKey(),
    deviceId: integer("device_id")
      .notNull()
      .references(() => devices.id, { onDelete: "cascade" }),
    peerNodeId: integer("peer_node_id"),
    direction: eventDirection("direction").notNull(),

    /** The counter of the *frame*, read from the event body -- so the queued
     *  entry and the delivered entry fold into one row (D10). */
    frameCounter: bigint("frame_counter", { mode: "number" }).notNull(),
    seq: smallint("seq"),

    text: text("text"),
    state: messageState("state").notNull(),

    /**
     * docs/bridge-protocol.md section 4: state 2 (queued) "may be followed later
     * by 0 or 1 for the same counter -- the phone and the server must treat the
     * journal as a log of state transitions, not as a set of final outcomes".
     */
    attempts: smallint("attempts"),
    rssi: smallint("rssi"),
    snr: smallint("snr"),

    /** Set only while the budget is holding the frame back (CLAUDE.md 2.4). */
    queuedUntil: timestamp("queued_until", { withTimezone: true }),

    occurredAt: timestamp("occurred_at", { withTimezone: true }).notNull(),
    stateChangedAt: timestamp("state_changed_at", { withTimezone: true }).notNull(),
  },
  (table) => [
    uniqueIndex("messages_device_counter_direction").on(
      table.deviceId,
      table.frameCounter,
      table.direction,
    ),
    index("messages_device_time").on(table.deviceId, table.occurredAt),
  ],
);

/** Projection: per-link radio quality over time (CLAUDE.md 4.3). */
export const linkStats = pgTable(
  "link_stats",
  {
    id: serial("id").primaryKey(),
    deviceId: integer("device_id")
      .notNull()
      .references(() => devices.id, { onDelete: "cascade" }),
    peerNodeId: integer("peer_node_id").notNull(),
    frameCounter: bigint("frame_counter", { mode: "number" }).notNull(),
    rssi: smallint("rssi"),
    snr: smallint("snr"),
    sf: smallint("sf"),
    occurredAt: timestamp("occurred_at", { withTimezone: true }).notNull(),
  },
  (table) => [
    uniqueIndex("link_stats_device_counter").on(table.deviceId, table.frameCounter),
    index("link_stats_device_time").on(table.deviceId, table.occurredAt),
  ],
);

/**
 * Projection: the newest of everything, so the node list does not need a
 * window function over the whole log on every page load.
 */
export const deviceState = pgTable("device_state", {
  deviceId: integer("device_id")
    .primaryKey()
    .references(() => devices.id, { onDelete: "cascade" }),

  lastEventAt: timestamp("last_event_at", { withTimezone: true }),
  highestCounter: bigint("highest_counter", { mode: "number" }),

  latDeg7: integer("lat_deg7"),
  lonDeg7: integer("lon_deg7"),
  altM: smallint("alt_m"),
  hdop: smallint("hdop"),
  positionAt: timestamp("position_at", { withTimezone: true }),

  tempC100: integer("temp_c100"),
  humidity100: integer("humidity_100"),
  pressurePa: integer("pressure_pa"),
  batteryMv: integer("battery_mv"),
  uptimeS: bigint("uptime_s", { mode: "number" }),
  telemetryAt: timestamp("telemetry_at", { withTimezone: true }),

  /** From EVT_BUDGET. CLAUDE.md 1.2 -- reported, never enforced here. */
  budgetBand: smallint("budget_band"),
  budgetUsedMs: integer("budget_used_ms"),
  budgetLimitMs: integer("budget_limit_ms"),
  budgetNextTxAt: timestamp("budget_next_tx_at", { withTimezone: true }),
  budgetAt: timestamp("budget_at", { withTimezone: true }),

  queueDepth: smallint("queue_depth"),
  /** CLAUDE.md 1.2: no valid time means transmit-blocked, and the dashboard has
   *  to distinguish that from a node that simply has nothing to say. */
  timeValid: boolean("time_valid"),
  keyProvisioned: boolean("key_provisioned"),
});

/**
 * Projection: the reporting device's own numbers over time.
 *
 * device_state below holds only the newest of everything, which is what a node
 * list needs and what a chart cannot use. This is the same data as a series --
 * one row per EVT_STATUS or EVT_BUDGET, in the order the device wrote them.
 *
 * It is a projection like the others: dropped and rebuilt from `events` on every
 * ingest, never edited.
 */
export const deviceSamples = pgTable(
  "device_samples",
  {
    id: serial("id").primaryKey(),
    deviceId: integer("device_id")
      .notNull()
      .references(() => devices.id, { onDelete: "cascade" }),
    journalCounter: bigint("journal_counter", { mode: "number" }).notNull(),

    batteryMv: integer("battery_mv"),
    uptimeS: bigint("uptime_s", { mode: "number" }),
    queueDepth: smallint("queue_depth"),

    budgetBand: smallint("budget_band"),
    budgetUsedMs: integer("budget_used_ms"),
    budgetLimitMs: integer("budget_limit_ms"),

    occurredAt: timestamp("occurred_at", { withTimezone: true }).notNull(),
  },
  (table) => [
    uniqueIndex("device_samples_device_counter").on(table.deviceId, table.journalCounter),
    index("device_samples_device_time").on(table.deviceId, table.occurredAt),
  ],
);

/**
 * Projection: what one device heard *about another node*.
 *
 * device_state above is built only from a device's own events -- its EVT_STATUS,
 * EVT_BUDGET and EVT_FIX. That keeps every field single-sourced, but it also
 * means a node whose phone has not synced for a day is simply blank on the
 * dashboard, even when its peer has been hearing it the whole time.
 *
 * So the frames a device received are projected here too, attributed to the node
 * that sent them. The UI can then say "last known position of B, heard by A at
 * 14:12" instead of nothing at all -- which for a device carried into places
 * where phones die is the normal case, not the exception.
 */
export const peerObservations = pgTable(
  "peer_observations",
  {
    id: serial("id").primaryKey(),

    /** The device that did the hearing. */
    observerDeviceId: integer("observer_device_id")
      .notNull()
      .references(() => devices.id, { onDelete: "cascade" }),

    /** The 16-bit node id in the frame's src field -- which may be a node this
     *  server has never been told about. Deliberately not a foreign key. */
    peerNodeId: integer("peer_node_id").notNull(),

    frameCounter: bigint("frame_counter", { mode: "number" }).notNull(),
    frameType: smallint("frame_type").notNull(),

    latDeg7: integer("lat_deg7"),
    lonDeg7: integer("lon_deg7"),
    altM: smallint("alt_m"),
    hdop: smallint("hdop"),

    tempC100: integer("temp_c100"),
    humidity100: integer("humidity_100"),
    pressurePa: integer("pressure_pa"),
    batteryMv: integer("battery_mv"),
    uptimeS: bigint("uptime_s", { mode: "number" }),

    occurredAt: timestamp("occurred_at", { withTimezone: true }).notNull(),
  },
  (table) => [
    uniqueIndex("peer_observations_observer_counter").on(
      table.observerDeviceId,
      table.frameCounter,
    ),
    index("peer_observations_peer_time").on(table.peerNodeId, table.occurredAt),
  ],
);

/**
 * Not a projection. CLAUDE.md 4.3: "config_versions carries applied_at, set from
 * a device acknowledgement -- a pushed config is not an applied config."
 */
export const configVersions = pgTable(
  "config_versions",
  {
    id: serial("id").primaryKey(),
    deviceId: integer("device_id")
      .notNull()
      .references(() => devices.id, { onDelete: "cascade" }),
    configVersion: bigint("config_version", { mode: "number" }).notNull(),

    /** The TLV blob as it was sent (docs/bridge-protocol.md, Config TLVs). */
    tlvs: bytea("tlvs").notNull(),

    pushedAt: timestamp("pushed_at", { withTimezone: true }).notNull().defaultNow(),

    /** Null until EVT_CONFIG_APPLIED arrives. Never set by the push path. */
    appliedAt: timestamp("applied_at", { withTimezone: true }),
    appliedMask: bigint("applied_mask", { mode: "number" }),

    /**
     * "Unknown types are ignored and reported back in EVT_CONFIG_APPLIED as
     * unapplied, rather than failing the whole write." Kept so the UI can say
     * which settings the node did not understand.
     */
    unappliedTypes: bytea("unapplied_types"),
  },
  (table) => [
    uniqueIndex("config_versions_device_version").on(table.deviceId, table.configVersion),
  ],
);

export const devicesRelations = relations(devices, ({ many, one }) => ({
  events: many(events),
  messages: many(messages),
  state: one(deviceState, { fields: [devices.id], references: [deviceState.deviceId] }),
}));

export const eventsRelations = relations(events, ({ one }) => ({
  device: one(devices, { fields: [events.deviceId], references: [devices.id] }),
}));

export type Device = typeof devices.$inferSelect;
export type EventRow = typeof events.$inferSelect;
export type NewEventRow = typeof events.$inferInsert;
export type MessageRow = typeof messages.$inferSelect;
export type LinkStatRow = typeof linkStats.$inferSelect;
export type DeviceStateRow = typeof deviceState.$inferSelect;
export type PeerObservationRow = typeof peerObservations.$inferSelect;
export type DeviceSampleRow = typeof deviceSamples.$inferSelect;

/** Used by the tests to prove a rebuilt projection equals the stored one. */
export const projectionTables = [messages, linkStats, peerObservations, deviceSamples, deviceState] as const;

export const schemaVersionSentinel = sql`1`;
