/**
 * Loading the shared test vectors.
 *
 * The very same `test-vectors/*.json` that `firmware/test` reads -- via
 * `tools/gen_vectors.py`, which turns them into a C++ header of byte arrays.
 * Neither implementation produced them; they were written from the spec. So both
 * sides agreeing with the vectors is both sides agreeing with each other, which
 * is the only way "docs/bridge-protocol.md is normative and client-independent"
 * can be checked rather than merely asserted.
 */

import { readFileSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
export const VECTOR_DIR = join(HERE, "..", "..", "test-vectors");

interface RawVector {
  name: string;
  bytes: string;
  note?: string;
}

function collect(node: unknown, into: Map<string, Uint8Array>): void {
  if (Array.isArray(node)) {
    for (const child of node) collect(child, into);
    return;
  }
  if (node && typeof node === "object") {
    const record = node as Record<string, unknown>;
    if (typeof record["name"] === "string" && typeof record["bytes"] === "string") {
      const raw = record as unknown as RawVector;
      if (into.has(raw.name)) {
        throw new Error(`duplicate vector name: ${raw.name}`);
      }
      into.set(raw.name, hexToBytes(raw.bytes));
    }
    for (const value of Object.values(record)) collect(value, into);
  }
}

export function hexToBytes(hex: string): Uint8Array {
  if (hex.length % 2 !== 0) {
    throw new Error(`odd-length hex string: ${hex}`);
  }
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i += 1) {
    out[i] = Number.parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

export function bytesToHex(bytes: Uint8Array): string {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

const cache = new Map<string, Uint8Array>();
for (const file of readdirSync(VECTOR_DIR).filter((name) => name.endsWith(".json"))) {
  collect(JSON.parse(readFileSync(join(VECTOR_DIR, file), "utf-8")), cache);
}

/**
 * Look a vector up by name.
 *
 * Throws if it is missing. A test that silently skipped an absent vector would
 * pass for the wrong reason, and a renamed vector would quietly stop being
 * checked on one side while still being checked on the other -- which is the
 * exact drift these files exist to prevent.
 */
export function vector(name: string): Uint8Array {
  const found = cache.get(name);
  if (!found) {
    throw new Error(
      `no test vector named "${name}". Available: ${[...cache.keys()].slice(0, 8).join(", ")}...`,
    );
  }
  return found;
}

export function vectorCount(): number {
  return cache.size;
}
