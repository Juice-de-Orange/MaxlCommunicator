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

echo "applying migrations"
./node_modules/.bin/tsx src/db/migrate.ts

echo "starting server on ${HOST}:${PORT}"
exec node dist/server/entry.mjs
