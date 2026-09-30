/*
 * The phone-facing protocol on the firmware side, against the shared vectors.
 *
 * The same test-vectors/bridge_protocol.json is read by bridge/test on the
 * TypeScript side. Neither implementation produced them, so agreeing with them
 * means agreeing with each other -- which is what docs/bridge-protocol.md being
 * "normative and client-independent" has to mean in practice.
 *
 * docs/test-plan.md 6.5 and 6.6 are the hardware gates for chunking; the logic
 * they exercise is here.
 */

#include <cstring>
#include <initializer_list>

#include "doctest.h"

#include "ble/bridge_codec.h"
#include "test_vectors.h"

using namespace ble;

namespace {

const vectors::Vector *vec(const char *name)
{
    const vectors::Vector *found = vectors::find(name);
    CAPTURE(name);
    REQUIRE(found != nullptr);
    return found;
}

} // namespace

TEST_CASE("a short message becomes one chunk with both F and L set")
{
    const vectors::Vector *message = vec("chunk_single_message");
    const vectors::Vector *expected = vec("chunk_single_c0");

    Chunker chunker;
    REQUIRE(chunker.begin(message->bytes, message->length, 247, 5));
    CHECK(chunker.chunkCount() == 1);

    uint8_t chunk[64];
    size_t written = 0;
    REQUIRE(chunker.next(chunk, sizeof(chunk), &written));
    REQUIRE(written == expected->length);
    CHECK(std::memcmp(chunk, expected->bytes, written) == 0);

    // 0xC5 = F | L | msgId 5.
    CHECK((chunk[0] & kChunkFirst) != 0);
    CHECK((chunk[0] & kChunkLast) != 0);
    CHECK((chunk[0] & kMsgIdMask) == 5);
    CHECK(chunk[1] == 0);

    CHECK(chunker.done());
    CHECK_FALSE(chunker.next(chunk, sizeof(chunk), &written));
}

TEST_CASE("a long message at the minimum MTU splits exactly as the vectors say")
{
    // MTU 23 is what to assume before negotiation completes (1).
    const vectors::Vector *message = vec("chunk_three_message");
    const char *names[] = {"chunk_three_c0", "chunk_three_c1", "chunk_three_c2"};

    CHECK(maxFragmentBytes(23) == 18);

    Chunker chunker;
    REQUIRE(chunker.begin(message->bytes, message->length, 23, 9));
    CHECK(chunker.chunkCount() == 3);

    for (int i = 0; i < 3; ++i) {
        const vectors::Vector *expected = vec(names[i]);
        uint8_t chunk[64];
        size_t written = 0;
        CAPTURE(i);
        REQUIRE(chunker.next(chunk, sizeof(chunk), &written));
        REQUIRE(written == expected->length);
        CHECK(std::memcmp(chunk, expected->bytes, written) == 0);
        CHECK(chunk[1] == i);
    }
    CHECK(chunker.done());
}

TEST_CASE("chunks reassemble back into the message")
{
    const vectors::Vector *message = vec("chunk_three_message");
    const char *names[] = {"chunk_three_c0", "chunk_three_c1", "chunk_three_c2"};

    Reassembler assembler;
    CHECK(assembler.feed(vec(names[0])->bytes, vec(names[0])->length, 0) ==
          ReassembleResult::NeedMore);
    CHECK(assembler.feed(vec(names[1])->bytes, vec(names[1])->length, 100) ==
          ReassembleResult::NeedMore);
    REQUIRE(assembler.feed(vec(names[2])->bytes, vec(names[2])->length, 200) ==
            ReassembleResult::Complete);

    REQUIRE(assembler.messageLength() == message->length);
    CHECK(std::memcmp(assembler.message(), message->bytes, message->length) == 0);
    CHECK(assembler.msgId() == 9);
}

TEST_CASE("a message survives a chunker and reassembler round trip at every MTU")
{
    uint8_t message[1500];
    for (size_t i = 0; i < sizeof(message); ++i) {
        message[i] = static_cast<uint8_t>(i * 7u + 3u);
    }

    for (uint16_t mtu = kMinMtu; mtu <= kMaxMtu; mtu = static_cast<uint16_t>(mtu + 7)) {
        CAPTURE(mtu);
        Chunker chunker;
        REQUIRE(chunker.begin(message, sizeof(message), mtu, 17));

        Reassembler assembler;
        ReassembleResult last = ReassembleResult::NeedMore;
        uint8_t chunk[kMaxMtu];
        size_t written = 0;
        uint32_t now = 0;
        while (chunker.next(chunk, sizeof(chunk), &written)) {
            last = assembler.feed(chunk, written, now);
            now += 10;
        }
        REQUIRE(last == ReassembleResult::Complete);
        REQUIRE(assembler.messageLength() == sizeof(message));
        CHECK(std::memcmp(assembler.message(), message, sizeof(message)) == 0);
    }
}

TEST_CASE("a dropped middle chunk discards the message rather than half-applying it")
{
    // docs/test-plan.md 6.6. bridge-protocol.md 1.1: "It does not attempt
    // recovery; the sender re-sends the whole message."
    const char *names[] = {"chunk_three_c0", "chunk_three_c1", "chunk_three_c2"};

    Reassembler assembler;
    REQUIRE(assembler.feed(vec(names[0])->bytes, vec(names[0])->length, 0) ==
            ReassembleResult::NeedMore);
    // c1 never arrives.
    CHECK(assembler.feed(vec(names[2])->bytes, vec(names[2])->length, 50) ==
          ReassembleResult::Discarded);

    // And nothing partial is left behind for the next message to inherit.
    REQUIRE(assembler.feed(vec("chunk_single_c0")->bytes, vec("chunk_single_c0")->length, 60) ==
            ReassembleResult::Complete);
    CHECK(assembler.messageLength() == 2);
}

TEST_CASE("a msgId change mid-message discards it")
{
    // No concurrency: one message in flight per direction (1.1, 6).
    const vectors::Vector *c0 = vec("chunk_three_c0");
    const vectors::Vector *c1 = vec("chunk_three_c1");

    uint8_t interloper[64];
    std::memcpy(interloper, c1->bytes, c1->length);
    interloper[0] = static_cast<uint8_t>((interloper[0] & ~kMsgIdMask) | 11u);

    Reassembler assembler;
    REQUIRE(assembler.feed(c0->bytes, c0->length, 0) == ReassembleResult::NeedMore);
    CHECK(assembler.feed(interloper, c1->length, 10) == ReassembleResult::Discarded);
}

TEST_CASE("a partial message times out after five seconds")
{
    const vectors::Vector *c0 = vec("chunk_three_c0");
    const vectors::Vector *c1 = vec("chunk_three_c1");

    Reassembler assembler;
    REQUIRE(assembler.feed(c0->bytes, c0->length, 1000) == ReassembleResult::NeedMore);

    CHECK_FALSE(assembler.tick(1000 + kChunkTimeoutMs - 1));
    CHECK(assembler.tick(1000 + kChunkTimeoutMs));

    // The continuation now has nothing to continue.
    CHECK(assembler.feed(c1->bytes, c1->length, 1000 + kChunkTimeoutMs + 1) ==
          ReassembleResult::Discarded);
}

TEST_CASE("a continuation chunk with nothing in progress is discarded")
{
    const vectors::Vector *c1 = vec("chunk_three_c1");
    Reassembler assembler;
    CHECK(assembler.feed(c1->bytes, c1->length, 0) == ReassembleResult::Discarded);
}

TEST_CASE("the message layer matches the shared vectors")
{
    const vectors::Vector *expected = vec("rsp_err_not_authorised");

    const uint8_t body[2] = {0x0A, 0x04};  // PROVISION_KEY, ERR_NOT_AUTHORISED
    uint8_t out[16];
    const size_t written =
        encodeMessage(static_cast<uint8_t>(EventCode::ResponseError), 0x11, body, sizeof(body),
                      out, sizeof(out));
    REQUIRE(written == expected->length);
    CHECK(std::memcmp(out, expected->bytes, written) == 0);

    MessageView view{};
    REQUIRE(decodeMessage(expected->bytes, expected->length, &view));
    CHECK(view.opcode == static_cast<uint8_t>(EventCode::ResponseError));
    CHECK(view.txnId == 0x11);
    REQUIRE(view.bodyLen == 2);
    CHECK(view.body[0] == 0x0A);
    CHECK(view.body[1] == 0x04);
}

TEST_CASE("events carry txnId zero")
{
    // 1.2: "Events are unsolicited and carry txnId = 0."
    for (const char *name : {"evt_budget", "evt_frame_tx_result"}) {
        const vectors::Vector *event = vec(name);
        MessageView view{};
        REQUIRE(decodeMessage(event->bytes, event->length, &view));
        CAPTURE(name);
        CHECK(view.txnId == 0);
        CHECK((view.opcode & 0x80u) != 0);  // events and responses are 0x80+
    }
}

TEST_CASE("exactly three opcodes are open and everything else is bonded")
{
    /*
     * docs/bridge-protocol.md 2. The one that matters is PROVISION_KEY: an
     * unbonded write must not be able to set the network key.
     */
    CHECK(tierOf(static_cast<uint8_t>(Opcode::GetInfo)) == AuthTier::Open);
    CHECK(tierOf(static_cast<uint8_t>(Opcode::GetStatus)) == AuthTier::Open);
    CHECK(tierOf(static_cast<uint8_t>(Opcode::GetBudget)) == AuthTier::Open);

    CHECK(tierOf(static_cast<uint8_t>(Opcode::ProvisionKey)) == AuthTier::Bonded);
    CHECK(tierOf(static_cast<uint8_t>(Opcode::RotateKey)) == AuthTier::Bonded);
    CHECK(tierOf(static_cast<uint8_t>(Opcode::SetConfig)) == AuthTier::Bonded);
    CHECK(tierOf(static_cast<uint8_t>(Opcode::FactoryReset)) == AuthTier::Bonded);
    CHECK(tierOf(static_cast<uint8_t>(Opcode::SendText)) == AuthTier::Bonded);

    // Every opcode that is not one of the three open ones -- including the whole
    // reserved range and the Phase 8 tile opcodes -- is bonded by default.
    int open = 0;
    for (int opcode = 0; opcode <= 0xFF; ++opcode) {
        if (tierOf(static_cast<uint8_t>(opcode)) == AuthTier::Open) {
            ++open;
        }
    }
    CHECK(open == 3);
}

TEST_CASE("config TLVs walk, including the unknown one")
{
    const vectors::Vector *run = vec("set_config_tlv_run");

    struct Seen { uint8_t type; uint8_t length; };
    Seen seen[8];
    size_t count = 0;

    size_t cursor = 0;
    TlvView tlv{};
    while (nextTlv(run->bytes, run->length, &cursor, &tlv)) {
        REQUIRE(count < 8);
        seen[count].type = tlv.type;
        seen[count].length = tlv.length;
        ++count;
    }

    REQUIRE(count == 5);
    CHECK(seen[0].type == static_cast<uint8_t>(ConfigTlv::SniffIntervalMs));
    CHECK(seen[0].length == 2);
    CHECK(seen[1].type == static_cast<uint8_t>(ConfigTlv::Band));
    CHECK(seen[2].type == static_cast<uint8_t>(ConfigTlv::TxPowerDbm));
    CHECK(seen[3].type == static_cast<uint8_t>(ConfigTlv::TelemetryIntervalS));
    // The unknown type is returned like any other, so the caller can report it
    // as unapplied rather than rejecting the whole write (3).
    CHECK(seen[4].type == 0xF0);
    CHECK(seen[4].length == 2);
}

TEST_CASE("the SET_CONFIG body keeps its version prefix out of the TLV run")
{
    const vectors::Vector *body = vec("set_config_body");
    const vectors::Vector *run = vec("set_config_tlv_run");
    REQUIRE(body->length == run->length + 4);
    // configVersion u32 = 7, little endian.
    CHECK(body->bytes[0] == 0x07);
    CHECK(body->bytes[1] == 0x00);
    CHECK(std::memcmp(body->bytes + 4, run->bytes, run->length) == 0);
}

TEST_CASE("a truncated TLV run is refused rather than half-read")
{
    // A length that runs past the end of the body must not read beyond it.
    const uint8_t truncated[] = {0x02, 0x04, 0xD0, 0x07};  // says 4 bytes, has 2
    size_t cursor = 0;
    TlvView tlv{};
    CHECK_FALSE(nextTlv(truncated, sizeof(truncated), &cursor, &tlv));
}

TEST_CASE("a message longer than 4096 bytes is a protocol error")
{
    // 1.1: "Maximum reassembled message: 4096 bytes. Larger is a protocol error."
    uint8_t oversized[kMaxMessageBytes + 1] = {};
    Chunker chunker;
    CHECK_FALSE(chunker.begin(oversized, sizeof(oversized), 247, 0));
    CHECK(chunker.begin(oversized, kMaxMessageBytes, 247, 0));
}

TEST_CASE("EVT_JOURNAL carries the journal counter, and it is not the frame counter")
{
    /*
     * The same two vectors bridge/test and android/protocol read. Three
     * implementations agreeing with a file none of them produced is the only
     * thing that makes "docs/bridge-protocol.md is normative" checkable.
     *
     * The point of the first one is D10 made visible: the entry is 4098, the
     * frame it reports on is 1024. A decoder that read the body's counter as the
     * journal counter would acknowledge 1024, free entries nobody stored, and
     * pass every other vector in the file.
     */
    const vectors::Vector *wrapped = vec("evt_journal_wrapping_tx_result");
    ble::MessageView message{};
    REQUIRE(ble::decodeMessage(wrapped->bytes, wrapped->length, &message));
    CHECK(message.opcode == static_cast<uint8_t>(ble::EventCode::Journal));
    // Unsolicited, even though GET_QUEUE asked for it: section 1.2 gives every
    // event txnId 0, and a response carrying 0 would be unroutable.
    CHECK(message.txnId == 0);

    const uint32_t journalCounter = static_cast<uint32_t>(message.body[0])
                                    | (static_cast<uint32_t>(message.body[1]) << 8)
                                    | (static_cast<uint32_t>(message.body[2]) << 16)
                                    | (static_cast<uint32_t>(message.body[3]) << 24);
    CHECK(journalCounter == 4098u);
    CHECK(message.body[4] == static_cast<uint8_t>(ble::EventCode::FrameTxResult));
    CHECK(message.body[5] == 12);

    // The wrapped body is the other vector, byte for byte.
    const vectors::Vector *bare = vec("evt_frame_tx_result");
    ble::MessageView bareMessage{};
    REQUIRE(ble::decodeMessage(bare->bytes, bare->length, &bareMessage));
    REQUIRE(bareMessage.bodyLen == message.body[5]);
    for (size_t i = 0; i < bareMessage.bodyLen; ++i) {
        CHECK(message.body[ble::kJournalHeaderBytes + i] == bareMessage.body[i]);
    }

    const uint32_t frameCounter = static_cast<uint32_t>(bareMessage.body[0])
                                  | (static_cast<uint32_t>(bareMessage.body[1]) << 8)
                                  | (static_cast<uint32_t>(bareMessage.body[2]) << 16)
                                  | (static_cast<uint32_t>(bareMessage.body[3]) << 24);
    CHECK(frameCounter == 1024u);
    CHECK(frameCounter != journalCounter);
}

TEST_CASE("an EVT_JOURNAL wrapping an unknown opcode still parses")
{
    /*
     * Section 4: an unknown wrapped opcode is stored and counted towards
     * ACK_QUEUE anyway, because the counter is legible even when the body is
     * not. Dropping it would free journal space for something nobody ever saw --
     * which is what makes a future event type safe to add.
     */
    const vectors::Vector *unknown = vec("evt_journal_unknown_opcode");
    ble::MessageView message{};
    REQUIRE(ble::decodeMessage(unknown->bytes, unknown->length, &message));
    CHECK(message.opcode == static_cast<uint8_t>(ble::EventCode::Journal));
    CHECK(message.body[4] == 0x8F);
    CHECK(message.body[5] == 3);
    CHECK(message.bodyLen == ble::kJournalHeaderBytes + 3);
}
