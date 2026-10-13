/*
 * DASHBOARD_HOST. Behind the TLS proxy that docs/DEPLOYMENT.md prescribes, the
 * login form was answered with 403 "Cross-site POST form submissions are
 * forbidden": the server saw `http://`, the browser said `https://`, and nothing
 * told Astro which forwarded headers to believe. This is the piece that tells
 * it -- the end-to-end proof needs a proxy and a browser and is not in here.
 */

import { describe, expect, it } from "vitest";

import config from "../astro.config.mjs";
import { allowedDomainsFor } from "../src/lib/public-host.mjs";

describe("DASHBOARD_HOST", () => {
  it("trusts forwarded headers for exactly the public host, over https", () => {
    expect(allowedDomainsFor("trailtalk.example.com")).toEqual([{ protocol: "https", hostname: "trailtalk.example.com" }]);
  });

  it("carries a non-default port", () => {
    expect(allowedDomainsFor("trailtalk.example.com:8443")).toEqual([
      { protocol: "https", hostname: "trailtalk.example.com", port: "8443" },
    ]);
  });

  it("accepts a full origin, and drops the default port a pattern could never match", () => {
    expect(allowedDomainsFor("https://trailtalk.example.com:443/")).toEqual([
      { protocol: "https", hostname: "trailtalk.example.com" },
    ]);
  });

  it("ignores WEB_PORT once a public host is named", () => {
    expect(allowedDomainsFor("trailtalk.example.com", "8080")).toEqual([
      { protocol: "https", hostname: "trailtalk.example.com" },
    ]);
  });

  it.each(["trailtalk.example.com/dashboard", "ftp://trailtalk.example.com", "user@trailtalk.example.com", "not a host"])(
    "refuses %s at build time rather than building a server that answers 403",
    (value) => {
      expect(() => allowedDomainsFor(value)).toThrow(/DASHBOARD_HOST/);
    },
  );
});

/*
 * Without DASHBOARD_HOST. Astro then believes the `Host` header for nothing and
 * compares the browser's `Origin` with `http://localhost:4321` -- the port the
 * server listens on inside the container, not the one WEB_PORT publishes. A
 * dashboard on any other port, and `http://127.0.0.1:4321` too, answered every
 * sign-in with 403 "Cross-site POST form submissions are forbidden".
 */
describe("WEB_PORT, while DASHBOARD_HOST is unset", () => {
  it("accepts localhost and 127.0.0.1 on the default port", () => {
    const expected = [
      { protocol: "http", hostname: "localhost", port: "4321" },
      { protocol: "http", hostname: "127.0.0.1", port: "4321" },
    ];
    expect(allowedDomainsFor(undefined)).toEqual(expected);
    expect(allowedDomainsFor("  ", "")).toEqual(expected);
  });

  it("accepts both on the port the dashboard is published on", () => {
    expect(allowedDomainsFor("", "8080")).toEqual([
      { protocol: "http", hostname: "localhost", port: "8080" },
      { protocol: "http", hostname: "127.0.0.1", port: "8080" },
    ]);
  });

  it("drops port 80, which a pattern could never match", () => {
    expect(allowedDomainsFor("", "80")).toEqual([
      { protocol: "http", hostname: "localhost" },
      { protocol: "http", hostname: "127.0.0.1" },
    ]);
  });

  it.each(["http", "0", "65536", "80a", "4321/"])("refuses WEB_PORT=%s at build time", (port) => {
    expect(() => allowedDomainsFor("", port)).toThrow(/WEB_PORT/);
  });
});

describe("astro.config.mjs", () => {
  it("leaves the cross-site check on and feeds it the allowed domains", () => {
    // `checkOrigin` is on by default; the fix must not be to switch it off.
    expect(config.security?.checkOrigin).not.toBe(false);
    expect(Array.isArray(config.security?.allowedDomains)).toBe(true);
  });
});
