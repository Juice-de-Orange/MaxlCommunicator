import { fileURLToPath } from "node:url";

import { defineConfig } from "vitest/config";

export default defineConfig({
  resolve: {
    alias: {
      "@protocol": fileURLToPath(new URL("../bridge/src/protocol", import.meta.url)),
      "@": fileURLToPath(new URL("./src", import.meta.url)),
    },
  },
  test: {
    setupFiles: ["./test/setup.ts"],
    // The database tests talk to a real PostgreSQL 17 in Docker, not to a mock.
    // Gate 7.1 is about an ON CONFLICT clause and gate 7.2 about ordering, and
    // neither of those exists in a fake.
    testTimeout: 30000,
    hookTimeout: 60000,
    // One database, one schema. Files that create and drop tables in parallel
    // race each other into confusing failures that have nothing to do with the
    // code under test.
    fileParallelism: false,
    //
    // Note for whoever meets a "Bus error (core dumped)" here or from `astro`:
    // it was not the tool. npm had written a truncated
    // @rolldown/binding-linux-x64-gnu (18,396,656 bytes of a 19,324,672-byte
    // file), and mmap on a short file raises SIGBUS. `file` says it plainly --
    // "missing section headers at 19324608". Delete the package, clear the npm
    // cache, reinstall.
  },
});
