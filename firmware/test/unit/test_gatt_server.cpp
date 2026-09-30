/*
 * The bridge protocol, device side.
 *
 * Four gates from docs/test-plan.md phase 6 are reachable here without a radio,
 * because the BLE stack sits behind hal::IBleTransport (decision D3):
 *
 *   6.3  unbonded PROVISION_KEY -> ERR_NOT_AUTHORISED, key unchanged
 *   6.4  unbonded GET_INFO / GET_STATUS -> succeed
 *   6.5  max-size message chunking, reassembled correctly
 *   6.6  a dropped middle chunk -> discarded cleanly, no partial application
 *
 * They are not *claimed* as passed -- 6.1, 6.2 and 6.7 need a device, and a
 * phase-6 gate list with four ticks and a note saying "on a fake" would be
 * misleading. What this proves is that the logic is right before anyone spends
 * bench time on it.
 */

#include "doctest.h"

#include <cstring>
#include <string>
#include <vector>

#include "ble/bridge_codec.h"
#include "ble/gatt_server.h"
#include "fakes/fake_ble.h"

using namespace ble;

namespace {

struct Fixture {
    fakes::FakeBleTransport transport;
    fakes::FakeHost host;
    GattServer server{transport, host};

    /// Send a complete command, chunked exactly as a phone would.
    void send(Opcode opcode, const std::vector<uint8_t> &body, uint8_t txnId = 7,
              uint32_t nowMs = 1000)
    {
        uint8_t message[kMaxMessageBytes];
        const size_t total = encodeMessage(static_cast<uint8_t>(opcode), txnId, body.data(),
                                           body.size(), message, sizeof(message));
        REQUIRE(total > 0);

        Chunker chunker;
        REQUIRE(chunker.begin(message, total, transport.negotiatedMtu, 1));
        uint8_t chunk[kMaxMtu];
        size_t written = 0;
        while (chunker.next(chunk, sizeof(chunk), &written)) {
            server.onChunk(chunk, written, nowMs);
        }
    }

    /// The last message the phone would have received, decoded.
    MessageView lastMessage(std::vector<uint8_t> &storage) const
    {
        const auto all = transport.messages();
        REQUIRE(!all.empty());
        storage = all.back();
        MessageView view{};
        REQUIRE(decodeMessage(storage.data(), storage.size(), &view));
        return view;
    }
};

std::vector<uint8_t> u32le(uint32_t value)
{
    return {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
            static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
}

} // namespace

TEST_CASE("gate 6.4 -- open-tier commands work on an unbonded connection")
{
    Fixture fixture;
    fixture.transport.isBonded = false;

    SUBCASE("GET_INFO")
    {
        fixture.send(Opcode::GetInfo, {});
        std::vector<uint8_t> storage;
        const MessageView reply = fixture.lastMessage(storage);
        CHECK(reply.opcode == static_cast<uint8_t>(EventCode::ResponseOk));
        CHECK(reply.txnId == 7);
        CHECK(reply.body[0] == static_cast<uint8_t>(Opcode::GetInfo));
        CHECK(reply.body[1] == 1); // BRIDGE_PROTO
    }

    SUBCASE("GET_STATUS")
    {
        fixture.send(Opcode::GetStatus, {});
        std::vector<uint8_t> storage;
        const MessageView reply = fixture.lastMessage(storage);
        CHECK(reply.opcode == static_cast<uint8_t>(EventCode::ResponseOk));
        CHECK(reply.bodyLen == 1u + 17u); // echoed opcode plus the status body
    }

    SUBCASE("GET_BUDGET")
    {
        // A phone that cannot read the budget cannot explain a silent node.
        fixture.send(Opcode::GetBudget, {});
        std::vector<uint8_t> storage;
        const MessageView reply = fixture.lastMessage(storage);
        CHECK(reply.opcode == static_cast<uint8_t>(EventCode::ResponseOk));
    }

    CHECK(fixture.server.rejectedUnauthorised() == 0);
}

TEST_CASE("gate 6.3 -- bonded-tier commands are refused on an unbonded connection")
{
    Fixture fixture;
    fixture.transport.isBonded = false;

    std::vector<uint8_t> body = {0, 42};
    for (int i = 0; i < 16; ++i) {
        body.push_back(static_cast<uint8_t>(0xA0 + i));
    }
    fixture.send(Opcode::ProvisionKey, body);

    std::vector<uint8_t> storage;
    const MessageView reply = fixture.lastMessage(storage);
    CHECK(reply.opcode == static_cast<uint8_t>(EventCode::ResponseError));
    CHECK(reply.body[0] == static_cast<uint8_t>(Opcode::ProvisionKey));
    CHECK(reply.body[1] == static_cast<uint8_t>(BridgeError::NotAuthorised));

    // "The key unchanged" is the half that matters. An error returned after the
    // key was already written would pass a test that only looked at the reply.
    CHECK(fixture.host.provisioned.empty());
    CHECK(fixture.server.rejectedUnauthorised() == 1);
}

TEST_CASE("every bonded-tier command is refused unbonded, and none of them reach the host")
{
    Fixture fixture;
    fixture.transport.isBonded = false;

    fixture.send(Opcode::SendText, {2, 0, 2, 'h', 'i'});
    fixture.send(Opcode::AckQueue, u32le(5));
    fixture.send(Opcode::SetTime, u32le(1788126105));
    fixture.send(Opcode::RotateKey, {1});
    fixture.send(Opcode::FactoryReset, u32le(0x5245534D));
    fixture.send(Opcode::LinkTest, {2, 0, 5, 9});
    fixture.send(Opcode::RequestFix, {60, 0});

    CHECK(fixture.host.sentTexts.empty());
    CHECK(fixture.host.acked.empty());
    CHECK(fixture.host.timesSet.empty());
    CHECK(fixture.host.rotations.empty());
    CHECK(fixture.host.factoryResets == 0);
    CHECK(fixture.host.linkTests.empty());
    CHECK(fixture.host.fixRequests.empty());
    CHECK(fixture.server.rejectedUnauthorised() == 7);

    // And it is refused rather than ignored -- section 2: "It does not silently
    // no-op." Every one produced a reply.
    CHECK(fixture.transport.messages().size() == 7);
}

TEST_CASE("an unknown opcode is answered, not ignored")
{
    Fixture fixture;
    fixture.transport.isBonded = true;

    uint8_t message[8];
    const size_t total = encodeMessage(0x7E, 3, nullptr, 0, message, sizeof(message));
    Chunker chunker;
    chunker.begin(message, total, 247, 1);
    uint8_t chunk[kMaxMtu];
    size_t written = 0;
    while (chunker.next(chunk, sizeof(chunk), &written)) {
        fixture.server.onChunk(chunk, written, 1000);
    }

    std::vector<uint8_t> storage;
    const MessageView reply = fixture.lastMessage(storage);
    CHECK(reply.opcode == static_cast<uint8_t>(EventCode::ResponseError));
    CHECK(reply.body[1] == static_cast<uint8_t>(BridgeError::Unsupported));
}

TEST_CASE("an unknown opcode defaults to the bonded tier")
{
    // tierOf() is deliberately conservative: adding an opcode to the protocol
    // and forgetting the dispatcher must leave it unreachable, not unguarded.
    Fixture fixture;
    fixture.transport.isBonded = false;

    uint8_t message[8];
    const size_t total = encodeMessage(0x7E, 3, nullptr, 0, message, sizeof(message));
    Chunker chunker;
    chunker.begin(message, total, 247, 1);
    uint8_t chunk[kMaxMtu];
    size_t written = 0;
    while (chunker.next(chunk, sizeof(chunk), &written)) {
        fixture.server.onChunk(chunk, written, 1000);
    }

    std::vector<uint8_t> storage;
    const MessageView reply = fixture.lastMessage(storage);
    CHECK(reply.body[1] == static_cast<uint8_t>(BridgeError::NotAuthorised));
}

TEST_CASE("bonded commands reach the host with their arguments intact")
{
    Fixture fixture;
    fixture.transport.isBonded = true;

    SUBCASE("SEND_TEXT")
    {
        fixture.send(Opcode::SendText, {0x02, 0x00, 5, 'H', 'e', 'l', 'l', 'o'});
        REQUIRE(fixture.host.sentTexts.size() == 1);
        CHECK(fixture.host.lastDst == 2);
        CHECK(std::string(fixture.host.sentTexts[0].begin(), fixture.host.sentTexts[0].end())
              == "Hello");
    }

    SUBCASE("PROVISION_KEY")
    {
        std::vector<uint8_t> body = {1, 0x2A};
        for (int i = 0; i < 16; ++i) {
            body.push_back(static_cast<uint8_t>(i));
        }
        fixture.send(Opcode::ProvisionKey, body);
        REQUIRE(fixture.host.provisioned.size() == 1);
        CHECK(fixture.host.lastSlot == 1);
        CHECK(fixture.host.lastNetId == 0x2A);
        CHECK(fixture.host.provisioned[0][15] == 15);
    }

    SUBCASE("FACTORY_RESET needs its magic")
    {
        fixture.send(Opcode::FactoryReset, u32le(0x11223344));
        CHECK(fixture.host.factoryResets == 0);
        std::vector<uint8_t> storage;
        CHECK(fixture.lastMessage(storage).body[1] == static_cast<uint8_t>(BridgeError::BadParam));

        fixture.send(Opcode::FactoryReset, u32le(0x5245534D));
        CHECK(fixture.host.factoryResets == 1);
    }

    SUBCASE("a wrong body length is a length error, not a crash")
    {
        fixture.send(Opcode::SetTime, {1, 2});
        std::vector<uint8_t> storage;
        CHECK(fixture.lastMessage(storage).body[1] == static_cast<uint8_t>(BridgeError::BadLength));
        CHECK(fixture.host.timesSet.empty());
    }

    SUBCASE("SEND_TEXT with a length that disagrees with the body")
    {
        // Claims 40 bytes of text and carries two. Reading the claim would walk
        // off the end of the message.
        fixture.send(Opcode::SendText, {0x02, 0x00, 40, 'h', 'i'});
        std::vector<uint8_t> storage;
        CHECK(fixture.lastMessage(storage).body[1] == static_cast<uint8_t>(BridgeError::BadLength));
        CHECK(fixture.host.sentTexts.empty());
    }

    SUBCASE("SET_CONFIG is acknowledged with what was applied and what was not")
    {
        std::vector<uint8_t> body = u32le(7);
        body.insert(body.end(), {0x02, 0x02, 0xD0, 0x07}); // sniffIntervalMs = 2000
        fixture.send(Opcode::SetConfig, body);

        REQUIRE(fixture.host.configVersions.size() == 1);
        CHECK(fixture.host.configVersions[0] == 7);

        // The response, then EVT_CONFIG_APPLIED. CLAUDE.md 4.3: a pushed config
        // is not an applied config, and the dashboard sets applied_at from this
        // event and from nothing else.
        const auto messages = fixture.transport.messages();
        REQUIRE(messages.size() == 2);
        MessageView applied{};
        REQUIRE(decodeMessage(messages[1].data(), messages[1].size(), &applied));
        CHECK(applied.opcode == static_cast<uint8_t>(EventCode::ConfigApplied));
        CHECK(applied.txnId == 0); // events are unsolicited
        CHECK(applied.body[0] == 7);
        CHECK(applied.body[4] == 0b0011);
        CHECK(applied.body[8] == 0x09); // the TLV the node did not understand
    }
}

TEST_CASE("GET_QUEUE emits the events and then says how many")
{
    Fixture fixture;
    fixture.transport.isBonded = true;
    for (uint32_t i = 1; i <= 5; ++i) {
        fixture.host.journal.push_back({i, 0x81, {static_cast<uint8_t>(i), 0xAA}});
    }

    std::vector<uint8_t> body = u32le(2);
    body.push_back(10); // maxEvents
    fixture.send(Opcode::GetQueue, body);

    const auto messages = fixture.transport.messages();
    // Three events (counters 3, 4, 5), then the response.
    REQUIRE(messages.size() == 4);
    CHECK(fixture.host.lastSince == 2);

    /*
     * Wrapped in EVT_JOURNAL since BRIDGE_PROTO 2 -- decision D15.
     *
     * The wrapper is what carries the journal counter, and without it no client
     * could form ACK_QUEUE at all: the counter was computed in fetchQueue and
     * dropped before transmission. The wrapped body must come back byte for
     * byte, so the client can hand it to the decoder it already has.
     */
    for (size_t i = 0; i < 3; ++i) {
        MessageView event{};
        REQUIRE(decodeMessage(messages[i].data(), messages[i].size(), &event));
        CHECK(event.opcode == static_cast<uint8_t>(EventCode::Journal));
        CHECK(event.txnId == 0);

        const uint32_t counter = static_cast<uint32_t>(event.body[0])
                                 | (static_cast<uint32_t>(event.body[1]) << 8)
                                 | (static_cast<uint32_t>(event.body[2]) << 16)
                                 | (static_cast<uint32_t>(event.body[3]) << 24);
        CHECK(counter == 3 + i);
        CHECK(event.body[4] == 0x81);  // the wrapped opcode
        CHECK(event.body[5] == 2);     // its body length
        CHECK(event.body[kJournalHeaderBytes] == 3 + i);
        CHECK(event.body[kJournalHeaderBytes + 1] == 0xAA);
    }

    MessageView reply{};
    REQUIRE(decodeMessage(messages[3].data(), messages[3].size(), &reply));
    CHECK(reply.opcode == static_cast<uint8_t>(EventCode::ResponseOk));
    CHECK(reply.body[1] == 3);
}

TEST_CASE("gate 6.5 -- a large message survives chunking, at both MTUs")
{
    for (const uint16_t mtu : {kMinMtu, kMaxMtu}) {
        CAPTURE(mtu);
        Fixture fixture;
        fixture.transport.isBonded = true;
        fixture.transport.negotiatedMtu = mtu;

        // The largest thing a phone actually sends: a config blob of many TLVs.
        std::vector<uint8_t> body = u32le(9);
        for (int i = 0; i < 200; ++i) {
            body.insert(body.end(), {0x02, 0x02, 0xD0, 0x07});
        }
        fixture.send(Opcode::SetConfig, body);

        REQUIRE(fixture.host.configBlobs.size() == 1);
        CHECK(fixture.host.configBlobs[0].size() == 800);
        // Reassembly is byte-exact, not merely the right length.
        for (size_t i = 0; i < 800; i += 4) {
            CHECK(fixture.host.configBlobs[0][i] == 0x02);
            CHECK(fixture.host.configBlobs[0][i + 3] == 0x07);
        }
    }
}

TEST_CASE("gate 6.6 -- a dropped middle chunk applies nothing")
{
    Fixture fixture;
    fixture.transport.isBonded = true;
    fixture.transport.negotiatedMtu = kMinMtu; // force many small chunks

    std::vector<uint8_t> body = u32le(11);
    for (int i = 0; i < 40; ++i) {
        body.insert(body.end(), {0x02, 0x02, 0xD0, 0x07});
    }

    uint8_t message[kMaxMessageBytes];
    const size_t total = encodeMessage(static_cast<uint8_t>(Opcode::SetConfig), 5, body.data(),
                                       body.size(), message, sizeof(message));
    Chunker chunker;
    REQUIRE(chunker.begin(message, total, kMinMtu, 1));

    std::vector<std::vector<uint8_t>> chunks;
    uint8_t chunk[kMaxMtu];
    size_t written = 0;
    while (chunker.next(chunk, sizeof(chunk), &written)) {
        chunks.emplace_back(chunk, chunk + written);
    }
    REQUIRE(chunks.size() > 4);

    // Everything except one in the middle.
    const size_t dropped = chunks.size() / 2;
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (i == dropped) {
            continue;
        }
        fixture.server.onChunk(chunks[i].data(), chunks[i].size(), 1000);
    }

    CHECK(fixture.host.configBlobs.empty());
    CHECK(fixture.host.configVersions.empty());
    CHECK(fixture.server.discardedMessages() >= 1);

    // And the next complete message still works -- the discard did not wedge it.
    fixture.transport.clear();
    fixture.send(Opcode::GetInfo, {}, 6, 2000);
    std::vector<uint8_t> storage;
    CHECK(fixture.lastMessage(storage).opcode == static_cast<uint8_t>(EventCode::ResponseOk));
}

TEST_CASE("a partial message that goes quiet is timed out")
{
    Fixture fixture;
    fixture.transport.isBonded = true;
    fixture.transport.negotiatedMtu = kMinMtu;

    std::vector<uint8_t> body = u32le(1);
    for (int i = 0; i < 40; ++i) {
        body.insert(body.end(), {0x02, 0x02, 0xD0, 0x07});
    }
    uint8_t message[kMaxMessageBytes];
    const size_t total = encodeMessage(static_cast<uint8_t>(Opcode::SetConfig), 5, body.data(),
                                       body.size(), message, sizeof(message));
    Chunker chunker;
    chunker.begin(message, total, kMinMtu, 1);

    uint8_t chunk[kMaxMtu];
    size_t written = 0;
    chunker.next(chunk, sizeof(chunk), &written);
    fixture.server.onChunk(chunk, written, 1000);

    // Section 1.1: "if 5 s elapse between chunks" the partial message is dropped.
    fixture.server.tick(1000 + 6000);
    CHECK(fixture.server.discardedMessages() == 1);
    CHECK(fixture.host.configBlobs.empty());
}

TEST_CASE("the STATUS characteristic is published for a generic BLE tool")
{
    // Section 1: CONFIG and STATUS "exist so a generic BLE tool (nRF Connect)
    // can inspect a node without implementing this protocol".
    Fixture fixture;
    fixture.server.tick(0);
    CHECK(fixture.transport.statusPublishes == 1);
    CHECK(fixture.transport.published.size() == 17);

    fixture.server.tick(1000);
    CHECK(fixture.transport.statusPublishes == 1); // not on every loop pass
    fixture.server.tick(6000);
    CHECK(fixture.transport.statusPublishes == 2);
}

TEST_CASE("a disconnect drops any partial message")
{
    Fixture fixture;
    fixture.transport.isBonded = true;
    fixture.transport.negotiatedMtu = kMinMtu;

    std::vector<uint8_t> body = u32le(1);
    for (int i = 0; i < 40; ++i) {
        body.insert(body.end(), {0x02, 0x02, 0xD0, 0x07});
    }
    uint8_t message[kMaxMessageBytes];
    const size_t total = encodeMessage(static_cast<uint8_t>(Opcode::SetConfig), 5, body.data(),
                                       body.size(), message, sizeof(message));
    Chunker chunker;
    chunker.begin(message, total, kMinMtu, 1);
    uint8_t chunk[kMaxMtu];
    size_t written = 0;
    chunker.next(chunk, sizeof(chunk), &written);
    fixture.server.onChunk(chunk, written, 1000);

    fixture.server.onDisconnect();

    // The rest of the old message arriving on a new connection must not be
    // stitched onto what came before it.
    while (chunker.next(chunk, sizeof(chunk), &written)) {
        fixture.server.onChunk(chunk, written, 2000);
    }
    CHECK(fixture.host.configBlobs.empty());
}
