/*
 * The duty cycle budget, asked about the frame that actually goes on the air.
 *
 * test_budget.cpp proves the tracker refuses what does not fit, and
 * test_airtime.cpp proves the airtime arithmetic against the spec tables. What
 * neither can see is whether the node asks the tracker about the right frame.
 * Until issue #1 it did not: the ARQ admitted a data frame against its airtime
 * with the 8-symbol preamble, the radio then sent it with the preamble the sniff
 * interval demands (CLAUDE.md 2.3, 489 symbols at the default), and the budget
 * was charged the real figure only after the fact. A frame that fitted on paper
 * pushed the rolling hour past what CLAUDE.md 1.2 calls legally binding.
 *
 * These tests drive a whole app::Node with a fake radio, so the question and the
 * charge both come from the shipping code.
 */

#include "doctest.h"

#include <cstring>

#include "app/node.h"
#include "fakes/fake_block_store.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_jitter.h"
#include "fakes/fake_key_store.h"
#include "fakes/fake_radio.h"
#include "link/airtime.h"
#include "link/band.h"
#include "link/budget.h"
#include "link/frame.h"

namespace {

const uint8_t kKey[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                          0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
constexpr uint8_t kNetId = 0x2A;
constexpr uint32_t kBootUnix = 1788126105u;

/// Twelve bytes of text make a 28-byte frame -- the frame size the CLAUDE.md 2.3
/// table is written for (SF9, 2 s sniff interval, 489 symbols, 2196 ms).
const char kText[] = "twelve bytes";
static_assert(sizeof(kText) - 1 == 12, "the text must make a 28-byte frame");
constexpr uint8_t kFrameBytes = 12 + link::kFrameOverheadBytes;

uint32_t sniffAirtimeUs()
{
    return link::timeOnAirUs(link::kRendezvousSf, kFrameBytes,
                             link::preambleSymbolsForInterval(link::kRendezvousSf,
                                                              link::kRendezvousSniffIntervalMs));
}

/// Spend all but `leftUs` of the g3 hour, directly on the flash ring the node
/// will reconstruct its budget from.
void prefillBudget(fakes::FakeBlockStore &store, fakes::FakeClock &clock, uint32_t leftUs)
{
    link::DutyCycleBudget budget;
    REQUIRE(budget.begin(store, clock));
    uint64_t remaining = static_cast<uint64_t>(link::plan(link::Band::G3).airtimeBudgetMsPerHour)
                         * 1000u - leftUs;
    while (remaining > 0) {
        const uint32_t chunk = remaining > 2000000u ? 2000000u : static_cast<uint32_t>(remaining);
        REQUIRE(budget.recordTransmission(link::Band::G3, chunk));
        remaining -= chunk;
    }
}

struct Rig {
    uint32_t now = 0;
    fakes::FakeClock clock;
    fakes::FakeBlockStore store;
    fakes::FakeKeyStore keys;
    fakes::FixedJitter jitter;
    fakes::SimChannel channel;
    fakes::FakeRadio radio{channel, &now};
    app::Node node{clock, store, keys};

    explicit Rig(uint32_t budgetLeftUs)
    {
        clock.setUnix(kBootUnix);
        prefillBudget(store, clock, budgetLeftUs);
        // Past the frame lockout of the last prefill record, still inside the
        // hour: only the rolling total can hold the frame back now.
        clock.advanceSeconds(120);
        now = clock.monotonicMs();
        REQUIRE(node.begin(0x0001));
        keys.store(0, kNetId, kKey);
        channel.attach(&radio, nullptr);
        REQUIRE(node.attachRadio(radio, jitter));
    }

    void sendText()
    {
        REQUIRE(node.sendText(0x0002, reinterpret_cast<const uint8_t *>(kText),
                              sizeof(kText) - 1) == 0);
    }

    /// Let wall-clock time pass in one step, both clocks together.
    void skipSeconds(uint32_t seconds)
    {
        clock.advanceSeconds(seconds);
        now = clock.monotonicMs();
    }

    void run(uint32_t forMs)
    {
        const uint32_t until = now + forMs;
        while (now < until) {
            now += 10;
            clock.advanceMs(10);
            channel.pump(now);
            radio.pump();
            node.pumpRadio(now);
        }
    }

    /// GET_BUDGET's used figure, in milliseconds (docs/bridge-protocol.md 4).
    uint32_t usedMs()
    {
        uint8_t body[32];
        REQUIRE(node.budget(body, sizeof(body)) >= 9);
        return static_cast<uint32_t>(body[1]) | (static_cast<uint32_t>(body[2]) << 8)
               | (static_cast<uint32_t>(body[3]) << 16) | (static_cast<uint32_t>(body[4]) << 24);
    }
};

} // namespace

TEST_CASE("a data frame is charged what CLAUDE.md 2.3 tabulates for it")
{
    // An empty budget, the default configuration: SF9, g3, 2 s sniff interval.
    Rig rig(360000000u);
    rig.sendText();
    rig.run(3000);  // charged when the transmission completes, 2.2 s after it starts

    REQUIRE(rig.radio.transmitCount == 1);
    // CLAUDE.md 2.3, default row: 489 symbols, 2196 ms TX airtime.
    CHECK(link::preambleSymbolsForInterval(9, 2000) == 489);
    CHECK(sniffAirtimeUs() / 1000u == 2196u);
    CHECK(rig.usedMs() == 2196u);
}

TEST_CASE("a data frame is admitted only if its real airtime fits the hour")
{
    /*
     * One second of the g3 hour left. With the 8-symbol preamble the frame would
     * cost 226 ms (CLAUDE.md 1.4) and fit; with the 489-symbol preamble it is
     * actually sent with, it costs 2196 ms and does not.
     */
    const uint32_t leftUs = 1000000u;
    REQUIRE(link::timeOnAirUs(9, kFrameBytes, link::kStandardPreambleSymbols) < leftUs);
    REQUIRE(sniffAirtimeUs() > leftUs);

    Rig rig(leftUs);
    rig.sendText();
    rig.run(30000);

    // CLAUDE.md 1.2: the node MUST refuse -- queued, never sent, never dropped.
    CHECK(rig.radio.transmitCount == 0);
    CHECK(rig.usedMs() <= link::plan(link::Band::G3).airtimeBudgetMsPerHour);
    CHECK(rig.node.queue().countInState(app::MessageState::Queued)
          + rig.node.queue().countInState(app::MessageState::InFlight) == 1);

    // Once the prefill has left the rolling hour, the same frame goes out and
    // is charged its real airtime.
    rig.skipSeconds(link::kBudgetWindowSeconds);
    rig.run(3000);
    CHECK(rig.radio.transmitCount == 1);
    CHECK(rig.usedMs() == sniffAirtimeUs() / 1000u);
}

TEST_CASE("a factory reset does not give the airtime back")
{
    /*
     * Issue #40. FACTORY_RESET is an opcode any bonded phone can send
     * (docs/bridge-protocol.md 3). If it erased the budget ring, a reset and a
     * reboot would clear the rolling hour and the frame lockout -- the duty
     * cycle CLAUDE.md 1.2 makes legally binding would be one tap away from
     * empty. "A budget that a power cycle can clear is not a budget", and a
     * factory reset is a power cycle with extra steps.
     */
    Rig rig(360000000u);
    rig.sendText();
    rig.run(3000);
    REQUIRE(rig.radio.transmitCount == 1);
    const uint32_t spentMs = rig.usedMs();
    REQUIRE(spentMs == 2196u);

    REQUIRE(rig.node.factoryReset() == 0);
    // A clean reboot afterwards: what the reset left on flash is all there is.

    fakes::FakeKeyStore freshKeys;
    app::Node rebooted(rig.clock, rig.store, freshKeys);
    REQUIRE(rebooted.begin(0x0001));
    uint8_t body[32];
    REQUIRE(rebooted.budget(body, sizeof(body)) >= 13);
    const uint32_t used = static_cast<uint32_t>(body[1]) | (static_cast<uint32_t>(body[2]) << 8)
                          | (static_cast<uint32_t>(body[3]) << 16)
                          | (static_cast<uint32_t>(body[4]) << 24);
    CHECK(used == spentMs);
    // And the frame lockout is still running: the next data frame must wait.
    const uint32_t nextTx = static_cast<uint32_t>(body[9]) | (static_cast<uint32_t>(body[10]) << 8)
                            | (static_cast<uint32_t>(body[11]) << 16)
                            | (static_cast<uint32_t>(body[12]) << 24);
    CHECK(nextTx > rig.clock.unixSeconds());
}
