import { defineConfig } from "vite";

/*
 * The PWA is served as static assets from the dashboard's own origin -- Web
 * Bluetooth needs a secure context, and two origins would mean two certificates
 * and a cross-origin fetch to the ingest endpoint for no gain.
 *
 * base "./" so the build works wherever it is mounted; it lands under /app/ on
 * the deployed server.
 */
export default defineConfig({
  base: "./",
  build: {
    outDir: "dist",
    target: "es2022",
    sourcemap: true,
  },
});
