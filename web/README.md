# web/ — management dashboard (phase 7)

Built and tested; all four gates of phase 7 in `docs/test-plan.md` have passed
(`docs/test-results/2026-08-31_phase7_dashboard_gates.md`). The same origin also serves the PWA
bridge from `bridge/`, because Web Bluetooth needs a secure context. How to run it publicly:
[`docs/DEPLOYMENT.md`](../docs/DEPLOYMENT.md).

## Stack — pinned, not floating

- **Astro 7 SSR + React 19 islands + Tailwind v4 + drizzle-orm 0.45.x + PostgreSQL 17**
- **Node 22 LTS** (Astro 7 requires ≥ 22)
- Tailwind v4 is CSS-first: Vite plugin, **no** `tailwind.config.js`
- Astro 7 removed the CLI commands `astro db`, `login`, `link` and `init`
- drizzle-orm deliberately on 0.45.x — the 1.0 line is still in beta
- `bridge/` is served as static assets from the **same origin** (`/app/`)

## Data model

An append-only event log; **all** state is a projection, nothing is updated in place. Core
tables: `devices`, `events`, `messages`, `link_stats`, `config_versions`.

The idempotency key is

```sql
UNIQUE (device_id, journal_counter, direction)
```

— **`journal_counter`, not `frame_counter`.** This is not a naming detail:
`docs/bridge-protocol.md` §4 requires several `EVT_FRAME_TX_RESULT` entries for the same frame
(`queued`, then `delivered` or `undelivered`). Keyed on the frame counter, idempotency would throw
away exactly the state transitions it is meant to carry. The frame counter travels in the body,
which is why `EVT_FRAME_TX_RESULT` has a `counter` field at all. See decision **D10** in
`docs/decisions/0001-open-decisions.md`.

`config_versions.applied_at` is set only from a device acknowledgement — a pushed configuration
is not an applied one.

## What the dashboard shows

Node list with last position, telemetry charts, message log with delivery state, radio figures
per link, duty-cycle budget consumption over time, configuration push — and the radio reference
page with the sniff-interval table that `CLAUDE.md` §2.3 explicitly asks for (*"The dashboard must
show this table, not a vague note"*).

## Running it locally

```bash
cd web
cp .env.example .env   # once; set POSTGRES_PASSWORD (twice), DASHBOARD_PASSWORD, SESSION_SECRET
npm ci
npm run db:up          # PostgreSQL 17 in a container, on 127.0.0.1:5433
npm run db:migrate     # create the schema — the tests do not
npm test               # against the real database -- it empties the tables, before and after
npm run build:all      # PWA + dashboard
npm run dev            # http://localhost:4321
```

`npm run dev`, `db:migrate`, the `device:*` scripts and the tests read `web/.env` themselves; a
variable that is already set in the environment wins. `WEB_PORT` is the published port of the
container deployment and does not apply to `npm run dev`: that is Astro's 4321 unless you pass
one, `npm run dev -- --port 4400`.

Nodes authenticate their uploads with a per-device ingest token:

```bash
npm run device:add -- --node-id 1 --name "T-Echo A"   # prints the token once; only its hash is stored
npm run device:list
npm run device:rotate -- --node-id 1
```

`npm run seed` pushes a plausible, synthetic day of events through the real ingest endpoint — a
first look without hardware. It needs `npm run dev` running and the tokens of two registered
nodes in `SEED_TOKEN_A` / `SEED_TOKEN_B` (`SEED_URL` overrides the address).

The **container build is stricter than the local one**: `astro check` also checks test files, and
a fresh container has no `.astro` cache. A green local build is not a green container build.

## What is open

- **Outgoing messages have no text on the server.** `EVT_FRAME_TX_RESULT` carries counters and
  radio figures, no content. The text is in the receiver's `EVT_FRAME_RX` and could be matched via
  `(src, counter)` once both nodes sync. Not solved, deliberately not invented.
