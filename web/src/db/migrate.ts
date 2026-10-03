/*
 * Apply the generated migrations. Run by `npm run db:migrate`, and by the
 * container on start -- a stack that needs a human to run a migration before it
 * works is a stack that will be deployed broken.
 */

import { migrate } from "drizzle-orm/postgres-js/migrator";

import { createDatabase } from "./client";
import { loadDotEnv } from "./env";
import { describeDatabaseError } from "./errors";

loadDotEnv();

const url = process.env.DATABASE_URL;
if (!url) {
  console.error(
    "DATABASE_URL is not set, and no .env supplied one. " +
      "Locally: cp .env.example .env, then npm run db:up.",
  );
  process.exit(1);
}

const { db, sql } = createDatabase(url, { max: 1 });
try {
  await migrate(db, { migrationsFolder: "./drizzle" });
} catch (error) {
  // One line. The container prints this on every start with the database not
  // yet there, and a stack trace through drizzle says nothing the line does not.
  console.error(`migration failed: ${describeDatabaseError(error)}`);
  await sql.end({ timeout: 1 }).catch(() => {});
  process.exit(1);
}
await sql.end();
console.log("migrations applied");
