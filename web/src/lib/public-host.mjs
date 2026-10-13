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
 *
 * Without a public host the same list has a second job. Astro believes the
 * `Host` header only for the hosts listed here, too; for any other it compares
 * the browser's `Origin` with `http://localhost:<the port the server listens
 * on>` -- and inside the container that is 4321 whatever `WEB_PORT` publishes.
 * With an empty list, a dashboard published on another port refused every
 * sign-in with 403, and so did `http://127.0.0.1:4321`. So the default names
 * the two loopback names on the published port.
 */

/** The container's own port, and compose's default for `WEB_PORT`. */
const DEFAULT_WEB_PORT = "4321";

/**
 * @param {string | undefined} value `trailtalk.example.com`, optionally with a port
 *   (`trailtalk.example.com:8443`) or as a full origin (`https://trailtalk.example.com`).
 * @param {string | undefined} [webPort] `WEB_PORT`, the port the dashboard is
 *   published on and opened through when `value` is unset; 4321 when empty.
 * @returns {{ protocol: string, hostname: string, port?: string }[]} When `value`
 *   is unset: `http://localhost` and `http://127.0.0.1` on `webPort`.
 */
export function allowedDomainsFor(value, webPort) {
  const text = value?.trim();
  if (!text) {
    return loopbackDomainsFor(webPort);
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
        "for example trailtalk.example.com (a port is allowed: trailtalk.example.com:8443)",
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

/**
 * @param {string | undefined} webPort
 * @returns {{ protocol: string, hostname: string, port?: string }[]}
 */
function loopbackDomainsFor(webPort) {
  const port = webPort?.trim() || DEFAULT_WEB_PORT;
  const number = Number(port);
  if (!/^\d+$/.test(port) || number < 1 || number > 65535) {
    throw new Error(`WEB_PORT is "${port}" -- expected a port number, for example 4321`);
  }
  // As above: the URL API drops ":80", and a pattern with it would match nothing.
  const portPattern = number === 80 ? {} : { port: String(number) };
  return ["localhost", "127.0.0.1"].map((hostname) => ({ protocol: "http", hostname, ...portPattern }));
}
