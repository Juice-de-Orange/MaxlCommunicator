/*
 * A node id is the u16 address of the frame header (CLAUDE.md 2.1), and
 * `devices.node_id` is an int4. Anything else -- 1.5, 2^40, "abc" -- is not an
 * unknown node, it is not a node id at all, and handing it to the query made
 * PostgreSQL refuse the parameter and the endpoint answer 500.
 */

export const MAX_NODE_ID = 0xffff;

export const NODE_ID_RULE = `nodeId must be an integer between 0 and ${MAX_NODE_ID}`;

export function isNodeId(value: unknown): value is number {
  return typeof value === "number" && Number.isInteger(value) && value >= 0 && value <= MAX_NODE_ID;
}

/** From a URL segment or a command-line argument: digits only, so "1abc" and
 *  "1.5" are refused rather than read as 1. */
export function parseNodeId(text: string | null | undefined): number | null {
  if (!text || !/^\d{1,5}$/.test(text)) return null;
  const value = Number(text);
  return isNodeId(value) ? value : null;
}
