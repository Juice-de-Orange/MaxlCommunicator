// @ts-check
import { fileURLToPath } from "node:url";

import node from "@astrojs/node";
import react from "@astrojs/react";
import tailwind from "@tailwindcss/vite";
import { defineConfig } from "astro/config";

/*
 * SSR on the Node adapter, standalone: the dashboard reads a live database on
 * every request, so there is nothing to prerender. It runs in Docker Compose
 * behind a TLS-terminating reverse proxy (docs/DEPLOYMENT.md).
 *
 * The `@protocol` alias points at the bridge's protocol module rather than at a
 * copy. docs/bridge-protocol.md is normative and both sides already agree with
 * the shared vectors in test-vectors/; a second decoder in this package would be
 * a second thing to keep in step, and the first one to drift silently.
 */
export default defineConfig({
  output: "server",
  adapter: node({ mode: "standalone" }),
  integrations: [react()],
  vite: {
    plugins: [tailwind()],
    resolve: {
      alias: {
        "@protocol": fileURLToPath(new URL("../bridge/src/protocol", import.meta.url)),
      },
    },
    server: {
      fs: {
        // The alias above resolves outside this package's root.
        allow: [fileURLToPath(new URL("..", import.meta.url))],
      },
    },
  },
});
