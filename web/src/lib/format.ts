/** Presentation helpers. Nothing here decides anything. */

export function relativeTime(at: Date | null | undefined): string {
  if (!at) return "never";
  const seconds = Math.round((Date.now() - at.getTime()) / 1000);
  if (seconds < 60) return "just now";
  if (seconds < 3600) return `${Math.round(seconds / 60)} min ago`;
  if (seconds < 86400) return `${Math.round(seconds / 3600)} h ago`;
  return `${Math.round(seconds / 86400)} d ago`;
}

export function absoluteTime(at: Date | null | undefined): string {
  if (!at) return "—";
  return at.toLocaleString("en-GB", {
    day: "2-digit",
    month: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
  });
}

export const degrees = (e7: number | null) => (e7 === null ? null : e7 / 1e7);

/** Great-circle distance in metres. */
export function distanceM(
  aLat: number, aLon: number, bLat: number, bLon: number,
): number {
  const R = 6_371_000;
  const toRad = (d: number) => (d * Math.PI) / 180;
  const dLat = toRad(bLat - aLat);
  const dLon = toRad(bLon - aLon);
  const h =
    Math.sin(dLat / 2) ** 2 +
    Math.cos(toRad(aLat)) * Math.cos(toRad(bLat)) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(h));
}

/** Initial bearing in degrees, 0 = north. */
export function bearingDeg(
  aLat: number, aLon: number, bLat: number, bLon: number,
): number {
  const toRad = (d: number) => (d * Math.PI) / 180;
  const dLon = toRad(bLon - aLon);
  const y = Math.sin(dLon) * Math.cos(toRad(bLat));
  const x =
    Math.cos(toRad(aLat)) * Math.sin(toRad(bLat)) -
    Math.sin(toRad(aLat)) * Math.cos(toRad(bLat)) * Math.cos(dLon);
  return (((Math.atan2(y, x) * 180) / Math.PI) + 360) % 360;
}

export function compassPoint(deg: number): string {
  const points = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"];
  return points[Math.round(deg / 45) % 8]!;
}

export const MESSAGE_STATE_LABEL: Record<string, string> = {
  queued: "waiting for budget",
  in_flight: "in flight",
  delivered: "delivered",
  undelivered: "not delivered",
  dropped: "dropped",
  received: "received",
};

/*
 * CLAUDE.md 2.4 requires three outgoing states to be visibly distinct, because
 * they mean different things to the person holding the device: the duty cycle is
 * holding it, it is being retried, or it gave up. Colour alone does not carry
 * that -- each one ships with its own words.
 */
export const MESSAGE_STATE_CLASS: Record<string, string> = {
  queued: "text-[color:var(--color-warning)]",
  in_flight: "text-[color:var(--color-series-1)]",
  delivered: "text-[color:var(--color-good)]",
  undelivered: "text-[color:var(--color-critical)]",
  dropped: "text-ink-muted",
  received: "text-ink-secondary",
};
