/*
 * Read `.env` into the process environment, without overriding anything already
 * there.
 *
 * Why this exists rather than a dependency: the format we need is `KEY=value`,
 * and dotenv would be a runtime dependency for eight lines. The deployment does not use it at all -- there the
 * variables come from the container environment, which is why nothing here may
 * overwrite a value that is already set.
 *
 * It used to live only in `test/setup.ts`, so `npm run db:migrate` did not see
 * `.env` and refused with "DATABASE_URL is not set" -- while `npm test`, three
 * lines further down the README, worked. The documented flow only appeared to
 * work because somebody had migrated the schema by hand once.
 *
 * The same happened a second time with the server and `scripts/device.ts`:
 * whatever reads `process.env` for one of the variables in `.env.example` has to
 * have called this first -- `src/middleware.ts` does it for every page and
 * endpoint.
 */

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

export function loadDotEnv(from: URL = new URL("../../.env", import.meta.url)): void {
  let contents: string;
  try {
    contents = readFileSync(fileURLToPath(from), "utf8");
  } catch {
    // No .env is a normal state: in the container the environment is the
    // source. A caller that needs a variable says so itself.
    return;
  }

  for (const line of contents.split("\n")) {
    const match = /^([A-Z0-9_]+)=(.*)$/.exec(line.trim());
    if (match && !process.env[match[1]!]) {
      process.env[match[1]!] = match[2]!;
    }
  }
}
