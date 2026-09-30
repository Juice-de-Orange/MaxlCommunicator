/*
 * The bridge protocol, device side.
 *
 * Everything from docs/bridge-protocol.md that is not the radio: reassembling
 * chunks into a message, enforcing the authorisation tier, dispatching to the
 * application through ble::IHost, and chunking the reply back out.
 *
 * Nothing here knows about Bluefruit, the SoftDevice or an nRF52. The BLE stack
 * sits behind hal::IBleTransport (decision D3), which is what lets gates 6.3 to
 * 6.6 be exercised on a host:
 *
 *   6.3  unbonded PROVISION_KEY -> ERR_NOT_AUTHORISED, key unchanged
 *   6.4  unbonded GET_INFO / GET_STATUS -> succeed
 *   6.5  max-size message chunking, 4096 B, reassembled correctly
 *   6.6  a dropped middle chunk -> discarded cleanly, no partial application
 *
 * 6.6 is the one worth being careful about. "No partial application" means a
 * message that never completed must not have had any of its effect, and the only
 * way to guarantee that is to apply nothing until the whole message is in hand.
 * So dispatch happens after reassembly and never during it.
 */

#ifndef MAXL_BLE_GATT_SERVER_H
#define MAXL_BLE_GATT_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include "ble/bridge_codec.h"
#include "ble/i_host.h"
#include "hal/i_ble_transport.h"

namespace ble {

/*
 * Response bodies are small -- the longest is the 17-byte status body and a
 * handful of config TLVs. Sizing these buffers at kMaxMessageBytes would put two
 * 4 KB arrays on a FreeRTOS task stack that the Adafruit core gives 1 KB by
 * default, for messages that never come close.
 *
 * The 4 KB limit is about what may be *received* -- a tile in phase 8 -- and
 * that buffer lives in the Reassembler, once.
 */
inline constexpr size_t kMaxResponseBytes = 256;

/*
 * Derived from hal::IBleChunkSink, not merely shaped like it.
 *
 * The transport's header used to say "ble::GattServer implements this shape",
 * and it did -- by coincidence, checked by nobody, because until src/main.cpp
 * nothing ever handed one to the other. It did not compile, which is the good
 * outcome.
 */
class GattServer : public hal::IBleChunkSink {
public:
    GattServer(hal::IBleTransport &transport, IHost &host) : transport_(transport), host_(host) {}

    /// Feed one chunk from the RX characteristic.
    void onChunk(const uint8_t *chunk, size_t length, uint32_t nowMs) override;

    /// Drop a partial message and forget the connection's state.
    void onDisconnect() override;

    /// Time out a partial message that has gone quiet, and refresh the STATUS
    /// characteristic. Call from the main loop.
    void tick(uint32_t nowMs);

    /// Emit an unsolicited event (txnId 0). Used for EVT_FRAME_RX and friends.
    bool emitEvent(EventCode code, const uint8_t *body, size_t length);

    /// How many messages were discarded because a chunk went missing. Gate 6.6
    /// counts these, and so does anyone wondering why a phone keeps retrying.
    uint32_t discardedMessages() const { return discarded_; }

    /// How many bonded-tier commands arrived on an unbonded link. Not an error
    /// path worth hiding: a non-zero count on a shipped device is somebody
    /// probing it.
    uint32_t rejectedUnauthorised() const { return rejected_; }

private:
    hal::IBleTransport &transport_;
    IHost &host_;
    Reassembler reassembler_;
    Chunker chunker_;
    uint8_t outMessage_[kMaxResponseBytes + 8];
    /*
     * Where a journal entry is wrapped before it goes out (EVT_JOURNAL, D15).
     *
     * A member and not a local: as a local it took dispatch()'s frame to 1168
     * bytes and -Wframe-larger-than=1024 refused the build -- which is the flag
     * doing its job, since the loop task has 4 KB in total. Here it costs the
     * same 261 bytes, once, and says so in the map file.
     *
     * 255 because the length field is a u8. Nothing the journal holds comes
     * close, but a bound that cannot be exceeded needs no reasoning about what
     * the journal holds today.
     */
    uint8_t journalScratch_[kJournalHeaderBytes + 255];
    uint8_t txMsgId_ = 0;
    uint32_t discarded_ = 0;
    uint32_t rejected_ = 0;
    uint32_t lastStatusMs_ = 0;
    bool statusEverPublished_ = false;

    void dispatch(const MessageView &message);
    bool respondOk(uint8_t opcode, uint8_t txnId, const uint8_t *body, size_t length);
    bool respondError(uint8_t opcode, uint8_t txnId, BridgeError error);
    bool sendMessage(uint8_t opcode, uint8_t txnId, const uint8_t *body, size_t length);
};

} // namespace ble

#endif // MAXL_BLE_GATT_SERVER_H
