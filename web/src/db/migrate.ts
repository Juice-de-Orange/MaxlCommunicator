/*
 * Apply the generated migrations. Run by `npm run db:migrate`, and by the
 * container on start -- a stack that needs a human to run a migration before it
 * works is a stack that will be deployed broken.
 */

import { migrate } from "drizzle-orm/postgres-js/migrator";

import { createDatabase } from "./client";
import { loadDotEnv } from "./env";

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
await migrate(db, { migrationsFolder: "./drizzle" });
await sql.end();
console.log("migrations applied");
