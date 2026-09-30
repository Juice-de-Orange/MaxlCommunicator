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

`WEB_PORT` (default 4321) and `POSTGRES_PORT` (default 5433) are the loopback ports on the host.

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
maxl.example.com {
    reverse_proxy 127.0.0.1:4321
}
```

With nginx (certificate from your ACME client of choice):

```nginx
server {
    listen 443 ssl;
    http2 on;
    server_name maxl.example.com;

    ssl_certificate     /etc/letsencrypt/live/maxl.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/maxl.example.com/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:4321;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto https;
    }
}
```

The session cookie is marked `Secure`, so logging in over plain HTTP from another machine does not
work — use the HTTPS name, or `http://localhost:4321` on the host itself (for example through an
SSH tunnel), which browsers treat as a secure context too.

## 4. Register the nodes

Each node's uploads are authenticated with its own ingest token (only the hash is stored):

```bash
docker compose exec web ./node_modules/.bin/tsx scripts/device.ts add --node-id 1 --name "T-Echo A"
docker compose exec web ./node_modules/.bin/tsx scripts/device.ts list
```

Enter the printed token in the PWA (open `https://maxl.example.com/app/` on the phone) or the
Android client when you pair a node.

## 5. Backups

All state is in the PostgreSQL volume; the event log is append-only, so a dump is a complete
history:

```bash
docker compose exec -T postgres pg_dump -U maxl -d maxl --clean --if-exists > maxl-$(date +%F).sql
```

## Updating

```bash
git pull
docker compose up -d --build
```

Migrations run on start. The protocol versioning rules — what has to be updated together — are in
`docs/versioning-and-updates.md`.
