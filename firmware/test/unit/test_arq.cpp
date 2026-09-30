/*
 * Stop-and-wait ARQ, and the part that matters: the budget beating the timer.
 *
 * docs/test-plan.md 2.5 is the hardware gate -- "power off the peer mid-exchange
 * -> exactly 3 retries, then undelivered surfaced. Never silent." What is checked
 * here is the state machine that produces that, in simulated time, including the
 * cases a bench test cannot stage on demand: a saturated budget, a band with no
 * valid time, and an SF12 retry in g1 where the lockout is 45 times the timer.
 */

#include <cstring>
#include <initializer_list>

#include "doctest.h"

#include "fakes/fake_block_store.h"
#include "fakes/fake_clock.h"
#include "fakes/fake_jitter.h"
#include "link/airtime.h"
#include "link/arq.h"
#include "link/budget.h"
#include "link/frame.h"

using namespace link;
using fakes::FakeBlockStore;
using fakes::FakeClock;
using fakes::FixedJitter;

namespace {

constexpr uint32_t kBootUnix = 1788000000u;

/// A rig with a working clock, an empty budget and pinned jitter.
struct Rig {
    FakeBlockStore store;
    FakeClock clock;
    DutyCycleBudget budget;
    FixedJitter jitter{500};
    Arq arq;

    Rig()
    {
        clock.setUnix(kBootUnix);
        REQUIRE(budget.begin(store, clock));
        arq.begin(budget, jitter);
    }

    /// Advance both the fake clock and the ARQ's notion of now.
    void advance(uint32_t ms) { clock.advanceMs(ms); }
    uint32_t now() const { return clock.monotonicMs(); }

    /// Run the machine until it wants something, or until the deadline.
    ArqOutcome runUntilAction(uint32_t maxMs, uint32_t stepMs = 10)
    {
        for (uint32_t elapsed = 0; elapsed < maxMs; elapsed += stepMs) {
            const ArqOutcome outcome = arq.poll(now());
            if (outcome.action != ArqAction::Nothing) {
                return outcome;
            }
            advance(stepMs);
        }
        return ArqOutcome{ArqAction::Nothing, kMaxOutstanding, DeliveryState::Idle, 0};
    }
};

/// A 28-byte POSITION frame.
struct Frame {
    uint8_t bytes[28];
    size_t length = sizeof(bytes);

    Frame(uint8_t seq, uint16_t dst)
    {
        Header header{};
        header.version = kWireVersion;
        header.type = FrameType::Position;
        header.src = 1;
        header.dst = dst;
        header.counter = seq;
        header.seq = seq;
        header.flags = (dst == kBroadcastAddress) ? 0u : kFlagAckReq;
        for (size_t i = 0; i < sizeof(bytes); ++i) {
            bytes[i] = 0;
        }
        encodeHeader(header, bytes);
    }
};

} // namespace

TEST_CASE("a broadcast is delivered as soon as it is sent")
{
    // CLAUDE.md 2.4: "Broadcast frames never request an ACK."
    Rig rig;
    Frame frame(1, kBroadcastAddress);
    const size_t slot =
        rig.arq.submit(frame.bytes, frame.length, 1, 9, Band::G3, false, rig.now());
    REQUIRE(slot < kMaxOutstanding);

    const ArqOutcome sent = rig.runUntilAction(1000);
    REQUIRE(sent.action == ArqAction::Transmit);
    rig.arq.onTransmitComplete(sent.slot, rig.now(), true);

    CHECK(rig.arq.stateOf(slot) == DeliveryState::Delivered);
    const ArqOutcome report = rig.arq.poll(rig.now());
    CHECK(report.action == ArqAction::Deliver);
    CHECK(report.state == DeliveryState::Delivered);
}

TEST_CASE("an acknowledged message is delivered on the first attempt")
{
    Rig rig;
    Frame frame(7, 2);
    const size_t slot = rig.arq.submit(frame.bytes, frame.length, 7, 9, Band::G3, true, rig.now());
    REQUIRE(slot < kMaxOutstanding);

    const ArqOutcome sent = rig.runUntilAction(1000);
    REQUIRE(sent.action == ArqAction::Transmit);
    rig.arq.onTransmitComplete(sent.slot, rig.now(), true);
    CHECK(rig.arq.stateOf(slot) == DeliveryState::InFlight);

    rig.arq.onAckReceived(7, rig.now());
    CHECK(rig.arq.stateOf(slot) == DeliveryState::Delivered);
    CHECK(rig.arq.attemptsOf(slot) == 1);
}

TEST_CASE("exactly three retries, then undelivered -- never silent")
{
    // docs/test-plan.md 2.5.
    Rig rig;
    Frame frame(3, 2);
    const size_t slot = rig.arq.submit(frame.bytes, frame.length, 3, 9, Band::G3, true, rig.now());
    REQUIRE(slot < kMaxOutstanding);

    int transmissions = 0;
    bool sawUndelivered = false;

    for (uint32_t elapsed = 0; elapsed < 20u * 60u * 1000u; elapsed += 10) {
        const ArqOutcome outcome = rig.arq.poll(rig.now());
        if (outcome.action == ArqAction::Transmit) {
            ++transmissions;
            // The peer is off: the transmission succeeds, the ACK never comes.
            rig.arq.onTransmitComplete(outcome.slot, rig.now(), true);
        } else if (outcome.action == ArqAction::Deliver) {
            CHECK(outcome.state == DeliveryState::Undelivered);
            sawUndelivered = true;
            rig.arq.clear(outcome.slot);
            break;
        }
        rig.advance(10);
    }

    CHECK(sawUndelivered);
    // One original plus three retries.
    CHECK(transmissions == 1 + kMaxRetries);
}

TEST_CASE("a saturated budget queues rather than fails")
{
    /*
     * The distinction CLAUDE.md 2.4 insists on: `queued until HH:MM` is not
     * `undelivered`. A message held by the budget must not burn its retries.
     */
    Rig rig;

    // Fill g1's 36 s allowance with SF12 frames.
    const uint32_t bigAirtime = timeOnAirUs(12, 64, kStandardPreambleSymbols);
    for (int i = 0; i < 20 && rig.budget.remainingMs(Band::G1) > 3000; ++i) {
        rig.clock.advanceSeconds(200);  // step past the lockout each time
        if (!rig.budget.canTransmit(Band::G1, bigAirtime)) {
            break;
        }
        REQUIRE(rig.budget.recordTransmission(Band::G1, bigAirtime));
    }
    REQUIRE_FALSE(rig.budget.canTransmit(Band::G1, bigAirtime));

    Frame frame(9, 2);
    const size_t slot =
        rig.arq.submit(frame.bytes, frame.length, 9, 12, Band::G1, true, rig.now());
    REQUIRE(slot < kMaxOutstanding);

    // Poll for a while: it must not transmit, and it must not give up.
    for (int i = 0; i < 200; ++i) {
        const ArqOutcome outcome = rig.arq.poll(rig.now());
        CHECK(outcome.action != ArqAction::Transmit);
        CHECK(outcome.action != ArqAction::Deliver);
        rig.advance(100);
    }

    CHECK(rig.arq.stateOf(slot) == DeliveryState::Queued);
    CHECK(rig.arq.attemptsOf(slot) == 0);  // no retry was spent on a budget block

    // And the UI has a wall-clock time to show, not just "blocked".
    const uint32_t release = rig.arq.releaseUnixOf(slot);
    CHECK(release > kBootUnix);
}

TEST_CASE("a node with no valid time holds the message instead of failing it")
{
    // CLAUDE.md 1.2 / docs/test-plan.md 2.9. The message waits for SET_TIME.
    FakeBlockStore store;
    FakeClock clock;  // invalid
    DutyCycleBudget budget;
    REQUIRE(budget.begin(store, clock));
    FixedJitter jitter;
    Arq arq;
    arq.begin(budget, jitter);

    Frame frame(5, 2);
    const size_t slot = arq.submit(frame.bytes, frame.length, 5, 9, Band::G3, true, 0);
    REQUIRE(slot < kMaxOutstanding);

    for (uint32_t ms = 0; ms < 60000u; ms += 500) {
        const ArqOutcome outcome = arq.poll(ms);
        CHECK(outcome.action == ArqAction::Nothing);
        clock.advanceMonotonicMs(500);
    }
    CHECK(arq.stateOf(slot) == DeliveryState::Queued);
    CHECK(arq.attemptsOf(slot) == 0);

    // SET_TIME arrives and it goes out.
    clock.setUnix(kBootUnix);
    const ArqOutcome sent = arq.poll(clock.monotonicMs());
    CHECK(sent.action == ArqAction::Transmit);
}

TEST_CASE("the budget beats the retry timer, which is the whole point")
{
    /*
     * REVIEW.md A3: at SF12 the retry timer is about 3.6 s, but a single frame
     * locks g1 for 163 s. Without budget-aware scheduling every retry would be
     * blocked by something the timer knew nothing about.
     */
    Rig rig;
    Frame frame(11, 2);
    const size_t slot =
        rig.arq.submit(frame.bytes, frame.length, 11, 12, Band::G1, true, rig.now());
    REQUIRE(slot < kMaxOutstanding);

    const ArqOutcome first = rig.runUntilAction(5000);
    REQUIRE(first.action == ArqAction::Transmit);

    // Charge the airtime, exactly as the caller would after a real transmission.
    size_t frameLen = 0;
    (void)rig.arq.frameOf(slot, &frameLen);
    const uint32_t airtimeUs = timeOnAirUs(12, static_cast<uint8_t>(frameLen),
                                           kStandardPreambleSymbols);
    REQUIRE(rig.budget.recordTransmission(Band::G1, airtimeUs));
    rig.arq.onTransmitComplete(first.slot, rig.now(), true);

    // The retry timer is 2 x airtime + 300 ms, about 3.6 s. Well past it, the
    // lockout still holds the frame.
    rig.advance(10000);
    const ArqOutcome blocked = rig.arq.poll(rig.now());
    CHECK(blocked.action == ArqAction::Nothing);
    CHECK(rig.arq.stateOf(slot) == DeliveryState::Queued);
    CHECK(rig.arq.attemptsOf(slot) == 1);

    // Past the 163-second lockout it goes out again.
    const ArqOutcome retry = rig.runUntilAction(200u * 1000u, 250);
    CHECK(retry.action == ArqAction::Transmit);
    CHECK(rig.arq.attemptsOf(slot) == 2);
}

TEST_CASE("stop-and-wait: only one message is on the air at a time")
{
    Rig rig;
    Frame a(1, 2);
    Frame b(2, 2);
    const size_t slotA = rig.arq.submit(a.bytes, a.length, 1, 9, Band::G3, true, rig.now());
    const size_t slotB = rig.arq.submit(b.bytes, b.length, 2, 9, Band::G3, true, rig.now());
    REQUIRE(slotA < kMaxOutstanding);
    REQUIRE(slotB < kMaxOutstanding);

    const ArqOutcome first = rig.runUntilAction(1000);
    REQUIRE(first.action == ArqAction::Transmit);
    rig.arq.onTransmitComplete(first.slot, rig.now(), true);

    // While the first is unresolved, the second must not go out.
    for (int i = 0; i < 50; ++i) {
        const ArqOutcome outcome = rig.arq.poll(rig.now());
        CHECK(outcome.action != ArqAction::Transmit);
        rig.advance(10);
    }

    rig.arq.onAckReceived(1, rig.now());
    const ArqOutcome report = rig.arq.poll(rig.now());
    REQUIRE(report.action == ArqAction::Deliver);
    rig.arq.clear(report.slot);

    const ArqOutcome second = rig.runUntilAction(5000);
    CHECK(second.action == ArqAction::Transmit);
    CHECK(second.slot == slotB);
}

TEST_CASE("the backoff grows and stays inside its jitter band")
{
    // base x 2^attempt +- 25 %.
    Frame frame(1, 2);

    uint32_t previous = 0;
    for (uint32_t position : {0u, 500u, 1000u}) {
        FakeBlockStore store;
        FakeClock clock;
        clock.setUnix(kBootUnix);
        DutyCycleBudget budget;
        REQUIRE(budget.begin(store, clock));
        FixedJitter jitter(position);
        Arq arq;
        arq.begin(budget, jitter);

        const size_t slot = arq.submit(frame.bytes, frame.length, 1, 9, Band::G3, true, 0);
        REQUIRE(slot < kMaxOutstanding);

        uint32_t now = 0;
        const ArqOutcome sent = arq.poll(now);
        REQUIRE(sent.action == ArqAction::Transmit);
        arq.onTransmitComplete(slot, now, true);

        // Find when the retry becomes due, budget permitting.
        uint32_t waited = 0;
        while (waited < 60000u) {
            now += 10;
            waited += 10;
            if (arq.poll(now).action == ArqAction::Transmit) {
                break;
            }
        }

        const uint32_t base = 2u * timeOnAirMs(9, 28, kStandardPreambleSymbols) + 300u;
        CAPTURE(position);
        CAPTURE(waited);
        // Within the +-25 % band, with a little slack for the 10 ms poll step.
        CHECK(waited >= (base * 75u) / 100u);
        CHECK(waited <= (base * 125u) / 100u + 20u);

        // Higher jitter position means a longer wait.
        CHECK(waited >= previous);
        previous = waited;
    }
}

TEST_CASE("the queue reports when it is full rather than dropping")
{
    Rig rig;
    Frame frame(1, 2);
    for (size_t i = 0; i < kMaxOutstanding; ++i) {
        CHECK(rig.arq.submit(frame.bytes, frame.length, static_cast<uint8_t>(i), 9, Band::G3,
                             true, rig.now()) < kMaxOutstanding);
    }
    CHECK(rig.arq.pending() == kMaxOutstanding);
    // ERR_QUEUE_FULL over the bridge (docs/bridge-protocol.md 4).
    CHECK(rig.arq.submit(frame.bytes, frame.length, 99, 9, Band::G3, true, rig.now()) ==
          kMaxOutstanding);
}

TEST_CASE("slotOfActiveSeq finds the unresolved slot and nothing else")
{
    Rig rig;
    uint8_t frame[64] = {0};

    const size_t slot = rig.arq.submit(frame, sizeof(frame), 42, 9, Band::G3, true, rig.now());
    REQUIRE(slot < kMaxOutstanding);

    // Queued counts as active: an ACK can arrive while the budget holds a retry.
    CHECK(rig.arq.slotOfActiveSeq(42) == slot);
    CHECK(rig.arq.slotOfActiveSeq(43) == kMaxOutstanding);

    // Once delivered, the seq is no longer active -- a late second ACK must not
    // find anything to close.
    const ArqOutcome outcome = rig.runUntilAction(10000);
    REQUIRE(outcome.action == ArqAction::Transmit);
    rig.arq.onTransmitComplete(slot, rig.now(), true);
    rig.arq.onAckReceived(42, rig.now());
    CHECK(rig.arq.slotOfActiveSeq(42) == kMaxOutstanding);
}

TEST_CASE("replaceFrame swaps the bytes and refuses a different length")
{
    Rig rig;
    uint8_t frame[32];
    for (size_t i = 0; i < sizeof(frame); ++i) {
        frame[i] = static_cast<uint8_t>(i);
    }
    const size_t slot = rig.arq.submit(frame, sizeof(frame), 7, 9, Band::G3, true, rig.now());
    REQUIRE(slot < kMaxOutstanding);

    // A rebuilt retry has the same payload, so the same length -- anything else
    // means the caller rebuilt the wrong message.
    uint8_t shorter[16] = {0};
    CHECK(!rig.arq.replaceFrame(slot, shorter, sizeof(shorter)));

    uint8_t rebuilt[32];
    for (size_t i = 0; i < sizeof(rebuilt); ++i) {
        rebuilt[i] = static_cast<uint8_t>(0xA0u + i);
    }
    CHECK(rig.arq.replaceFrame(slot, rebuilt, sizeof(rebuilt)));

    size_t len = 0;
    const uint8_t *stored = rig.arq.frameOf(slot, &len);
    REQUIRE(stored != nullptr);
    CHECK(len == sizeof(rebuilt));
    CHECK(std::memcmp(stored, rebuilt, len) == 0);

    CHECK(!rig.arq.replaceFrame(kMaxOutstanding, rebuilt, sizeof(rebuilt)));
}
