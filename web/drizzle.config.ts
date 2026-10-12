import { defineConfig } from "drizzle-kit";

export default defineConfig({
  dialect: "postgresql",
  schema: "./src/db/schema.ts",
  out: "./drizzle",
  dbCredentials: {
    url: process.env.DATABASE_URL ?? "postgres://trailtalk:trailtalk@127.0.0.1:5433/trailtalk",
  },
  strict: true,
  verbose: true,
});
