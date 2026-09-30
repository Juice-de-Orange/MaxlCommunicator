/*
 * Config TLVs, dashboard side.
 *
 * The encoding is docs/bridge-protocol.md's, and the ranges are the ones that
 * document gives. They are enforced here as well as on the device, and that
 * duplication is deliberate: the device is the authority and rejects what it
 * cannot apply, but a dashboard that lets somebody push a 20 ms sniff interval
 * and only finds out from an EVT_CONFIG_APPLIED an hour later is a dashboard
 * that wasted an hour.
 *
 * There is deliberately no TLV for the duty cycle limit. CLAUDE.md 1.3: "It is
 * not configurable, in firmware or over the air."
 */

export const ConfigTlv = {
  DeviceName: 0x01,
  SniffIntervalMs: 0x02,
  Band: 0x03,
  SfMode: 0x04,
  FixedSf: 0x05,
  TxPowerDbm: 0x06,
  TelemetryIntervalS: 0x07,
  BeaconIntervalS: 0x08,
  GnssFixTimeoutS: 0x09,
} as const;

export interface ConfigSettings {
  sniffIntervalMs?: number;
  band?: number;
  sfMode?: number;
  fixedSf?: number;
  txPowerDbm?: number;
  telemetryIntervalS?: number;
  beaconIntervalS?: number;
  gnssFixTimeoutS?: number;
}

export interface FieldSpec {
  key: keyof ConfigSettings;
  tlv: number;
  label: string;
  hint: string;
  bytes: 1 | 2;
  signed?: boolean;
  min: number;
  max: number;
  options?: { value: number; label: string }[];
}

/**
 * The fields, their ranges, and why each range is what it is. The hints are
 * shown in the UI, because a number with no consequence attached is a number
 * somebody will change to see what happens.
 */
export const CONFIG_FIELDS: FieldSpec[] = [
  {
    key: "sniffIntervalMs",
    tlv: ConfigTlv.SniffIntervalMs,
    label: "Sniff interval",
    hint:
      "The single tuning knob. Trades latency against battery and airtime at once — " +
      "see the table under Radio. Default 2000.",
    bytes: 2,
    min: 250,
    max: 10000,
  },
  {
    key: "band",
    tlv: ConfigTlv.Band,
    label: "Sub-band",
    hint: "g3 gives ten times the airtime and allows the full +22 dBm.",
    bytes: 1,
    min: 0,
    max: 1,
    options: [
      { value: 0, label: "g3 — 869.4–869.65 MHz, 10 %" },
      { value: 1, label: "g1 — 868.0–868.6 MHz, 1 %" },
    ],
  },
  {
    key: "sfMode",
    tlv: ConfigTlv.SfMode,
    label: "Spreading factor",
    hint: "Adaptive follows the link quality; fixed holds what is set below.",
    bytes: 1,
    min: 0,
    max: 1,
    options: [
      { value: 0, label: "adaptive" },
      { value: 1, label: "fixed" },
    ],
  },
  {
    key: "fixedSf",
    tlv: ConfigTlv.FixedSf,
    label: "Fixed spreading factor",
    hint: "Only effective when \"fixed\" is selected above. SF12 at a short interval is a receiver that is effectively always on.",
    bytes: 1,
    min: 7,
    max: 12,
  },
  {
    key: "txPowerDbm",
    tlv: ConfigTlv.TxPowerDbm,
    label: "Transmit power (dBm)",
    hint: "Capped by the device to the band's ERP limit — this value is a request, not a promise.",
    bytes: 1,
    signed: true,
    min: -9,
    max: 22,
  },
  {
    key: "telemetryIntervalS",
    tlv: ConfigTlv.TelemetryIntervalS,
    label: "Telemetry interval (s)",
    hint: "0 turns it off. Every transmission is paid from the same hourly budget as a message.",
    bytes: 2,
    min: 0,
    max: 65535,
  },
  {
    key: "beaconIntervalS",
    tlv: ConfigTlv.BeaconIntervalS,
    label: "Beacon interval (s)",
    hint: "With two nodes, beacon rarely — every received frame counts as a sign of life anyway.",
    bytes: 2,
    min: 0,
    max: 65535,
  },
  {
    key: "gnssFixTimeoutS",
    tlv: ConfigTlv.GnssFixTimeoutS,
    label: "GNSS timeout (s)",
    hint: "Hard upper limit. GNSS draws tens of milliamps and is the power budget.",
    bytes: 2,
    min: 10,
    max: 900,
  },
];

export function encodeConfigTlvs(settings: ConfigSettings): Uint8Array {
  const out: number[] = [];
  for (const field of CONFIG_FIELDS) {
    const value = settings[field.key];
    if (value === undefined) {
      continue;
    }
    out.push(field.tlv, field.bytes);
    if (field.bytes === 1) {
      out.push(value & 0xff);
    } else {
      out.push(value & 0xff, (value >> 8) & 0xff);
    }
  }
  return Uint8Array.from(out);
}

export function decodeConfigTlvs(blob: Uint8Array): ConfigSettings {
  const settings: ConfigSettings = {};
  let cursor = 0;
  while (cursor + 2 <= blob.length) {
    const type = blob[cursor]!;
    const length = blob[cursor + 1]!;
    if (cursor + 2 + length > blob.length) {
      break;
    }
    const field = CONFIG_FIELDS.find((candidate) => candidate.tlv === type);
    if (field && length === field.bytes) {
      const raw =
        field.bytes === 1 ? blob[cursor + 2]! : blob[cursor + 2]! | (blob[cursor + 3]! << 8);
      settings[field.key] = field.signed && raw > 127 ? raw - 256 : raw;
    }
    cursor += 2 + length;
  }
  return settings;
}

/** Returns the first problem, or null. */
export function validateConfig(settings: ConfigSettings): string | null {
  for (const field of CONFIG_FIELDS) {
    const value = settings[field.key];
    if (value === undefined) {
      continue;
    }
    if (!Number.isInteger(value)) {
      return `${field.label} must be a whole number.`;
    }
    if (value < field.min || value > field.max) {
      return `${field.label}: ${value} is outside ${field.min}…${field.max}.`;
    }
  }
  return null;
}
