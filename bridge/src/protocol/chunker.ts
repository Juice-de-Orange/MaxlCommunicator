/**
 * Chunking and reassembly. `docs/bridge-protocol.md` §1.1.
 *
 * ```
 * byte 0 : F(0x80) | L(0x40) | msgId(0x3F)
 * byte 1 : chunkIndex (0..255)
 * byte 2..: fragment
 * ```
 *
 * `msgId` is a 6-bit rolling counter per direction. It exists to detect
 * interleaving and loss, **not** to allow concurrency -- one message in flight
 * per direction at a time.
 */

export const CHUNK_FIRST = 0x80;
export const CHUNK_LAST = 0x40;
export const MSG_ID_MASK = 0x3f;
export const CHUNK_HEADER_BYTES = 2;

/** "Maximum reassembled message: 4096 bytes. Larger is a protocol error." */
export const MAX_MESSAGE_BYTES = 4096;

/** "Assume 23 until negotiation completes"; "MTU is negotiated up to 247." */
export const MIN_MTU = 23;
export const MAX_MTU = 247;

/** "if 5 s elapse between chunks" the partial message is dropped. */
export const CHUNK_TIMEOUT_MS = 5000;

/**
 * "chunks of at most MTU - 3 - 2 payload bytes" -- three for the ATT header,
 * two for the chunk header above.
 */
export function maxFragmentBytes(mtu: number): number {
  return Math.max(mtu, MIN_MTU) - 3 - CHUNK_HEADER_BYTES;
}

/** Split a message into chunks ready to write to the `RX` characteristic. */
export function chunk(message: Uint8Array, mtu: number, msgId: number): Uint8Array[] {
  if (message.length === 0) {
    throw new RangeError("refusing to chunk an empty message");
  }
  if (message.length > MAX_MESSAGE_BYTES) {
    throw new RangeError(
      `message is ${message.length} bytes; the protocol limit is ${MAX_MESSAGE_BYTES}`,
    );
  }

  const fragmentBytes = maxFragmentBytes(mtu);
  const id = msgId & MSG_ID_MASK;
  const chunks: Uint8Array[] = [];

  for (let offset = 0; offset < message.length; offset += fragmentBytes) {
    const take = Math.min(fragmentBytes, message.length - offset);
    const isFirst = offset === 0;
    const isLast = offset + take === message.length;

    const out = new Uint8Array(CHUNK_HEADER_BYTES + take);
    out[0] = id | (isFirst ? CHUNK_FIRST : 0) | (isLast ? CHUNK_LAST : 0);
    out[1] = chunks.length;
    out.set(message.subarray(offset, offset + take), CHUNK_HEADER_BYTES);
    chunks.push(out);
  }

  if (chunks.length > 256) {
    // chunkIndex is one byte. Unreachable through the documented path -- at the
    // 23-byte minimum MTU, 256 fragments of 18 bytes is 4608, above the 4096
    // message limit -- so this is a guard, not a case.
    throw new RangeError("message needs more than 256 chunks");
  }
  return chunks;
}

export type ReassembleOutcome =
  | { readonly kind: "need-more" }
  | { readonly kind: "complete"; readonly message: Uint8Array; readonly msgId: number }
  | { readonly kind: "discarded"; readonly reason: string };

/**
 * Collects chunks back into messages.
 *
 * On anything unexpected the partial message is dropped and the sender is
 * expected to re-send the whole thing. §1.1: "It does not attempt recovery."
 * Half-applying a truncated `SET_CONFIG` would be far worse than losing it.
 */
export class Reassembler {
  #parts: Uint8Array[] = [];
  #length = 0;
  #msgId = -1;
  #expectedIndex = 0;
  #lastChunkAtMs = 0;
  #inProgress = false;

  reset(): void {
    this.#parts = [];
    this.#length = 0;
    this.#expectedIndex = 0;
    this.#inProgress = false;
  }

  get inProgress(): boolean {
    return this.#inProgress;
  }

  /** Drop a partial message that has gone quiet. Returns true if one was dropped. */
  tick(nowMs: number): boolean {
    if (!this.#inProgress) return false;
    if (nowMs - this.#lastChunkAtMs < CHUNK_TIMEOUT_MS) return false;
    this.reset();
    return true;
  }

  feed(chunkBytes: Uint8Array, nowMs: number): ReassembleOutcome {
    if (chunkBytes.length < CHUNK_HEADER_BYTES) {
      return { kind: "discarded", reason: "chunk shorter than its header" };
    }

    const flags = chunkBytes[0]!;
    const index = chunkBytes[1]!;
    const isFirst = (flags & CHUNK_FIRST) !== 0;
    const isLast = (flags & CHUNK_LAST) !== 0;
    const msgId = flags & MSG_ID_MASK;
    const fragment = chunkBytes.subarray(CHUNK_HEADER_BYTES);

    // Time out a stale partial before this chunk is considered, so a late
    // fragment cannot be stitched onto a message from five seconds ago.
    this.tick(nowMs);

    if (isFirst) {
      // A new first chunk replaces whatever was in progress: the sender only
      // ever re-sends a whole message, so a new start means the old one is gone.
      this.reset();
      this.#msgId = msgId;
      this.#inProgress = true;
      this.#expectedIndex = 0;
    } else if (!this.#inProgress) {
      return { kind: "discarded", reason: "continuation with nothing in progress" };
    }

    if (msgId !== this.#msgId) {
      this.reset();
      return { kind: "discarded", reason: "msgId changed mid-message" };
    }
    if (index !== this.#expectedIndex) {
      this.reset();
      return {
        kind: "discarded",
        reason: `chunk out of order: expected ${this.#expectedIndex}, got ${index}`,
      };
    }
    if (this.#length + fragment.length > MAX_MESSAGE_BYTES) {
      this.reset();
      return { kind: "discarded", reason: "message exceeds 4096 bytes" };
    }

    this.#parts.push(new Uint8Array(fragment));
    this.#length += fragment.length;
    this.#expectedIndex += 1;
    this.#lastChunkAtMs = nowMs;

    if (!isLast) {
      return { kind: "need-more" };
    }

    const message = new Uint8Array(this.#length);
    let offset = 0;
    for (const part of this.#parts) {
      message.set(part, offset);
      offset += part.length;
    }
    const completedId = this.#msgId;
    this.reset();
    return { kind: "complete", message, msgId: completedId };
  }
}

/** The 6-bit per-direction rolling counter from §1.1. */
export class MsgIdCounter {
  #next = 0;

  take(): number {
    const value = this.#next;
    this.#next = (this.#next + 1) & MSG_ID_MASK;
    return value;
  }
}
