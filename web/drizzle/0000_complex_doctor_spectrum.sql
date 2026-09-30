CREATE TYPE "public"."event_direction" AS ENUM('rx', 'tx');--> statement-breakpoint
CREATE TYPE "public"."message_state" AS ENUM('queued', 'in_flight', 'delivered', 'undelivered', 'dropped', 'received');--> statement-breakpoint
CREATE TABLE "config_versions" (
	"id" serial PRIMARY KEY NOT NULL,
	"device_id" integer NOT NULL,
	"config_version" bigint NOT NULL,
	"tlvs" "bytea" NOT NULL,
	"pushed_at" timestamp with time zone DEFAULT now() NOT NULL,
	"applied_at" timestamp with time zone,
	"applied_mask" bigint,
	"unapplied_types" "bytea"
);
--> statement-breakpoint
CREATE TABLE "device_state" (
	"device_id" integer PRIMARY KEY NOT NULL,
	"last_event_at" timestamp with time zone,
	"highest_counter" bigint,
	"lat_deg7" integer,
	"lon_deg7" integer,
	"alt_m" smallint,
	"hdop" smallint,
	"position_at" timestamp with time zone,
	"temp_c100" integer,
	"humidity_100" integer,
	"pressure_pa" integer,
	"battery_mv" integer,
	"uptime_s" bigint,
	"telemetry_at" timestamp with time zone,
	"budget_band" smallint,
	"budget_used_ms" integer,
	"budget_limit_ms" integer,
	"budget_next_tx_at" timestamp with time zone,
	"budget_at" timestamp with time zone,
	"queue_depth" smallint,
	"time_valid" boolean,
	"key_provisioned" boolean
);
--> statement-breakpoint
CREATE TABLE "devices" (
	"id" serial PRIMARY KEY NOT NULL,
	"node_id" integer NOT NULL,
	"name" text NOT NULL,
	"ingest_token_hash" text NOT NULL,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	"last_seen_at" timestamp with time zone,
	CONSTRAINT "devices_node_id_unique" UNIQUE("node_id")
);
--> statement-breakpoint
CREATE TABLE "events" (
	"id" bigint PRIMARY KEY GENERATED ALWAYS AS IDENTITY (sequence name "events_id_seq" INCREMENT BY 1 MINVALUE 1 MAXVALUE 9223372036854775807 START WITH 1 CACHE 1),
	"device_id" integer NOT NULL,
	"journal_counter" bigint NOT NULL,
	"direction" "event_direction" NOT NULL,
	"opcode" smallint NOT NULL,
	"body" "bytea" NOT NULL,
	"device_time" timestamp with time zone,
	"received_at" timestamp with time zone NOT NULL,
	"ingested_at" timestamp with time zone DEFAULT now() NOT NULL
);
--> statement-breakpoint
CREATE TABLE "link_stats" (
	"id" serial PRIMARY KEY NOT NULL,
	"device_id" integer NOT NULL,
	"peer_node_id" integer NOT NULL,
	"frame_counter" bigint NOT NULL,
	"rssi" smallint,
	"snr" smallint,
	"sf" smallint,
	"occurred_at" timestamp with time zone NOT NULL
);
--> statement-breakpoint
CREATE TABLE "messages" (
	"id" serial PRIMARY KEY NOT NULL,
	"device_id" integer NOT NULL,
	"peer_node_id" integer,
	"direction" "event_direction" NOT NULL,
	"frame_counter" bigint NOT NULL,
	"seq" smallint,
	"text" text,
	"state" "message_state" NOT NULL,
	"attempts" smallint,
	"rssi" smallint,
	"snr" smallint,
	"queued_until" timestamp with time zone,
	"occurred_at" timestamp with time zone NOT NULL,
	"state_changed_at" timestamp with time zone NOT NULL
);
--> statement-breakpoint
CREATE TABLE "peer_observations" (
	"id" serial PRIMARY KEY NOT NULL,
	"observer_device_id" integer NOT NULL,
	"peer_node_id" integer NOT NULL,
	"frame_counter" bigint NOT NULL,
	"frame_type" smallint NOT NULL,
	"lat_deg7" integer,
	"lon_deg7" integer,
	"alt_m" smallint,
	"hdop" smallint,
	"temp_c100" integer,
	"humidity_100" integer,
	"pressure_pa" integer,
	"battery_mv" integer,
	"uptime_s" bigint,
	"occurred_at" timestamp with time zone NOT NULL
);
--> statement-breakpoint
ALTER TABLE "config_versions" ADD CONSTRAINT "config_versions_device_id_devices_id_fk" FOREIGN KEY ("device_id") REFERENCES "public"."devices"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "device_state" ADD CONSTRAINT "device_state_device_id_devices_id_fk" FOREIGN KEY ("device_id") REFERENCES "public"."devices"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "events" ADD CONSTRAINT "events_device_id_devices_id_fk" FOREIGN KEY ("device_id") REFERENCES "public"."devices"("id") ON DELETE restrict ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "link_stats" ADD CONSTRAINT "link_stats_device_id_devices_id_fk" FOREIGN KEY ("device_id") REFERENCES "public"."devices"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "messages" ADD CONSTRAINT "messages_device_id_devices_id_fk" FOREIGN KEY ("device_id") REFERENCES "public"."devices"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "peer_observations" ADD CONSTRAINT "peer_observations_observer_device_id_devices_id_fk" FOREIGN KEY ("observer_device_id") REFERENCES "public"."devices"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
CREATE UNIQUE INDEX "config_versions_device_version" ON "config_versions" USING btree ("device_id","config_version");--> statement-breakpoint
CREATE UNIQUE INDEX "events_device_counter_direction" ON "events" USING btree ("device_id","journal_counter","direction");--> statement-breakpoint
CREATE INDEX "events_device_received" ON "events" USING btree ("device_id","received_at");--> statement-breakpoint
CREATE INDEX "events_opcode" ON "events" USING btree ("opcode");--> statement-breakpoint
CREATE UNIQUE INDEX "link_stats_device_counter" ON "link_stats" USING btree ("device_id","frame_counter");--> statement-breakpoint
CREATE INDEX "link_stats_device_time" ON "link_stats" USING btree ("device_id","occurred_at");--> statement-breakpoint
CREATE UNIQUE INDEX "messages_device_counter_direction" ON "messages" USING btree ("device_id","frame_counter","direction");--> statement-breakpoint
CREATE INDEX "messages_device_time" ON "messages" USING btree ("device_id","occurred_at");--> statement-breakpoint
CREATE UNIQUE INDEX "peer_observations_observer_counter" ON "peer_observations" USING btree ("observer_device_id","frame_counter");--> statement-breakpoint
CREATE INDEX "peer_observations_peer_time" ON "peer_observations" USING btree ("peer_node_id","occurred_at");