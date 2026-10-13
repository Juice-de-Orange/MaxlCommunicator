# Deploying the dashboard and the PWA

The dashboard (`web/`) and the phone bridge (`bridge/`, a PWA) are one container: the PWA is
served as static assets under `/app/` from the dashboard's own origin. That is required, not a
convenience — **Web Bluetooth only works in a secure context**, so a phone can only talk to a node
from an HTTPS page (or `http://localhost`), and splitting the two across origins would need a
second certificate and a cross-origin ingest call for no gain.

## 1. Configure

```bash
cd web
cp .env.example .env
```

Set at least:

| Variable | Why |
|---|---|
| `POSTGRES_PASSWORD` | The database password; compose refuses to start without it. Use the same value in `DATABASE_URL` if you run tools from the host. |
| `DASHBOARD_PASSWORD` | The single dashboard login (`openssl rand -base64 24`). |
| `SESSION_SECRET` | Signs the session cookie (`openssl rand -hex 32`). |
| `DASHBOARD_HOST` | The public host name the reverse proxy of step 3 serves, e.g. `trailtalk.example.com` (with the port if it is not 443: `trailtalk.example.com:8443`). **Required behind a TLS proxy** — without it the login form answers 403. Leave it empty if the dashboard is only ever opened on the host itself, as `http://localhost:<WEB_PORT>` or `http://127.0.0.1:<WEB_PORT>`. |

`WEB_PORT` (default 4321) and `POSTGRES_PORT` (default 5433) are the loopback ports on the host.
While `DASHBOARD_HOST` is empty, `WEB_PORT` is also the port the login form accepts, and like
`DASHBOARD_HOST` it is built into the server: after changing it, `docker compose up -d --build`
(a container started with another `WEB_PORT` than its image was built for says so and exits).

`DATABASE_URL` in `.env` is only read by tools run from the host (`npm run db:migrate`, the
`device:*` scripts); the container gets its own from compose. The test suite does not run in that
database: `npm test` uses a separate one with `_test` appended to the name and refuses to empty
any other, so running the tests next to a production `.env` leaves the event log alone.

## 2. Start

```bash
docker compose up -d --build
docker compose logs -f web      # "applying migrations", then "starting server"
```

The container applies database migrations on every start before it serves, so there is no manual
migration step. Both ports are published on `127.0.0.1` only.

## 3. Put TLS in front of it

Any reverse proxy that terminates TLS with a publicly trusted certificate works. With Caddy:

```caddyfile
trailtalk.example.com {
    reverse_proxy 127.0.0.1:4321
}
```

With nginx (certificate from your ACME client of choice):

```nginx
server {
    listen 443 ssl;
    http2 on;
    server_name trailtalk.example.com;

    ssl_certificate     /etc/letsencrypt/live/trailtalk.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/trailtalk.example.com/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:4321;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto https;
    }
}
```

The proxy talks to the container over plain HTTP, so the server has to be told which public
origin it is behind: that is `DASHBOARD_HOST` from step 1. The login and logout forms are protected
against cross-site submission by comparing the browser's `Origin` header with the address of the
request, and the server believes the proxy's `X-Forwarded-Proto` / `X-Forwarded-Host` only for
that one host. Both examples above send what is needed (Caddy adds the headers by itself). The
value is compiled into the server when the image is built, so after changing it:

```bash
docker compose up -d --build
```

A container started with a `DASHBOARD_HOST` other than the one its image was built for says so in
its log and exits rather than serving a login that cannot work. `http://localhost:4321` keeps
working with the variable set.

The session cookie is marked `Secure`, so logging in over plain HTTP from another machine does not
work — use the HTTPS name, or `http://localhost:4321` on the host itself (for example through an
SSH tunnel), which browsers treat as a secure context too.

## 4. Register the nodes

Each node's uploads are authenticated with its own ingest token (only the hash is stored):

```bash
docker compose exec web ./node_modules/.bin/tsx scripts/device.ts add --node-id 1 --name "T-Echo A"
docker compose exec web ./node_modules/.bin/tsx scripts/device.ts list
```

Enter the printed token in the PWA (open `https://trailtalk.example.com/app/` on the phone) or the
Android client when you pair a node.

## 5. Backups

All state is in the PostgreSQL volume; the event log is append-only, so a dump is a complete
history:

```bash
docker compose exec -T postgres sh -c 'pg_dump -U "$POSTGRES_USER" -d "$POSTGRES_DB" --clean --if-exists' > trailtalk-$(date +%F).sql
```

To restore, stop the dashboard, feed the dump back in and start it again. The dump was written
with `--clean --if-exists`, so it replaces the tables that are there — on a fresh installation run
`docker compose up -d` once first, so that the database exists:

```bash
docker compose stop web
docker compose exec -T postgres sh -c 'psql -U "$POSTGRES_USER" -d "$POSTGRES_DB" -v ON_ERROR_STOP=1' < trailtalk-2026-10-03.sql
docker compose start web
```

Both commands take the role and the database from the container's own `POSTGRES_USER` and
`POSTGRES_DB`, so they are the same for every installation, including one from before the rename.

## Updating

```bash
git pull
docker compose up -d --build
```

Migrations run on start. The protocol versioning rules — what has to be updated together — are in
`docs/versioning-and-updates.md`.

### Updating from MaxlCommunicator

The project was called MaxlCommunicator until its first release. An installation from before the
rename updates exactly as above and keeps its data without any extra step:

- Its `.env` has no `COMPOSE_PROJECT_NAME`, so compose still runs it as the project `maxl` — the
  same containers, the same data volume `maxl_pgdata`, the same role and database `maxl`. The web
  container says so in its log on every start (`note: running as compose project 'maxl' …`). That
  is the intended state, not a fault.
- **Do not** add `COMPOSE_PROJECT_NAME=trailtalk` to that `.env`, and do not copy the new
  `.env.example` over it: compose would start a second, empty database next to the old volume, and
  the dashboard would show no nodes and no history. Your data would still be in `maxl_pgdata`.
- Ingest tokens, the dashboard password and the session cookie stay valid. The PWA keeps its
  pairing and every event it still holds, because its storage names did not change.
- Nodes need no reflash. Images from before the rename advertise as `Maxl-XXXX`, new ones as
  `trailtalk-XXXX`; the PWA and the Android client find both by the service UUID.
- The checkout's `origin` may still say `…/MaxlCommunicator.git`. GitHub redirects it; to use the
  new name: `git remote set-url origin https://github.com/Juice-de-Orange/trailtalk.git`.

Moving the installation to the new names is optional. It is a backup into a new project and a
restore, and the old volume stays untouched until you remove it yourself:

```bash
docker compose exec -T postgres sh -c 'pg_dump -U "$POSTGRES_USER" -d "$POSTGRES_DB" --clean --if-exists --no-owner' > before-rename.sql
docker compose down        # stops the project `maxl`; the volume maxl_pgdata stays
# in .env: COMPOSE_PROJECT_NAME=trailtalk, POSTGRES_USER=trailtalk, POSTGRES_DB=trailtalk,
#          and the same role and database in DATABASE_URL
docker compose up -d --build
docker compose stop web
docker compose exec -T postgres sh -c 'psql -U "$POSTGRES_USER" -d "$POSTGRES_DB" -v ON_ERROR_STOP=1' < before-rename.sql
docker compose start web
```

`--no-owner` because the tables move from the role `maxl` to the role `trailtalk`. Check the
dashboard and `device:list` (section 4) before you remove the old volume and image with
`docker volume rm maxl_pgdata` and `docker image rm maxl-web`.
