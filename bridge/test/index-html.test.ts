/*
 * The build is mounted in a directory (/app/ on the dashboard) and every URL in
 * it is relative. The dashboard's static handler answers /app with the same
 * index.html, and from there `./assets/...` resolves to /assets/... -- the page
 * comes up blank. index.html corrects its own address before anything loads.
 */

import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";

const html = readFileSync(new URL("../index.html", import.meta.url), "utf8");

function open(pathname: string, search = "", hash = ""): string | null {
  // The first <script> in our own file, by position -- this is not an HTML
  // filter and must not look like one.
  const start = html.indexOf("<script>");
  const end = html.indexOf("</script>", start);
  if (start < 0 || end < 0) throw new Error("index.html has no inline script");
  let replaced: string | null = null;
  runInNewContext(html.slice(start + "<script>".length, end), {
    location: { pathname, search, hash, replace: (to: string) => void (replaced = to) },
  });
  return replaced;
}

describe("index.html opened without the trailing slash", () => {
  it("moves to the directory URL", () => {
    expect(open("/app")).toBe("/app/");
  });

  it("keeps query and fragment", () => {
    expect(open("/app", "?a=1", "#x")).toBe("/app/?a=1#x");
  });

  it("leaves a directory URL and a file URL alone", () => {
    expect(open("/app/")).toBeNull();
    expect(open("/")).toBeNull();
    expect(open("/app/index.html")).toBeNull();
  });

  it("runs before the stylesheet and the module are requested", () => {
    expect(html.indexOf("<script>")).toBeGreaterThan(-1);
    expect(html.indexOf("<script>")).toBeLessThan(html.indexOf('rel="stylesheet"'));
    expect(html.indexOf("<script>")).toBeLessThan(html.indexOf('type="module"'));
  });
});
