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
    expect(allowedDomainsFor("maxl.example.com")).toEqual([{ protocol: "https", hostname: "maxl.example.com" }]);
  });

  it("carries a non-default port", () => {
    expect(allowedDomainsFor("maxl.example.com:8443")).toEqual([
      { protocol: "https", hostname: "maxl.example.com", port: "8443" },
    ]);
  });

  it("accepts a full origin, and drops the default port a pattern could never match", () => {
    expect(allowedDomainsFor("https://maxl.example.com:443/")).toEqual([
      { protocol: "https", hostname: "maxl.example.com" },
    ]);
  });

  it("trusts nothing when unset, which is right for http://localhost", () => {
    expect(allowedDomainsFor(undefined)).toEqual([]);
    expect(allowedDomainsFor("  ")).toEqual([]);
  });

  it.each(["maxl.example.com/dashboard", "ftp://maxl.example.com", "user@maxl.example.com", "not a host"])(
    "refuses %s at build time rather than building a server that answers 403",
    (value) => {
      expect(() => allowedDomainsFor(value)).toThrow(/DASHBOARD_HOST/);
    },
  );
});

describe("astro.config.mjs", () => {
  it("leaves the cross-site check on and feeds it the allowed domains", () => {
    // `checkOrigin` is on by default; the fix must not be to switch it off.
    expect(config.security?.checkOrigin).not.toBe(false);
    expect(Array.isArray(config.security?.allowedDomains)).toBe(true);
  });
});
