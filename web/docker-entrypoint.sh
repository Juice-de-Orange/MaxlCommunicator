#!/bin/sh
# Migrate, then serve.
#
# A stack that needs somebody to run a migration by hand before it works is a
# stack that gets deployed broken, and the failure then looks like a bug in the
# application rather than a step nobody did.
#
# tsx rather than node's --experimental-strip-types: the migration imports its
# neighbours without file extensions, and type stripping requires full
# specifiers. Rewriting working source to suit the runner would be the tail
# wagging the dog.
set -e

# DASHBOARD_HOST is compiled into the server (see the Dockerfile). Changing it in
# .env and restarting without a rebuild would leave the old host in force, and
# the symptom is a login form that answers 403 -- so say it here instead.
if [ -n "${DASHBOARD_HOST:-}" ] && [ "${DASHBOARD_HOST}" != "${MAXL_BUILT_FOR_HOST:-}" ]; then
  echo "DASHBOARD_HOST is '${DASHBOARD_HOST}', but this image was built for '${MAXL_BUILT_FOR_HOST:-}'." >&2
  echo "Rebuild it: docker compose up -d --build" >&2
  exit 1
fi

echo "applying migrations"
./node_modules/.bin/tsx src/db/migrate.ts

echo "starting server on ${HOST}:${PORT}"
exec node dist/server/entry.mjs
