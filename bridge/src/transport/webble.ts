/**
 * Web Bluetooth transport.
 *
 * **Not exercised by the test suite** -- it needs a browser and a device. It is
 * written against the same `Transport` interface as the mock, so everything
 * above it *is* tested; what remains untested here is the twenty lines that talk
 * to `navigator.bluetooth`.
 *
 * Two constraints from `CLAUDE.md` §4.2 are baked in rather than worked around:
 *
 * **The connection drops when the tab is hidden and there is no background
 * mode.** So there is no reconnect loop, no service worker, no wake lock. None
 * of them keep a GATT connection alive, and building them would be a convincing
 * imitation of background sync that fails in the field. The product is designed
 * around this: the device queues everything and the user opens the app to sync.
 * A native Android bridge with a foreground service is Phase 9.
 *
 * **iOS is not supported.** Apple has declined to implement Web Bluetooth and
 * that has not changed. `isSupported()` exists so the UI can say so plainly
 * rather than failing silently.
 */

import { MIN_MTU } from "../protocol/chunker.js";
import { TransportError, type Transport } from "./types.js";

/** Custom 128-bit service UUID (`CLAUDE.md` §4.1). */
export const SERVICE_UUID = "6d61786c-0001-4c6f-5261-4e6f64650000";
export const TX_CHARACTERISTIC = "6d61786c-0002-4c6f-5261-4e6f64650000";
export const RX_CHARACTERISTIC = "6d61786c-0003-4c6f-5261-4e6f64650000";
export const CONFIG_CHARACTERISTIC = "6d61786c-0004-4c6f-5261-4e6f64650000";
export const STATUS_CHARACTERISTIC = "6d61786c-0005-4c6f-5261-4e6f64650000";

export interface BluetoothSupport {
  readonly supported: boolean;
  /** English, for logs and for a developer reading a stack trace. */
  readonly reason?: string;
  /**
   * A stable identifier for the same fact. The user-facing wording is the UI's
   * business and is not in English; matching on `reason` to translate it would
   * make a message string load-bearing, and the first person to reword it would
   * silently break the translation.
   */
  readonly code?: "no-api" | "ios" | "insecure-context";
}

export function isSupported(): BluetoothSupport {
  /*
   * Read everything off `navigator` BEFORE any narrowing.
   *
   * @types/web-bluetooth declares `Navigator.bluetooth` as required, so an `in`
   * or truthiness guard narrows the negative branch to `never`, and reading
   * `userAgent` there will not compile -- even though at runtime that branch is
   * exactly where every browser without the API ends up. The runtime check is
   * real: this module is imported in Node during the tests.
   */
  const nav = globalThis.navigator as (Navigator & { bluetooth?: unknown }) | undefined;
  const userAgent = nav?.userAgent ?? "";
  const hasBluetooth = nav !== undefined && nav.bluetooth !== undefined;

  if (!hasBluetooth) {
    if (/iPhone|iPad|iPod/i.test(userAgent)) {
      return {
        supported: false,
        code: "ios",
        reason:
          "Apple does not implement Web Bluetooth, so this app cannot talk to a node on iOS. " +
          "Use an Android phone with Chrome.",
      };
    }
    return { supported: false, code: "no-api", reason: "This browser does not support Web Bluetooth." };
  }

  if (typeof window !== "undefined" && !window.isSecureContext) {
    return {
      supported: false,
      code: "insecure-context",
      reason: "Web Bluetooth needs a secure context; open the app over HTTPS.",
    };
  }
  return { supported: true };
}

/**
 * The MTU Web Bluetooth gives us.
 *
 * There is no API for the negotiated ATT MTU. Chrome on Android negotiates 247
 * and `writeValueWithoutResponse` accepts 244-byte payloads, but nothing in the
 * standard promises it -- so this is a documented assumption, not a fact, and
 * `SAFE_MTU` is what the chunker is told.
 */
export const ASSUMED_MTU = 247;

export class WebBluetoothTransport implements Transport {
  #server: BluetoothRemoteGATTServer;
  #tx: BluetoothRemoteGATTCharacteristic;
  #rx: BluetoothRemoteGATTCharacteristic;
  #handlers = new Set<(chunk: Uint8Array) => void>();
  #listening = false;

  readonly mtu: number;
  readonly bonded: boolean;

  private constructor(
    server: BluetoothRemoteGATTServer,
    tx: BluetoothRemoteGATTCharacteristic,
    rx: BluetoothRemoteGATTCharacteristic,
    mtu: number,
  ) {
    this.#server = server;
    this.#tx = tx;
    this.#rx = rx;
    this.mtu = mtu;
    /*
     * Web Bluetooth does not expose bond state. The device is the authority
     * anyway (§2: "Tier is enforced on the device, not by the client"), so this
     * reports false and the UI learns the truth from an ERR_NOT_AUTHORISED.
     * Guessing true here would produce a UI that offers actions the node
     * refuses.
     */
    this.bonded = false;
  }

  static async connect(): Promise<WebBluetoothTransport> {
    const support = isSupported();
    if (!support.supported) {
      throw new TransportError(support.reason ?? "Web Bluetooth unavailable");
    }

    const device = await globalThis.navigator.bluetooth.requestDevice({
      filters: [{ services: [SERVICE_UUID] }],
      optionalServices: [SERVICE_UUID],
    });
    const server = await device.gatt?.connect();
    if (!server) {
      throw new TransportError("GATT server unavailable");
    }

    const service = await server.getPrimaryService(SERVICE_UUID);
    const tx = await service.getCharacteristic(TX_CHARACTERISTIC);
    const rx = await service.getCharacteristic(RX_CHARACTERISTIC);
    return new WebBluetoothTransport(server, tx, rx, ASSUMED_MTU);
  }

  async write(chunk: Uint8Array): Promise<void> {
    try {
      // Without response: the chunk layer already detects loss through the
      // index sequence, and waiting for an ATT response per chunk would make a
      // 4 KB message painfully slow over a short foreground connection.
      await this.#rx.writeValueWithoutResponse(chunk as BufferSource);
    } catch (cause) {
      throw new TransportError("write failed", cause);
    }
  }

  subscribe(handler: (chunk: Uint8Array) => void): () => void {
    this.#handlers.add(handler);
    if (!this.#listening) {
      this.#listening = true;
      this.#tx.addEventListener("characteristicvaluechanged", this.#onNotify);
      void this.#tx.startNotifications();
    }
    return () => {
      this.#handlers.delete(handler);
    };
  }

  #onNotify = (event: Event): void => {
    const target = event.target as BluetoothRemoteGATTCharacteristic;
    const value = target.value;
    if (!value) return;
    const chunk = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    for (const handler of this.#handlers) {
      handler(chunk);
    }
  };

  async disconnect(): Promise<void> {
    this.#handlers.clear();
    this.#listening = false;
    this.#tx.removeEventListener("characteristicvaluechanged", this.#onNotify);
    this.#server.disconnect();
  }
}

export { MIN_MTU };
