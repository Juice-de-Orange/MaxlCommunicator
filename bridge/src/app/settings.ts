/*
 * What the app remembers between visits.
 *
 * localStorage rather than IndexedDB: this is a handful of scalars, and the
 * journal -- the thing that must not be lost -- lives in IndexedDB where it
 * belongs. Every read is guarded, because a browser in private mode throws on
 * access rather than returning null.
 */

export interface Settings {
  nodeId: number | null;
  ingestToken: string | null;
  serverBaseUrl: string;
  lastSyncAt: number | null;
  lastSyncEvents: number;
  pendingOnDevice: number;
  deviceName: string | null;
}

const KEY = "maxl-bridge-settings";

const DEFAULTS: Settings = {
  nodeId: null,
  ingestToken: null,
  serverBaseUrl: "",
  lastSyncAt: null,
  lastSyncEvents: 0,
  pendingOnDevice: 0,
  deviceName: null,
};

export function loadSettings(): Settings {
  try {
    const raw = localStorage.getItem(KEY);
    return raw ? { ...DEFAULTS, ...(JSON.parse(raw) as Partial<Settings>) } : { ...DEFAULTS };
  } catch {
    return { ...DEFAULTS };
  }
}

export function saveSettings(settings: Settings): void {
  try {
    localStorage.setItem(KEY, JSON.stringify(settings));
  } catch {
    // A browser that refuses to store this still works; it just forgets the
    // last sync between visits. Losing that is not worth failing a sync over.
  }
}
