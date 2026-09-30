/*
 * Register a node and issue its ingest token.
 *
 *   npm run device:add -- --node-id 1 --name "T-Echo A"
 *   npm run device:list
 *   npm run device:rotate -- --node-id 1
 *
 * The token is printed once and never stored -- only its SHA-256 goes into the
 * database. CLAUDE.md 4.3 has this endpoint authenticate a device-bridge pair,
 * and a dashboard database that can be read should not also be a database that
 * can impersonate every node in the network.
 */

import { eq } from "drizzle-orm";

import { createDatabase } from "../src/db/client";
import { devices } from "../src/db/schema";
import { generateIngestToken, hashIngestToken } from "../src/lib/auth";

function arg(name: string): string | undefined {
  const index = process.argv.indexOf(`--${name}`);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

const url = process.env.DATABASE_URL;
if (!url) {
  console.error("DATABASE_URL is not set");
  process.exit(1);
}

const command = process.argv[2];
const { db, sql } = createDatabase(url, { max: 1 });

function printToken(nodeId: number, name: string, token: string): void {
  console.log("");
  console.log(`  node ${nodeId} (${name})`);
  console.log(`  ingest token: ${token}`);
  console.log("");
  console.log("  Shown once. Only its hash is stored -- if it is lost, rotate it.");
  console.log("  The bridge sends it as: Authorization: Bearer <token>");
  console.log("");
}

try {
  if (command === "add") {
    const nodeId = Number.parseInt(arg("node-id") ?? "", 10);
    const name = arg("name");
    if (!Number.isInteger(nodeId) || nodeId < 0 || nodeId > 0xffff || !name) {
      console.error('usage: device add --node-id <0..65535> --name "<label>"');
      process.exit(2);
    }
    // 0xFFFF is broadcast in the frame header (CLAUDE.md 2.1) and can never be
    // a device's own id.
    if (nodeId === 0xffff) {
      console.error("0xFFFF is the broadcast address and cannot be a node id");
      process.exit(2);
    }
    const token = generateIngestToken();
    await db.insert(devices).values({ nodeId, name, ingestTokenHash: hashIngestToken(token) });
    printToken(nodeId, name, token);
  } else if (command === "rotate") {
    const nodeId = Number.parseInt(arg("node-id") ?? "", 10);
    const token = generateIngestToken();
    const [row] = await db
      .update(devices)
      .set({ ingestTokenHash: hashIngestToken(token) })
      .where(eq(devices.nodeId, nodeId))
      .returning({ name: devices.name });
    if (!row) {
      console.error(`no node ${nodeId}`);
      process.exit(1);
    }
    printToken(nodeId, row.name, token);
  } else if (command === "list") {
    const rows = await db.select().from(devices).orderBy(devices.nodeId);
    if (rows.length === 0) {
      console.log("no devices registered");
    }
    for (const row of rows) {
      console.log(
        `  ${String(row.nodeId).padStart(5)}  ${row.name.padEnd(20)}  ` +
          `last seen ${row.lastSeenAt?.toISOString() ?? "never"}`,
      );
    }
  } else {
    console.error("usage: device <add|rotate|list> [...]");
    process.exit(2);
  }
} finally {
  await sql.end();
}
