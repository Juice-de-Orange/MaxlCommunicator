/*
 * The one interactive thing on the dashboard, and therefore the only React
 * island: pushing a configuration to a node.
 *
 * Everything else is a server render over a database, and a framework that only
 * ever renders once has nothing to contribute there.
 *
 * The wording is careful on one point throughout. This queues a config; it does
 * not apply one. CLAUDE.md 4.3: "a pushed config is not an applied config", and
 * a UI that said "saved" would be telling the operator something the system
 * cannot know until the node next comes into range and acknowledges.
 */

import { useState } from "react";

import { CONFIG_FIELDS, type ConfigSettings } from "../lib/config-tlv";

interface Props {
  nodeId: number;
  current: ConfigSettings;
  lastPushedVersion: number | null;
  lastAppliedVersion: number | null;
}

type Status =
  | { kind: "idle" }
  | { kind: "sending" }
  | { kind: "queued"; version: number }
  | { kind: "error"; message: string };

export default function ConfigPush({
  nodeId,
  current,
  lastPushedVersion,
  lastAppliedVersion,
}: Props) {
  const [values, setValues] = useState<Record<string, string>>(() =>
    Object.fromEntries(
      CONFIG_FIELDS.map((field) => [
        field.key,
        current[field.key] === undefined ? "" : String(current[field.key]),
      ]),
    ),
  );
  const [status, setStatus] = useState<Status>({ kind: "idle" });

  const dirty = CONFIG_FIELDS.some((field) => {
    const typed = values[field.key];
    if (typed === undefined || typed === "") return false;
    return Number(typed) !== current[field.key];
  });

  async function push() {
    setStatus({ kind: "sending" });
    const settings: ConfigSettings = {};
    for (const field of CONFIG_FIELDS) {
      const typed = values[field.key];
      if (typed === undefined || typed === "") continue;
      const parsed = Number(typed);
      if (!Number.isFinite(parsed)) continue;
      // Only what actually changed. Sending the whole set every time would make
      // every push look like a change to everything in the node's own log.
      if (parsed !== current[field.key]) {
        (settings as Record<string, number>)[field.key] = parsed;
      }
    }

    try {
      const response = await fetch("/api/config", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ nodeId, settings }),
      });
      const body = await response.json();
      if (!response.ok) {
        setStatus({ kind: "error", message: body.detail ?? body.error ?? "unknown error" });
        return;
      }
      setStatus({ kind: "queued", version: body.configVersion });
    } catch (error) {
      setStatus({
        kind: "error",
        message: error instanceof Error ? error.message : String(error),
      });
    }
  }

  const awaiting =
    lastPushedVersion !== null && lastPushedVersion !== lastAppliedVersion;

  return (
    <div className="rounded-lg border border-hairline bg-surface p-5">
      {awaiting && (
        <p className="mb-4 rounded border border-[color:var(--color-warning)] px-3 py-2 text-xs text-ink-secondary">
          Version {lastPushedVersion} is waiting for the node to acknowledge it. It is delivered
          on the next sync — until then the device keeps running the old one.
        </p>
      )}

      <div className="grid gap-4 sm:grid-cols-2">
        {CONFIG_FIELDS.map((field) => (
          <label key={field.key} className="block text-sm">
            <span className="text-ink-secondary">{field.label}</span>
            {field.options ? (
              <select
                className="mt-1 w-full rounded border border-hairline bg-plane px-2 py-1.5"
                value={values[field.key] ?? ""}
                onChange={(event) =>
                  setValues({ ...values, [field.key]: event.target.value })
                }
              >
                <option value="">unchanged</option>
                {field.options.map((option) => (
                  <option key={option.value} value={String(option.value)}>
                    {option.label}
                  </option>
                ))}
              </select>
            ) : (
              <input
                type="number"
                min={field.min}
                max={field.max}
                className="tabular mt-1 w-full rounded border border-hairline bg-plane px-2 py-1.5"
                value={values[field.key] ?? ""}
                onChange={(event) =>
                  setValues({ ...values, [field.key]: event.target.value })
                }
              />
            )}
            <span className="mt-1 block text-xs text-ink-muted">{field.hint}</span>
          </label>
        ))}
      </div>

      <div className="mt-5 flex flex-wrap items-center gap-3">
        <button
          type="button"
          onClick={push}
          disabled={!dirty || status.kind === "sending"}
          className="rounded bg-[color:var(--color-series-1)] px-3 py-2 text-sm font-medium text-white disabled:opacity-50"
        >
          {status.kind === "sending" ? "queueing…" : "Queue configuration"}
        </button>

        {status.kind === "queued" && (
          <span className="text-sm text-[color:var(--color-good)]">
            Version {status.version} queued — not applied yet.
          </span>
        )}
        {status.kind === "error" && (
          <span className="text-sm text-[color:var(--color-critical)]">{status.message}</span>
        )}
        {!dirty && status.kind === "idle" && (
          <span className="text-sm text-ink-muted">Nothing changed.</span>
        )}
      </div>

      <p className="mt-4 text-xs text-ink-muted">
        There is deliberately no setting for the duty cycle. It is not configurable, in
        firmware or over the air.
      </p>
    </div>
  );
}
