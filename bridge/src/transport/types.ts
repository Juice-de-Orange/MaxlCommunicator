/**
 * The seam between the protocol and the radio underneath it.
 *
 * `docs/bridge-protocol.md` is client-independent and "nothing here may assume a
 * browser". This interface is where that stops being a slogan: the protocol code
 * and the connection lifecycle are written against `Transport`, and Web
 * Bluetooth is one implementation of it. The tests use another, which is how the
 * whole of §5 can be exercised without a device in the room.
 */

export interface Transport {
  /** Negotiated ATT MTU. Assume 23 until negotiation completes (§1). */
  readonly mtu: number;

  /** Whether the connection is bonded, which gates the bonded tier (§2). */
  readonly bonded: boolean;

  /** Write one chunk to the `RX` characteristic. */
  write(chunk: Uint8Array): Promise<void>;

  /** Subscribe to `TX` notifications. Returns an unsubscribe function. */
  subscribe(handler: (chunk: Uint8Array) => void): () => void;

  disconnect(): Promise<void>;
}

export class TransportError extends Error {
  constructor(message: string, override readonly cause?: unknown) {
    super(message);
    this.name = "TransportError";
  }
}
