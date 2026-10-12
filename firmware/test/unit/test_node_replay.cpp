/*
 * The replay window survives a reboot (issue #2).
 *
 * CLAUDE.md 2.1: "The receiver keeps a replay window per peer and rejects
 * anything at or below the high-water mark outside the window." Until issue #2
 * that mark lived in RAM only, so a frame captured off the air was accepted a
 * second time by a receiver that had rebooted in between -- docs/test-plan.md
 * 2.3 with a power cycle in the middle.
 *
 * Two whole nodes over the simulated channel; B is rebooted by building a new
 * app::Node on the same flash after a power cut, which keeps exactly what was
 * synced and nothing else.
 */

#include "doctest.h"

#include <cstring>

#include "app/journal.h"
#include "app/node.h"
#include "fakes/fake_block_store.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_jitter.h"
#include "fakes/fake_key_store.h"
#include "fakes/fake_radio.h"
#include "link/frame.h"

namespace {

const uint8_t kKey[16] = {0x8f, 0x2a, 0x11, 0xd0, 0x5c, 0x77, 0x93, 0x04,
                          0xbe, 0x61, 0x38, 0xaa, 0x0d, 0xf5, 0x42, 0x19};
constexpr uint8_t kNetId = 0x2A;
constexpr uint32_t kBootUnix = 1788126105u;

struct Board {
    fakes::FakeClock clock;
    fakes::FakeBlockStore store;
    fakes::FakeKeyStore keys;
    fakes::FixedJitter jitter;
    fakes::FakeRadio radio;
    app::Node *node = nullptr;
    uint16_t id;

    Board(fakes::SimChannel &channel, uint32_t *now, uint16_t nodeId)
        : radio(channel, now), id(nodeId)
    {
        clock.setUnix(kBootUnix);
        keys.store(0, kNetId, kKey);
        boot();
    }

    ~Board() { delete node; }

    /// Power the board up: a fresh Node on whatever the flash kept.
    void boot()
    {
        delete node;
        node = new app::Node(clock, store, keys);
        REQUIRE(node->begin(id));
        REQUIRE(node->attachRadio(radio, jitter));
    }

    /// Pull the battery and put it back.
    void powerCycle()
    {
        store.powerCut();
        boot();
    }
};

/// Both clocks move, the wall clock included: a rebooted node reconstructs its
/// budget lockout in RTC seconds, and FakeClock::advanceMs in 10 ms steps would
/// never move those.
void tick(fakes::FakeClock &clock, uint32_t now)
{
    clock.advanceMonotonicMs(10);
    if (now % 1000u == 0) {
        clock.setUnix(clock.unixSeconds() + 1u);
    }
}

void run(uint32_t *now, uint32_t forMs, Board &a, Board &b, fakes::SimChannel &channel)
{
    const uint32_t until = *now + forMs;
    while (*now < until) {
        *now += 10;
        tick(a.clock, *now);
        tick(b.clock, *now);
        channel.pump(*now);
        a.radio.pump();
        b.radio.pump();
        a.node->pumpRadio(*now);
        b.node->pumpRadio(*now);
    }
}

size_t textEventsIn(const app::Node &node)
{
    app::JournalEntry entries[64];
    const size_t howMany = node.journal().fetch(0, entries, 64);
    size_t texts = 0;
    for (size_t i = 0; i < howMany; ++i) {
        // EVT_FRAME_RX with a TEXT frame type at offset 6.
        if (entries[i].opcode == 0x81 && entries[i].length > 6 && entries[i].body[6] == 0x05) {
            ++texts;
        }
    }
    return texts;
}

void sendText(Board &from, uint16_t dst, const char *text)
{
    REQUIRE(from.node->sendText(dst, reinterpret_cast<const uint8_t *>(text), std::strlen(text))
            == 0);
}

} // namespace

TEST_CASE("a frame captured before a reboot is rejected after it")
{
    uint32_t now = 0;
    fakes::SimChannel channel(0x5EED);
    Board a(channel, &now, 0x0001);
    Board b(channel, &now, 0x0002);
    channel.attach(&a.radio, &b.radio);

    sendText(a, 0x0002, "before the reboot");
    run(&now, 30000, a, b, channel);
    REQUIRE(textEventsIn(*b.node) == 1);

    // What B took off the air -- the attacker's recording of A's frame.
    uint8_t captured[link::kMaxFrameBytes];
    const size_t capturedLen = b.radio.lastFrameLength;
    REQUIRE(capturedLen > link::kHeaderBytes);
    std::memcpy(captured, b.radio.lastFrame, capturedLen);
    link::Header header{};
    REQUIRE(link::inspect(captured, capturedLen, &header) == link::FrameError::None);
    REQUIRE(header.src == 0x0001);
    REQUIRE(header.type == link::FrameType::Text);

    b.powerCycle();
    const uint32_t rejectedBefore = b.node->replaysRejected();

    hal::RxInfo info{-85, 7};
    b.node->onFrameReceived(captured, capturedLen, info);

    CHECK(b.node->replaysRejected() == rejectedBefore + 1);
    CHECK(textEventsIn(*b.node) == 1);
}

TEST_CASE("after a reboot the peer's next frames are still accepted")
{
    /*
     * The flip side. The persisted mark is a reservation ahead of what was
     * really seen, like the frame counter's (link/counter.h), so the peer's
     * frames inside the reservation are refused after a reboot -- its retries
     * travel under fresh counters and get through. What must not happen is a
     * receiver that stays deaf to a peer it knew.
     */
    uint32_t now = 0;
    fakes::SimChannel channel(0xBEEF);
    Board a(channel, &now, 0x0001);
    Board b(channel, &now, 0x0002);
    channel.attach(&a.radio, &b.radio);

    sendText(a, 0x0002, "one");
    run(&now, 30000, a, b, channel);
    REQUIRE(textEventsIn(*b.node) == 1);

    b.powerCycle();

    for (const char *text : {"two", "three", "four"}) {
        sendText(a, 0x0002, text);
        run(&now, 300000, a, b, channel);
    }
    CHECK(textEventsIn(*b.node) >= 3);
    CHECK(a.node->deliveredTotal() >= 3);
}
