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
if [ -n "${DASHBOARD_HOST:-}" ] && [ "${DASHBOARD_HOST}" != "${TRAILTALK_BUILT_FOR_HOST:-}" ]; then
  echo "DASHBOARD_HOST is '${DASHBOARD_HOST}', but this image was built for '${TRAILTALK_BUILT_FOR_HOST:-}'." >&2
  echo "Rebuild it: docker compose up -d --build" >&2
  exit 1
fi

# The same for WEB_PORT while DASHBOARD_HOST is empty: the published port is the
# origin the login form accepts, and it is compiled in. An image built without
# one (plain `docker build`) was built for 4321.
if [ -z "${DASHBOARD_HOST:-}" ] && [ -n "${WEB_PORT:-}" ] && [ "${WEB_PORT}" != "${TRAILTALK_BUILT_FOR_WEB_PORT:-4321}" ]; then
  echo "WEB_PORT is '${WEB_PORT}', but this image was built for '${TRAILTALK_BUILT_FOR_WEB_PORT:-4321}'." >&2
  echo "Rebuild it: docker compose up -d --build" >&2
  exit 1
fi

# The compose project name falls back to `maxl`, the name from before the rename
# to trailtalk, so that an installation whose .env predates it keeps its data
# volume (docker-compose.yml). Nothing is wrong; say it once per start so it is
# a choice rather than an accident.
if [ "${COMPOSE_PROJECT_NAME:-}" = "maxl" ]; then
  echo "note: running as compose project 'maxl', the name from before the rename to trailtalk;" >&2
  echo "      the database stays in volume maxl_pgdata. Nothing to do -- docs/DEPLOYMENT.md" >&2
  echo "      'Updating from MaxlCommunicator' explains this and how to move if you want to." >&2
fi

echo "applying migrations"
./node_modules/.bin/tsx src/db/migrate.ts

echo "starting server on ${HOST}:${PORT}"
exec node dist/server/entry.mjs
