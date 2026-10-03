/*
 * DASHBOARD_HOST -> Astro's `security.allowedDomains`.
 *
 * Plain JavaScript, because astro.config.mjs imports it and that file is loaded
 * by Node before anything can compile TypeScript.
 *
 * Behind a TLS-terminating reverse proxy the server is spoken to over plain
 * HTTP, so it takes every request for `http://…` while the browser's `Origin`
 * says `https://…` -- and Astro's cross-site check refused the login form with
 * 403, on exactly the deployment docs/DEPLOYMENT.md prescribes. The proxy does
 * say what the browser used, in `X-Forwarded-Proto` (and `X-Forwarded-Host`),
 * but anybody can send those headers; Astro believes them only for the hosts
 * listed here. Naming the one public host keeps the check on and makes it
 * compare against the right origin.
 */

/**
 * @param {string | undefined} value `maxl.example.com`, optionally with a port
 *   (`maxl.example.com:8443`) or as a full origin (`https://maxl.example.com`).
 * @returns {{ protocol: string, hostname: string, port?: string }[]} Empty when
 *   unset: no forwarded header is trusted, which is right for `http://localhost`.
 */
export function allowedDomainsFor(value) {
  const text = value?.trim();
  if (!text) {
    return [];
  }

  let url;
  try {
    url = new URL(text.includes("://") ? text : `https://${text}`);
  } catch {
    url = null;
  }
  if (
    url === null
    || (url.protocol !== "https:" && url.protocol !== "http:")
    || url.pathname !== "/"
    || url.search !== ""
    || url.hash !== ""
    || url.username !== ""
  ) {
    throw new Error(
      `DASHBOARD_HOST is "${text}" -- expected the public host name of the dashboard, ` +
        "for example maxl.example.com (a port is allowed: maxl.example.com:8443)",
    );
  }

  return [
    {
      protocol: url.protocol.slice(0, -1),
      hostname: url.hostname,
      // Only when it is not the scheme's default: the URL API drops ":443", and
      // a pattern with a port would then match nothing.
      ...(url.port ? { port: url.port } : {}),
    },
  ];
}
