/*
 * The database tests talk to a real PostgreSQL 17 (`npm run db:up`), not to a
 * mock. Gate 7.1 is a property of an ON CONFLICT clause and gate 7.2 of an ORDER
 * BY; neither of them exists in a fake, and a suite that passes against one
 * proves nothing about either.
 */

import { loadDotEnv } from "../src/db/env";
import { testDatabaseUrl } from "./database";

// Shared with `npm run db:migrate`, which needs exactly the same thing. When it
// lived only here, the migration script could not see `.env` and the documented
// flow in web/README.md worked only by accident.
loadDotEnv(new URL("../.env", import.meta.url));

// Real, but not the one `DATABASE_URL` names: the suite empties every table, so
// it runs in that database's `_test` sibling (test/database.ts), which
// test/global-setup.ts has created and migrated. Everything under test reads
// `DATABASE_URL`, so this is the one place that has to know.
if (process.env.DATABASE_URL) {
  process.env.DATABASE_URL = testDatabaseUrl(process.env.DATABASE_URL);
}
