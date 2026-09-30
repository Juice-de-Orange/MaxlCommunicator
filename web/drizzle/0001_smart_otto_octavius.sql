CREATE TABLE "device_samples" (
	"id" serial PRIMARY KEY NOT NULL,
	"device_id" integer NOT NULL,
	"journal_counter" bigint NOT NULL,
	"battery_mv" integer,
	"uptime_s" bigint,
	"queue_depth" smallint,
	"budget_band" smallint,
	"budget_used_ms" integer,
	"budget_limit_ms" integer,
	"occurred_at" timestamp with time zone NOT NULL
);
--> statement-breakpoint
ALTER TABLE "device_samples" ADD CONSTRAINT "device_samples_device_id_devices_id_fk" FOREIGN KEY ("device_id") REFERENCES "public"."devices"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
CREATE UNIQUE INDEX "device_samples_device_counter" ON "device_samples" USING btree ("device_id","journal_counter");--> statement-breakpoint
CREATE INDEX "device_samples_device_time" ON "device_samples" USING btree ("device_id","occurred_at");