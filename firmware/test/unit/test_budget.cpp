/*
 * The duty cycle budget. These are the compliance tests.
 *
 * docs/test-plan.md 2.7, 2.8 and 2.9 are the hardware gates and they still need
 * two devices and a real battery pull. What is checked here is the logic those
 * gates exercise, driven through a fake clock and a store that can lose power --
 * which reaches states a bench test reaches only by luck.
 *
 * The invariant that matters, checked continuously rather than at the end:
 * airtime in any rolling hour never exceeds the band's allowance.
 */

#include "doctest.h"

#include "fakes/fake_block_store.h"
#include "fakes/fake_clock.h"
#include "link/airtime.h"
#include "link/budget.h"

using namespace link;
using fakes::FakeBlockStore;
using fakes::FakeClock;

namespace {

constexpr uint32_t kBootUnix = 1788000000u;  // an arbitrary but valid wall clock

/*
 * A 28-byte POSITION frame as it is actually transmitted at the default
 * configuration: SF9 with a preamble long enough to span the 2 s sniff interval
 * (CLAUDE.md 2.3). 2196 ms, not the 226 ms of the bare 8-symbol form -- the
 * difference is a factor of ten in what the budget is charged, so a test that
 * used the short preamble would be measuring a frame this firmware never sends.
 */
uint32_t realPositionAirtimeUs()
{
    const uint16_t preamble = preambleSymbolsForInterval(9, kRendezvousSniffIntervalMs);
    return timeOnAirUs(9, 28, preamble);
}

/// The bare-preamble form, which is what an ACK to a listening receiver costs.
uint32_t positionAirtimeUs(uint8_t sf = 9)
{
    return timeOnAirUs(sf, 28, kStandardPreambleSymbols);
}

} // namespace

TEST_CASE("a node with no valid time is fully transmit-blocked")
{
    // CLAUDE.md 1.2: "If the RTC time is not valid on boot, the device starts
    // fully transmit-blocked until time is re-established over BLE or GNSS."
    // docs/test-plan.md 2.9.
    FakeBlockStore store;
    FakeClock clock;  // starts invalid
    DutyCycleBudget budget;

    REQUIRE(budget.begin(store, clock));
    CHECK(budget.state() == BudgetState::NoTime);
    CHECK_FALSE(budget.canTransmit(Band::G3, positionAirtimeUs()));
    CHECK(budget.earliestLegalTxUnix(Band::G3, positionAirtimeUs()) == kNeverLegal);

    // Nothing may be recorded either -- an entry against an unknown time would
    // corrupt the window.
    CHECK_FALSE(budget.recordTransmission(Band::G3, positionAirtimeUs()));

    // SET_TIME, or a GNSS fix.
    clock.setUnix(kBootUnix);
    CHECK(budget.state() == BudgetState::Ready);
    CHECK(budget.canTransmit(Band::G3, positionAirtimeUs()));
}

TEST_CASE("a fresh budget is empty and the whole allowance is available")
{
    FakeBlockStore store;
    FakeClock clock;
    clock.setUnix(kBootUnix);
    DutyCycleBudget budget;
    REQUIRE(budget.begin(store, clock));

    CHECK(budget.usedMs(Band::G3) == 0);
    CHECK(budget.remainingMs(Band::G3) == 360000u);
    CHECK(budget.remainingMs(Band::G1) == 36000u);
    CHECK(budget.earliestLegalTxDelayMs(Band::G3, positionAirtimeUs()) == 0);
}

TEST_CASE("the frame lockout matches the CLAUDE.md 1.4 table")
{
    /*
     * Decision D9: after a frame of airtime A the band is silent for
     * A * (1/dc - 1), measured from the end of the transmission. The table gives
     * the figure for a single POSITION frame with the budget otherwise empty.
     */
    struct Row { uint8_t sf; Band band; uint32_t expectedTenths; };
    const Row kRows[] = {
        {7, Band::G3, 6},    {9, Band::G3, 20},   {10, Band::G3, 37},  {12, Band::G3, 148},
        {7, Band::G1, 66},   {9, Band::G1, 224},  {10, Band::G1, 408}, {12, Band::G1, 1630},
    };

    for (const Row &row : kRows) {
        FakeBlockStore store;
        FakeClock clock;
        clock.setUnix(kBootUnix);
        DutyCycleBudget budget;
        REQUIRE(budget.begin(store, clock));

        const uint32_t airtimeUs = timeOnAirUs(row.sf, 28, kStandardPreambleSymbols);
        REQUIRE(budget.recordTransmission(row.band, airtimeUs));

        CAPTURE(row.sf);
        CAPTURE(static_cast<int>(row.band));

        // The lockout is measured from the end of transmission, so the delay
        // reported now -- at the start -- includes the frame's own airtime.
        const uint32_t delayMs = budget.earliestLegalTxDelayMs(row.band, airtimeUs);
        const uint32_t fromEndTenths = (delayMs - airtimeUs / 1000u + 50u) / 100u;
        CHECK(fromEndTenths == row.expectedTenths);

        // ... and it really does refuse until then.
        CHECK_FALSE(budget.canTransmit(row.band, airtimeUs));
        clock.advanceMs(delayMs);
        CHECK(budget.canTransmit(row.band, airtimeUs));
    }
}

TEST_CASE("airtime never exceeds the hourly allowance, driven hard for six hours")
{
    /*
     * docs/test-plan.md 2.7 in simulation: hammer the budget with far more
     * traffic than it can carry and check the rolling-hour invariant after every
     * single transmission, not just at the end.
     */
    FakeBlockStore store;
    FakeClock clock;
    clock.setUnix(kBootUnix);
    DutyCycleBudget budget;
    REQUIRE(budget.begin(store, clock));

    const uint32_t airtimeUs = realPositionAirtimeUs();

    // A running log, so the invariant can be checked independently of the code
    // under test rather than by asking it about itself.
    struct Sent { uint32_t at; uint32_t us; };
    static Sent log[4096];
    size_t sentCount = 0;

    uint32_t blockedAttempts = 0;

    for (uint32_t second = 0; second < 6 * 3600; ++second) {
        // Try to transmit every second -- roughly 20 times the legal rate.
        if (budget.canTransmit(Band::G3, airtimeUs)) {
            REQUIRE(budget.recordTransmission(Band::G3, airtimeUs));
            REQUIRE(sentCount < 4096);
            log[sentCount].at = clock.unixSeconds();
            log[sentCount].us = airtimeUs;
            ++sentCount;

            // Independent check of the rolling hour over the whole log.
            const uint32_t now = clock.unixSeconds();
            uint64_t windowUs = 0;
            for (size_t i = 0; i < sentCount; ++i) {
                if (now - log[i].at < kBudgetWindowSeconds) {
                    windowUs += log[i].us;
                }
            }
            CHECK(windowUs <= 360u * 1000u * 1000u);
        } else {
            ++blockedAttempts;
        }
        clock.advanceSeconds(1);
    }

    // It did transmit, and it did refuse -- a test where either number is zero
    // proves nothing.
    CHECK(sentCount > 0);
    CHECK(blockedAttempts > 0);

    // At the default configuration the lockout is 22 s per frame, so the ceiling
    // is 163 an hour -- and the hourly cap is 163 too, because the two
    // constraints coincide at a steady rate. Six hours is under 1000 frames.
    CHECK(sentCount <= 6 * 164);
}

TEST_CASE("the budget survives a power cut and refuses immediately on reboot")
{
    // docs/test-plan.md 2.8a: saturate, reboot, immediately attempt TX -> refused,
    // and the release time is right relative to the pre-reboot window.
    FakeBlockStore store;
    FakeClock clock;
    clock.setUnix(kBootUnix);

    const uint32_t airtimeUs = timeOnAirUs(12, 28, kStandardPreambleSymbols);
    uint32_t releaseBefore = 0;

    {
        DutyCycleBudget budget;
        REQUIRE(budget.begin(store, clock));
        REQUIRE(budget.recordTransmission(Band::G1, airtimeUs));
        releaseBefore = budget.earliestLegalTxUnix(Band::G1, airtimeUs);
        // SF12 in g1 locks the band for 163 seconds. That is the case worth
        // testing, because it is the one a reboot would be tempting to escape.
        CHECK(releaseBefore > clock.unixSeconds() + 160);
    }

    store.powerCut();

    DutyCycleBudget rebooted;
    REQUIRE(rebooted.begin(store, clock));
    CHECK_FALSE(rebooted.canTransmit(Band::G1, airtimeUs));

    // The release time is reconstructed to the second, not reset.
    const uint32_t releaseAfter = rebooted.earliestLegalTxUnix(Band::G1, airtimeUs);
    CHECK(releaseAfter == releaseBefore);

    // And it does free up on time rather than staying stuck.
    clock.advanceSeconds(releaseAfter - clock.unixSeconds());
    CHECK(rebooted.canTransmit(Band::G1, airtimeUs));
}

TEST_CASE("pulling the battery cannot clear a saturated hourly budget")
{
    // REVIEW.md A4: "Power-cycling between transmissions would have been a
    // compliance hole." Saturate the hourly total, not just the lockout.
    FakeBlockStore store;
    FakeClock clock;
    clock.setUnix(kBootUnix);

    const uint32_t airtimeUs = realPositionAirtimeUs();
    uint32_t sent = 0;

    {
        DutyCycleBudget budget;
        REQUIRE(budget.begin(store, clock));
        /*
         * Step past the lockout each time, so that what finally stops us is the
         * hourly total rather than the per-frame lockout. That is the constraint
         * this test is about -- REVIEW.md A4's compliance hole was in the hourly
         * accounting.
         */
        for (int i = 0; i < 2000; ++i) {
            const uint32_t delayMs = budget.earliestLegalTxDelayMs(Band::G3, airtimeUs);
            if (delayMs == kNeverLegalDelay) {
                break;
            }
            if (delayMs > 60u * 1000u) {
                break;  // the hourly total, not the lockout, is now blocking
            }
            clock.advanceMs(delayMs);
            REQUIRE(budget.recordTransmission(Band::G3, airtimeUs));
            ++sent;
        }
        CHECK(sent > 100);
        CHECK(budget.remainingMs(Band::G3) < airtimeUs / 1000u);
    }

    const uint32_t usedBefore = [&] {
        DutyCycleBudget probe;
        REQUIRE(probe.begin(store, clock));
        return probe.usedMs(Band::G3);
    }();

    // Ten power cycles in a row.
    for (int i = 0; i < 10; ++i) {
        store.powerCut();
        DutyCycleBudget budget;
        REQUIRE(budget.begin(store, clock));
        CHECK(budget.usedMs(Band::G3) == usedBefore);
    }
}

TEST_CASE("the hourly window really rolls -- old airtime comes back")
{
    FakeBlockStore store;
    FakeClock clock;
    clock.setUnix(kBootUnix);
    DutyCycleBudget budget;
    REQUIRE(budget.begin(store, clock));

    const uint32_t airtimeUs = positionAirtimeUs();
    REQUIRE(budget.recordTransmission(Band::G3, airtimeUs));
    const uint32_t used = budget.usedMs(Band::G3);
    CHECK(used > 0);

    clock.advanceSeconds(kBudgetWindowSeconds - 1);
    CHECK(budget.usedMs(Band::G3) == used);  // still inside the hour

    clock.advanceSeconds(1);
    CHECK(budget.usedMs(Band::G3) == 0);  // and out the other side
}

TEST_CASE("the two sub-bands are accounted separately")
{
    // CLAUDE.md 1.2: "a rolling 60-minute airtime total per sub-band".
    FakeBlockStore store;
    FakeClock clock;
    clock.setUnix(kBootUnix);
    DutyCycleBudget budget;
    REQUIRE(budget.begin(store, clock));

    const uint32_t airtimeUs = positionAirtimeUs();
    REQUIRE(budget.recordTransmission(Band::G3, airtimeUs));

    CHECK(budget.usedMs(Band::G3) > 0);
    CHECK(budget.usedMs(Band::G1) == 0);
    // g1 is untouched by a g3 transmission and is free immediately.
    CHECK(budget.canTransmit(Band::G1, airtimeUs));
}

TEST_CASE("a store that cannot persist the ring blocks further transmission")
{
    // CLAUDE.md 1.2 again: the airtime was spent, but a reboot would forget it.
    FakeBlockStore store;
    FakeClock clock;
    clock.setUnix(kBootUnix);
    DutyCycleBudget budget;
    REQUIRE(budget.begin(store, clock));

    store.failWritesAfter(0);
    CHECK_FALSE(budget.recordTransmission(Band::G3, positionAirtimeUs()));
    CHECK(budget.state() == BudgetState::StorageFault);
    CHECK_FALSE(budget.canTransmit(Band::G3, positionAirtimeUs()));

    // Not recoverable by the store coming back: the session can no longer prove
    // what it has spent.
    store.failWritesAfter(-1);
    CHECK(budget.state() == BudgetState::StorageFault);
}

TEST_CASE("a frame that cannot fit in an hour is never legal")
{
    FakeBlockStore store;
    FakeClock clock;
    clock.setUnix(kBootUnix);
    DutyCycleBudget budget;
    REQUIRE(budget.begin(store, clock));

    // No real frame is this long -- the longest is 2.8 s -- but the answer must
    // be "never", not "in an hour".
    CHECK(budget.earliestLegalTxUnix(Band::G1, 40u * 1000u * 1000u) == kNeverLegal);
    CHECK(budget.earliestLegalTxDelayMs(Band::G1, 40u * 1000u * 1000u) == kNeverLegalDelay);
}

TEST_CASE("a clock corrected backwards does not un-spend airtime")
{
    FakeBlockStore store;
    FakeClock clock;
    clock.setUnix(kBootUnix);
    DutyCycleBudget budget;
    REQUIRE(budget.begin(store, clock));

    REQUIRE(budget.recordTransmission(Band::G3, positionAirtimeUs()));
    const uint32_t used = budget.usedMs(Band::G3);

    // SET_TIME arrives with a clock an hour behind. Records now sit in the
    // future; they are kept, because airtime that was spent was spent.
    clock.setUnix(kBootUnix - 3600);
    CHECK(budget.usedMs(Band::G3) == used);
}
