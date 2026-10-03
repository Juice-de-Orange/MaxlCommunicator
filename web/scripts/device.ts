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
import { loadDotEnv } from "../src/db/env";
import { describeDatabaseError } from "../src/db/errors";
import { devices } from "../src/db/schema";
import { generateIngestToken, hashIngestToken } from "../src/lib/auth";
import { MAX_NODE_ID, parseNodeId } from "../src/lib/node-id";

function arg(name: string): string | undefined {
  const index = process.argv.indexOf(`--${name}`);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

// As `npm run db:migrate` does. In the container there is no .env and the
// environment is the source.
loadDotEnv();

const url = process.env.DATABASE_URL;
if (!url) {
  console.error("DATABASE_URL is not set, and no .env supplied one. See web/.env.example.");
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
    const nodeId = parseNodeId(arg("node-id"));
    const name = arg("name");
    if (nodeId === null || !name) {
      console.error(`usage: device add --node-id <0..${MAX_NODE_ID}> --name "<label>"`);
      process.exit(2);
    }
    // 0xFFFF is broadcast in the frame header (CLAUDE.md 2.1) and can never be
    // a device's own id.
    if (nodeId === 0xffff) {
      console.error("0xFFFF is the broadcast address and cannot be a node id");
      process.exit(2);
    }
    const token = generateIngestToken();
    // ON CONFLICT rather than catching the unique violation: an existing node
    // is an answer, not a failure, and the error it used to raise carried the
    // new token's hash among its parameters.
    const [row] = await db
      .insert(devices)
      .values({ nodeId, name, ingestTokenHash: hashIngestToken(token) })
      .onConflictDoNothing({ target: devices.nodeId })
      .returning({ id: devices.id });
    if (!row) {
      console.error(
        `node ${nodeId} is already registered -- nothing was changed. ` +
          `For a new token: device rotate --node-id ${nodeId}`,
      );
      process.exit(1);
    }
    printToken(nodeId, name, token);
  } else if (command === "rotate") {
    const nodeId = parseNodeId(arg("node-id"));
    if (nodeId === null) {
      console.error(`usage: device rotate --node-id <0..${MAX_NODE_ID}>`);
      process.exit(2);
    }
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
} catch (error) {
  // One line, and never the query's parameters: for `add` and `rotate` one of
  // them is the hash of the token that was just generated.
  console.error(`device ${command} failed: ${describeDatabaseError(error)}`);
  process.exitCode = 1;
} finally {
  await sql.end({ timeout: 1 }).catch(() => {});
}
