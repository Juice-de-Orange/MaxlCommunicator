/*
 * One line for a database failure, for the command-line tools.
 *
 * drizzle wraps every failure in a DrizzleQueryError whose message is the query
 * and its parameters. Left uncaught, `device:add` on a node id that already
 * exists printed forty lines of stack trace -- and among the parameters the hash
 * of the token it had just generated. Only the hash is ever stored (CLAUDE.md
 * 4.3) and it belongs in a log no more than it belongs anywhere else.
 *
 * So this never returns the wrapper's own message: it walks to the cause, which
 * is what PostgreSQL or the socket actually said.
 */

interface WithCause {
  cause?: unknown;
  code?: unknown;
  message?: unknown;
  errors?: unknown;
}

export function describeDatabaseError(error: unknown): string {
  let current = error as WithCause | null | undefined;
  while (current && typeof current === "object" && current.cause) {
    current = current.cause as WithCause;
  }
  // A refused connection to `localhost` is an AggregateError (one attempt per
  // address family) with an empty message of its own.
  if (current && Array.isArray(current.errors) && current.errors.length > 0) {
    current = current.errors[0] as WithCause;
  }
  if (!current || typeof current !== "object") {
    return String(current);
  }

  const code = typeof current.code === "string" ? current.code : undefined;
  const message = typeof current.message === "string" ? current.message.split("\n")[0]! : "";
  if (code === "ECONNREFUSED" || code === "ENOTFOUND" || code === "ETIMEDOUT" || code === "CONNECT_TIMEOUT") {
    return `cannot reach the database (${message || code}) -- is it running? Locally: npm run db:up`;
  }
  if (message && code) return `${message} (${code})`;
  return message || code || "unknown error";
}
