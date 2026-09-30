import { defineConfig } from "drizzle-kit";

export default defineConfig({
  dialect: "postgresql",
  schema: "./src/db/schema.ts",
  out: "./drizzle",
  dbCredentials: {
    url: process.env.DATABASE_URL ?? "postgres://maxl:maxl@127.0.0.1:5433/maxl",
  },
  strict: true,
  verbose: true,
});
