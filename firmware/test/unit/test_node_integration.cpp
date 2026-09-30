/*
 * The device-side stack, end to end, with no device.
 *
 * A phone's chunks go in at hal::IBleTransport and come out as entries in a
 * persisted queue, a journal keyed on a persistent counter, and a duty cycle
 * budget that refuses. Every layer is the shipping one; only the flash, the
 * clock and the BLE link are fakes.
 *
 * This is the test the previous sessions could not write, because until app/node
 * existed the four layers had never been introduced to each other. An interface
 * nobody implements is a guess about what will be needed, and three of these
 * interfaces were exactly that until now.
 */

#include "doctest.h"

#include <cstring>
#include <string>
#include <vector>

#include "app/node.h"
#include "ble/bridge_codec.h"
#include "ble/gatt_server.h"
#include "fakes/fake_ble.h"
#include "fakes/fake_block_store.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_key_store.h"

namespace {

struct Stack {
    fakes::FakeClock clock;
    fakes::FakeBlockStore store;
    fakes::FakeBleTransport transport;
    fakes::FakeKeyStore keys;
    app::Node node{clock, store, keys};
    ble::GattServer server{transport, node};

    Stack()
    {
        // A real timestamp, taken from the RTC bring-up on node A.
        clock.setUnix(1788126105);
        REQUIRE(node.begin(0x0001));
    }

    void send(ble::Opcode opcode, const std::vector<uint8_t> &body, uint8_t txnId = 1)
    {
        uint8_t message[ble::kMaxMessageBytes];
        const size_t total = ble::encodeMessage(static_cast<uint8_t>(opcode), txnId, body.data(),
                                                body.size(), message, sizeof(message));
        REQUIRE(total > 0);
        ble::Chunker chunker;
        REQUIRE(chunker.begin(message, total, transport.negotiatedMtu, 1));
        uint8_t chunk[ble::kMaxMtu];
        size_t written = 0;
        while (chunker.next(chunk, sizeof(chunk), &written)) {
            server.onChunk(chunk, written, clock.monotonicMs());
        }
    }

    ble::MessageView lastReply(std::vector<uint8_t> &storage) const
    {
        const auto all = transport.messages();
        REQUIRE(!all.empty());
        storage = all.back();
        ble::MessageView view{};
        REQUIRE(ble::decodeMessage(storage.data(), storage.size(), &view));
        return view;
    }

    /// Provision a key so the node considers itself able to transmit.
    void provision()
    {
        transport.isBonded = true;
        std::vector<uint8_t> body = {0, 0x2A};
        for (int i = 0; i < 16; ++i) {
            body.push_back(static_cast<uint8_t>(0x10 + i));
        }
        send(ble::Opcode::ProvisionKey, body);
        transport.clear();
    }
};

std::vector<uint8_t> textCommand(uint16_t dst, const std::string &text)
{
    std::vector<uint8_t> body = {static_cast<uint8_t>(dst), static_cast<uint8_t>(dst >> 8),
                                 static_cast<uint8_t>(text.size())};
    body.insert(body.end(), text.begin(), text.end());
    return body;
}

} // namespace

TEST_CASE("a text from the phone reaches the persistent queue")
{
    Stack stack;
    stack.provision();

    stack.send(ble::Opcode::SendText, textCommand(2, "at the summit"));

    std::vector<uint8_t> storage;
    CHECK(stack.lastReply(storage).opcode == static_cast<uint8_t>(ble::EventCode::ResponseOk));
    REQUIRE(stack.node.queue().size() == 1);
    CHECK(stack.node.queue().at(0).dst == 2);
    CHECK(std::string(stack.node.queue().at(0).text, stack.node.queue().at(0).textLen)
          == "at the summit");

    // And it is on flash, not only in RAM: a fresh queue over the same store
    // finds it, which is what a reboot does.
    app::MessageQueue reloaded(stack.store);
    CHECK(reloaded.restore() == 1);
}

TEST_CASE("a node with no key refuses to accept a message, and says why")
{
    Stack stack;
    stack.transport.isBonded = true;
    // CLAUDE.md 2.1: no key, nothing to authenticate a frame with.
    stack.send(ble::Opcode::SendText, textCommand(2, "won't work"));

    std::vector<uint8_t> storage;
    const ble::MessageView reply = stack.lastReply(storage);
    CHECK(reply.opcode == static_cast<uint8_t>(ble::EventCode::ResponseError));
    CHECK(reply.body[1] == static_cast<uint8_t>(ble::BridgeError::NoKey));
    CHECK(stack.node.queue().empty());
}

TEST_CASE("a node with no valid time is transmit-blocked and reports it")
{
    Stack stack;
    stack.provision();
    stack.clock.invalidate();

    CHECK_FALSE(stack.node.transmitAllowed());

    stack.send(ble::Opcode::SendText, textCommand(2, "keine Zeit"));
    std::vector<uint8_t> storage;
    const ble::MessageView reply = stack.lastReply(storage);
    CHECK(reply.body[1] == static_cast<uint8_t>(ble::BridgeError::NoTime));

    // And the status body says so, which is what lets the phone explain a silent
    // node rather than showing one that simply will not send.
    stack.transport.clear();
    stack.send(ble::Opcode::GetStatus, {});
    const ble::MessageView status = stack.lastReply(storage);
    CHECK((status.body[17] & 0x01) == 0); // timeValid clear
}

TEST_CASE("the status body a phone reads is the one the protocol documents")
{
    Stack stack;
    stack.provision();
    stack.send(ble::Opcode::SendText, textCommand(2, "eins"));
    stack.transport.clear();

    stack.send(ble::Opcode::GetStatus, {});
    std::vector<uint8_t> storage;
    const ble::MessageView reply = stack.lastReply(storage);

    REQUIRE(reply.bodyLen == 1 + 17);
    const uint8_t *status = reply.body + 1; // after the echoed opcode
    CHECK(status[6] == 1);                  // queueDepth -- the message just queued
    CHECK(status[7] == 0);                  // band g3
    const uint32_t limit = static_cast<uint32_t>(status[12]) | (status[13] << 8)
                           | (status[14] << 16) | (static_cast<uint32_t>(status[15]) << 24);
    CHECK(limit == 360000); // CLAUDE.md 1.3, g3 allows 360 s an hour
    CHECK((status[16] & 0x03) == 0x03); // timeValid and keyProvisioned
}

TEST_CASE("config round-trips through the protocol")
{
    Stack stack;
    stack.provision();

    // sniffIntervalMs = 5000, telemetryIntervalS = 300, and one TLV the node
    // does not know.
    std::vector<uint8_t> body = {7, 0, 0, 0};
    body.insert(body.end(), {0x02, 0x02, 0x88, 0x13});
    body.insert(body.end(), {0x07, 0x02, 0x2C, 0x01});
    body.insert(body.end(), {0x5A, 0x01, 0xFF});
    stack.send(ble::Opcode::SetConfig, body);

    CHECK(stack.node.config().sniffIntervalMs == 5000);
    CHECK(stack.node.config().telemetryIntervalS == 300);

    // The acknowledgement names what did not take, rather than failing the
    // whole write (section 3).
    const auto messages = stack.transport.messages();
    REQUIRE(messages.size() >= 2);
    ble::MessageView applied{};
    REQUIRE(ble::decodeMessage(messages.back().data(), messages.back().size(), &applied));
    CHECK(applied.opcode == static_cast<uint8_t>(ble::EventCode::ConfigApplied));
    CHECK(applied.body[8] == 0x5A);

    // GET_CONFIG hands back what was set.
    stack.transport.clear();
    stack.send(ble::Opcode::GetConfig, {});
    std::vector<uint8_t> storage;
    const ble::MessageView reply = stack.lastReply(storage);
    size_t cursor = 0;
    ble::TlvView tlv{};
    bool sawSniff = false;
    while (ble::nextTlv(reply.body + 1, reply.bodyLen - 1, &cursor, &tlv)) {
        if (tlv.type == 0x02) {
            sawSniff = true;
            CHECK(static_cast<uint16_t>(tlv.value[0] | (tlv.value[1] << 8)) == 5000);
        }
    }
    CHECK(sawSniff);
}

TEST_CASE("a config value out of range is reported, and the others still apply")
{
    Stack stack;
    stack.provision();

    // A sniff interval of 20 ms is outside the documented 250..10000 -- a node
    // told to do that has a flat battery by lunchtime.
    std::vector<uint8_t> body = {8, 0, 0, 0};
    body.insert(body.end(), {0x02, 0x02, 0x14, 0x00});
    body.insert(body.end(), {0x08, 0x02, 0x2C, 0x01}); // beaconIntervalS = 300
    stack.send(ble::Opcode::SetConfig, body);

    CHECK(stack.node.config().sniffIntervalMs == 2000); // unchanged default
    CHECK(stack.node.config().beaconIntervalS == 300);  // the good one took
}

TEST_CASE("received frames become journal entries the phone can drain")
{
    Stack stack;
    stack.provision();

    const uint8_t position[12] = {0x40, 0x1B, 0x4A, 0x1C, 0x20, 0x4E, 0xC5, 0x07,
                                  0x00, 0x02, 0x09, 0x04};
    for (int i = 0; i < 3; ++i) {
        stack.node.recordReceivedFrame(500u + i, 2, 0x04, -91, 7, 9, position, sizeof(position));
    }
    CHECK(stack.node.journal().size() == 3);
    CHECK(stack.node.peers().find(2) != nullptr);
    CHECK(stack.node.peers().find(2)->framesReceived == 3);

    stack.transport.clear();
    std::vector<uint8_t> body = {0, 0, 0, 0, 10}; // sinceCounter 0, max 10
    stack.send(ble::Opcode::GetQueue, body);

    const auto messages = stack.transport.messages();
    REQUIRE(messages.size() == 4); // three events, then the response
    ble::MessageView event{};
    REQUIRE(ble::decodeMessage(messages[0].data(), messages[0].size(), &event));
    // EVT_JOURNAL since BRIDGE_PROTO 2 (D15): the counter travels with the entry,
    // and the wrapped opcode is the event it is really about.
    CHECK(event.opcode == static_cast<uint8_t>(ble::EventCode::Journal));
    CHECK(event.txnId == 0);
    CHECK(event.body[4] == static_cast<uint8_t>(ble::EventCode::FrameRx));

    // Fetching does not free anything -- only ACK_QUEUE does.
    CHECK(stack.node.journal().size() == 3);

    const uint32_t highest = stack.node.journal().highestCounter();
    stack.send(ble::Opcode::AckQueue,
               {static_cast<uint8_t>(highest), static_cast<uint8_t>(highest >> 8),
                static_cast<uint8_t>(highest >> 16), static_cast<uint8_t>(highest >> 24)});
    CHECK(stack.node.journal().size() == 0);
}

TEST_CASE("journal counters are drawn from the persistent supply and never repeat")
{
    Stack stack;
    stack.provision();

    const uint8_t payload[4] = {1, 2, 3, 4};
    std::vector<uint32_t> counters;
    for (int i = 0; i < 20; ++i) {
        stack.node.recordReceivedFrame(600u + i, 2, 0x02, -90, 6, 9, payload, sizeof(payload));
    }
    REQUIRE(stack.node.journal().size() == 20);

    app::JournalEntry entries[32];
    const size_t howMany = stack.node.journal().fetch(0, entries, 32);
    for (size_t i = 1; i < howMany; ++i) {
        // Monotonic, never reused (CLAUDE.md 2.1) -- and D10 makes these the
        // journal's own counters rather than the frames'.
        CHECK(entries[i].counter > entries[i - 1].counter);
    }
}

TEST_CASE("a factory reset clears everything except the frame counter")
{
    Stack stack;
    stack.provision();
    stack.send(ble::Opcode::SendText, textCommand(2, "vorher"));
    const uint8_t payload[4] = {1, 2, 3, 4};
    stack.node.recordReceivedFrame(700, 2, 0x02, -90, 6, 9, payload, sizeof(payload));

    REQUIRE(stack.node.queue().size() == 1);
    REQUIRE(stack.node.journal().size() == 1);
    const uint32_t counterBefore = stack.node.journal().highestCounter();

    stack.send(ble::Opcode::FactoryReset, {0x4D, 0x53, 0x45, 0x52});

    CHECK(stack.node.queue().empty());
    CHECK(stack.node.journal().size() == 0);
    CHECK_FALSE(stack.node.keyProvisioned());
    CHECK(stack.node.config().sniffIntervalMs == 2000);

    /*
     * And the counter did not go back. CLAUDE.md 2.1: "Monotonic per device,
     * never reset, never reused." A counter at zero after a factory reset would
     * repeat CCM nonces against a peer that still remembers the old ones -- the
     * one thing a reset must not do.
     */
    stack.provision();
    stack.node.recordReceivedFrame(701, 2, 0x02, -90, 6, 9, payload, sizeof(payload));
    CHECK(stack.node.journal().highestCounter() > counterBefore);
}

TEST_CASE("delivered totals outlive the queue that holds the messages")
{
    /*
     * Gate 2.1 asks that every message was delivered and none was not, and it
     * used to read that off queue().countInState(). The queue holds
     * kQueueCapacity == 24 and evicts the oldest DELIVERED entry when a new
     * message arrives at a full queue (gate 3.2), so its delivered count
     * saturates and then stops rising -- while docs/test-plan.md asks gate 2.1
     * for a hundred frames. Read that way the gate reports a failure on a run
     * in which every single message was acknowledged.
     */
    Stack stack;
    stack.provision();

    constexpr uint32_t kMessages = 40; // comfortably past kQueueCapacity
    static_assert(kMessages > app::kQueueCapacity, "the point of this test");

    for (uint32_t i = 0; i < kMessages; ++i) {
        // One at a time, delivered before the next goes in: the same
        // stop-and-wait shape bring-up sketch 19 uses.
        const std::string text = "link " + std::to_string(i);
        REQUIRE(stack.node.sendText(2, reinterpret_cast<const uint8_t *>(text.data()),
                                    text.size()) == 0);
        const app::QueuedMessage *fresh = nullptr;
        for (size_t slot = 0; slot < stack.node.queue().size(); ++slot) {
            const app::QueuedMessage &entry = stack.node.queue().at(slot);
            if (entry.state == app::MessageState::Pending) {
                fresh = &entry;
            }
        }
        REQUIRE(fresh != nullptr);
        stack.node.onTransmitResult(fresh->id, 100u + i, 2, 0, 0 /* delivered */, 1, -90, 6);
    }

    CHECK(stack.node.deliveredTotal() == kMessages);
    CHECK(stack.node.undeliveredTotal() == 0);

    // And the queue is exactly the working set it was designed to be: it never
    // held more than its capacity, which is why it could not answer this.
    CHECK(stack.node.queue().size() <= app::kQueueCapacity);
    CHECK(stack.node.queue().countInState(app::MessageState::Delivered) < kMessages);
}

TEST_CASE("giving up is counted apart from delivering")
{
    Stack stack;
    stack.provision();

    const std::string text = "one";
    REQUIRE(stack.node.sendText(2, reinterpret_cast<const uint8_t *>(text.data()), text.size())
            == 0);
    const uint32_t id = stack.node.queue().at(0).id;

    // Queued (result 2) is not terminal and must not be counted as either.
    stack.node.onTransmitResult(id, 100, 2, 0, 2 /* queued */, 0, 0, 0);
    CHECK(stack.node.deliveredTotal() == 0);
    CHECK(stack.node.undeliveredTotal() == 0);

    stack.node.onTransmitResult(id, 100, 2, 0, 1 /* undelivered */, 4, 0, 0);
    CHECK(stack.node.deliveredTotal() == 0);
    CHECK(stack.node.undeliveredTotal() == 1);
}

TEST_CASE("LINK_TEST is refused rather than queued when the budget is gone")
{
    Stack stack;
    stack.provision();

    // Spend the g3 hour: 360 s of allowance, in 2 s frames.
    for (int i = 0; i < 200; ++i) {
        stack.node.onTransmitResult(0, 42, 2, 0, 0, 1, -90, 6);
    }
    // The budget is driven directly -- the ARQ path that would normally do it
    // needs a radio.
    link::DutyCycleBudget budget;
    REQUIRE(budget.begin(stack.store, stack.clock));
    for (int i = 0; i < 180 && budget.canTransmit(link::Band::G3, 2000000); ++i) {
        budget.recordTransmission(link::Band::G3, 2000000);
    }

    fakes::FakeKeyStore saturatedKeys;
    app::Node saturated(stack.clock, stack.store, saturatedKeys);
    REQUIRE(saturated.begin(0x0001));
    fakes::FakeBleTransport transport;
    transport.isBonded = true;
    ble::GattServer server(transport, saturated);

    std::vector<uint8_t> body = {0, 0x2A};
    for (int i = 0; i < 16; ++i) {
        body.push_back(static_cast<uint8_t>(0x10 + i));
    }
    uint8_t message[256];
    size_t total = ble::encodeMessage(static_cast<uint8_t>(ble::Opcode::ProvisionKey), 1,
                                      body.data(), body.size(), message, sizeof(message));
    ble::Chunker chunker;
    chunker.begin(message, total, 247, 1);
    uint8_t chunk[ble::kMaxMtu];
    size_t written = 0;
    while (chunker.next(chunk, sizeof(chunk), &written)) {
        server.onChunk(chunk, written, 1000);
    }
    transport.clear();

    const std::vector<uint8_t> linkTest = {2, 0, 5, 9};
    total = ble::encodeMessage(static_cast<uint8_t>(ble::Opcode::LinkTest), 2, linkTest.data(),
                               linkTest.size(), message, sizeof(message));
    chunker.begin(message, total, 247, 2);
    while (chunker.next(chunk, sizeof(chunk), &written)) {
        server.onChunk(chunk, written, 1000);
    }

    const auto messages = transport.messages();
    REQUIRE(!messages.empty());
    ble::MessageView reply{};
    REQUIRE(ble::decodeMessage(messages.back().data(), messages.back().size(), &reply));
    // "A delayed link test is a useless link test" -- refused, not queued.
    CHECK(reply.opcode == static_cast<uint8_t>(ble::EventCode::ResponseError));
    CHECK(reply.body[1] == static_cast<uint8_t>(ble::BridgeError::BudgetExhausted));
}

TEST_CASE("GET_BUDGET reports the release time the UI needs")
{
    Stack stack;
    stack.provision();
    stack.transport.clear();

    stack.send(ble::Opcode::GetBudget, {});
    std::vector<uint8_t> storage;
    const ble::MessageView reply = stack.lastReply(storage);
    REQUIRE(reply.bodyLen == 1 + 13);
    const uint8_t *budget = reply.body + 1;
    CHECK(budget[0] == 0); // g3
    const uint32_t limit = static_cast<uint32_t>(budget[5]) | (budget[6] << 8)
                           | (budget[7] << 16) | (static_cast<uint32_t>(budget[8]) << 24);
    CHECK(limit == 360000);
}

TEST_CASE("the network key")
{
    Stack stack;
    stack.transport.isBonded = true;

    std::vector<uint8_t> body = {0, 0x2A};
    for (int i = 0; i < 16; ++i) {
        body.push_back(static_cast<uint8_t>(0xB0 + i));
    }

    SUBCASE("PROVISION_KEY writes it, and the bytes are the ones sent")
    {
        stack.send(ble::Opcode::ProvisionKey, body);
        std::vector<uint8_t> storage;
        CHECK(stack.lastReply(storage).opcode == static_cast<uint8_t>(ble::EventCode::ResponseOk));

        REQUIRE(stack.keys.hasKey(0));
        CHECK(stack.keys.netIdOf(0) == 0x2A);
        for (int i = 0; i < 16; ++i) {
            CHECK(stack.keys.raw(0)[i] == 0xB0 + i);
        }
        CHECK(stack.node.keyProvisioned());
    }

    SUBCASE("an all-zero key is refused")
    {
        // It is not a key. Accepting one encrypts every frame under something an
        // attacker guesses in a single try, and nothing about the frames looks
        // wrong.
        std::vector<uint8_t> zeroed = {0, 0x2A};
        zeroed.insert(zeroed.end(), 16, 0);
        stack.send(ble::Opcode::ProvisionKey, zeroed);

        std::vector<uint8_t> storage;
        CHECK(stack.lastReply(storage).body[1] == static_cast<uint8_t>(ble::BridgeError::BadParam));
        CHECK_FALSE(stack.keys.hasKey(0));
    }

    SUBCASE("a slot that does not exist is refused")
    {
        std::vector<uint8_t> bad = body;
        bad[0] = 5;
        stack.send(ble::Opcode::ProvisionKey, bad);
        std::vector<uint8_t> storage;
        CHECK(stack.lastReply(storage).body[1] == static_cast<uint8_t>(ble::BridgeError::BadParam));
        CHECK_FALSE(stack.keys.hasAnyKey());
    }

    SUBCASE("a failed write is reported, not swallowed")
    {
        // A node that says a key was provisioned and has none fails later, on
        // the air, where nobody can see why.
        stack.keys.failWrites = true;
        stack.send(ble::Opcode::ProvisionKey, body);
        std::vector<uint8_t> storage;
        CHECK(stack.lastReply(storage).body[1] == static_cast<uint8_t>(ble::BridgeError::Storage));
        CHECK_FALSE(stack.node.keyProvisioned());
    }

    SUBCASE("rotation needs a key in the slot being rotated to")
    {
        stack.send(ble::Opcode::ProvisionKey, body); // slot 0 only
        stack.transport.clear();

        stack.send(ble::Opcode::RotateKey, {1});
        std::vector<uint8_t> storage;
        // Rotating onto an empty slot would transmit under nothing while both
        // peers still believe they share a secret.
        CHECK(stack.lastReply(storage).body[1] == static_cast<uint8_t>(ble::BridgeError::NoKey));
        CHECK(stack.node.activeKeySlot() == 0);

        std::vector<uint8_t> second = body;
        second[0] = 1;
        stack.send(ble::Opcode::ProvisionKey, second);
        stack.transport.clear();
        stack.send(ble::Opcode::RotateKey, {1});
        CHECK(stack.lastReply(storage).opcode == static_cast<uint8_t>(ble::EventCode::ResponseOk));
        CHECK(stack.node.activeKeySlot() == 1);
    }

    SUBCASE("a factory reset erases it")
    {
        stack.send(ble::Opcode::ProvisionKey, body);
        REQUIRE(stack.node.keyProvisioned());

        stack.send(ble::Opcode::FactoryReset, {0x4D, 0x53, 0x45, 0x52});
        CHECK_FALSE(stack.node.keyProvisioned());
        CHECK(stack.keys.erases == 1);
        CHECK(stack.node.activeKeySlot() == 0);
    }

    SUBCASE("provisioning is bonded-tier, and the key never reaches the store unbonded")
    {
        Stack unbonded;
        unbonded.transport.isBonded = false;
        unbonded.send(ble::Opcode::ProvisionKey, body);

        std::vector<uint8_t> storage;
        CHECK(unbonded.lastReply(storage).body[1]
              == static_cast<uint8_t>(ble::BridgeError::NotAuthorised));
        // Gate 6.3's second half, now against a store that would have recorded
        // the write: "key unchanged".
        CHECK(unbonded.keys.writes == 0);
        CHECK_FALSE(unbonded.keys.hasAnyKey());
    }
}

TEST_CASE("EVT_STATUS is journaled on state transitions, not on a timer")
{
    Stack stack;
    stack.provision();

    // The first tick records the state the node woke into.
    stack.node.tick(1000);
    size_t statusEvents = 0;
    app::JournalEntry entries[32];
    size_t howMany = stack.node.journal().fetch(0, entries, 32);
    for (size_t i = 0; i < howMany; ++i) {
        if (entries[i].opcode == 0x83) {
            ++statusEvents;
        }
    }
    CHECK(statusEvents == 1);

    // A hundred more ticks with nothing changing add nothing.
    for (uint32_t t = 0; t < 100; ++t) {
        stack.node.tick(2000 + t * 100);
    }
    statusEvents = 0;
    howMany = stack.node.journal().fetch(0, entries, 32);
    for (size_t i = 0; i < howMany; ++i) {
        if (entries[i].opcode == 0x83) {
            ++statusEvents;
        }
    }
    CHECK(statusEvents == 1);

    // Losing the clock is a transition -- flags bit 0 -- and gets recorded.
    stack.clock.invalidate();
    stack.node.tick(20000);
    statusEvents = 0;
    howMany = stack.node.journal().fetch(0, entries, 32);
    for (size_t i = 0; i < howMany; ++i) {
        if (entries[i].opcode == 0x83) {
            ++statusEvents;
        }
    }
    CHECK(statusEvents == 2);
}

TEST_CASE("EVT_FIX is journaled with D11's hdop rule")
{
    Stack stack;
    stack.provision();

    stack.node.onFix(482082000, 163738000, 171, 0 /* absent */, 0, 11);

    app::JournalEntry entries[8];
    const size_t howMany = stack.node.journal().fetch(0, entries, 8);
    bool found = false;
    for (size_t i = 0; i < howMany; ++i) {
        if (entries[i].opcode != 0x86) {
            continue;
        }
        found = true;
        REQUIRE(entries[i].length == 13);
        // An absent hdop reads as 255 -- unknown -- never as a perfect 0.
        CHECK(entries[i].body[10] == 255);
        CHECK(entries[i].body[12] == 11);
    }
    CHECK(found);
    CHECK_FALSE(stack.node.fixPending());
}
