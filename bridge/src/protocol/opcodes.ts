/**
 * Opcodes, events, errors and auth tiers.
 *
 * `docs/bridge-protocol.md` is normative and client-independent -- it is the
 * document a Kotlin client would be written against in Phase 9 without reading
 * any of this. Every value here cites the section it comes from, so the two can
 * be checked against each other by reading.
 */

/**
 * Reported in `GET_INFO`. Section 0.
 *
 * Went to 2 on 2026-08-31 for `EVT_JOURNAL` and nothing else -- decision D15.
 * With two nodes there is no rolling upgrade; both get flashed in the same
 * session (`CLAUDE.md` §6), so the break costs nothing.
 */
export const BRIDGE_PROTOCOL_VERSION = 2;

/** Commands, phone -> device. Section 3. */
export const Opcode = {
  GetInfo: 0x01,
  GetStatus: 0x02,
  SendText: 0x03,
  GetQueue: 0x04,
  AckQueue: 0x05,
  GetConfig: 0x06,
  SetConfig: 0x07,
  SetTime: 0x08,
  RequestFix: 0x09,
  ProvisionKey: 0x0a,
  RotateKey: 0x0b,
  FactoryReset: 0x0c,
  GetBudget: 0x0d,
  LinkTest: 0x0e,
} as const;
export type Opcode = (typeof Opcode)[keyof typeof Opcode];

/** Events and responses, device -> phone. Section 4. */
export const EventCode = {
  FrameRx: 0x81,
  FrameTxResult: 0x82,
  Status: 0x83,
  Log: 0x84,
  ConfigApplied: 0x85,
  Fix: 0x86,
  Budget: 0x87,
  /**
   * A journal entry with its counter, section 4.
   *
   * Only `GET_QUEUE` produces it. It wraps one of the codes above, and the
   * counter it carries is the JOURNAL counter -- the one `ACK_QUEUE` speaks in,
   * which is not the frame counter some bodies also contain (D10).
   */
  Journal: 0x88,
  ResponseOk: 0xc0,
  ResponseError: 0xc1,
} as const;
export type EventCode = (typeof EventCode)[keyof typeof EventCode];

/** Section 4. */
export const BridgeError = {
  Unsupported: 0x01,
  BadLength: 0x02,
  BadParam: 0x03,
  NotAuthorised: 0x04,
  NoKey: 0x05,
  BudgetExhausted: 0x06,
  QueueFull: 0x07,
  NoTime: 0x08,
  Busy: 0x09,
  Storage: 0x0a,
} as const;
export type BridgeError = (typeof BridgeError)[keyof typeof BridgeError];

export const BRIDGE_ERROR_NAMES: Readonly<Record<number, string>> = {
  [BridgeError.Unsupported]: "ERR_UNSUPPORTED",
  [BridgeError.BadLength]: "ERR_BAD_LENGTH",
  [BridgeError.BadParam]: "ERR_BAD_PARAM",
  [BridgeError.NotAuthorised]: "ERR_NOT_AUTHORISED",
  [BridgeError.NoKey]: "ERR_NO_KEY",
  [BridgeError.BudgetExhausted]: "ERR_BUDGET_EXHAUSTED",
  [BridgeError.QueueFull]: "ERR_QUEUE_FULL",
  [BridgeError.NoTime]: "ERR_NO_TIME",
  [BridgeError.Busy]: "ERR_BUSY",
  [BridgeError.Storage]: "ERR_STORAGE",
};

/** Section 2. Enforced on the device; this is for the UI, not for security. */
export type AuthTier = "open" | "bonded";

const OPEN_OPCODES: ReadonlySet<number> = new Set([
  Opcode.GetInfo,
  Opcode.GetStatus,
  Opcode.GetBudget,
]);

/**
 * Which tier an opcode needs.
 *
 * Unknown opcodes are `bonded`. Defaulting an unrecognised opcode to `open`
 * would mean that listing it here is what makes it safe -- and forgetting to is
 * what makes it reachable by anyone in Bluetooth range. The device enforces this
 * independently; a client that got it wrong would merely mispredict, which is why
 * section 2 says the tier is enforced on the device, not by the client.
 */
export function tierOf(opcode: number): AuthTier {
  return OPEN_OPCODES.has(opcode) ? "open" : "bonded";
}

/** `EVT_FRAME_TX_RESULT.result`, section 4. */
export const TxResult = {
  Delivered: 0,
  Undelivered: 1,
  Queued: 2,
  DroppedByUser: 3,
} as const;
export type TxResult = (typeof TxResult)[keyof typeof TxResult];

/** Config TLV types, section 3. */
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
export type ConfigTlv = (typeof ConfigTlv)[keyof typeof ConfigTlv];

/**
 * There is deliberately no TLV for the duty cycle limit.
 *
 * Section 3 says so in as many words, and `CLAUDE.md` §1.2 explains why: the
 * duty cycle is legally binding, is enforced in firmware, and "is not a
 * configurable option and is not exposed as a user setting". A client that tried
 * to offer one would be building a UI for something the device will refuse.
 */
