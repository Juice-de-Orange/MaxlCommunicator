/*
 * Adaptive spreading factor.
 *
 * docs/test-plan.md 2.11 to 2.14 are the hardware gates and need a step
 * attenuator. What is checked here is the decision logic they exercise -- in
 * particular 2.12's oscillation check, which the plan calls "the one people
 * skip", and 2.14's "no mid-retry SF change".
 */

#include "doctest.h"

#include "link/adaptive_sf.h"

using namespace link;

TEST_CASE("a fresh link starts at the rendezvous configuration")
{
    // CLAUDE.md 2.5: the floor both nodes always accept.
    AdaptiveSf sf;
    CHECK(sf.currentSf() == kRendezvousSf);
    CHECK(kRendezvousSf == 9);
}

TEST_CASE("two consecutive failures escalate one step")
{
    AdaptiveSf sf;
    sf.onUndelivered();
    CHECK(sf.currentSf() == 9);  // one failure is not yet evidence
    sf.onUndelivered();
    CHECK(sf.currentSf() == 10);
}

TEST_CASE("a success between two failures resets the count")
{
    AdaptiveSf sf;
    sf.onUndelivered();
    sf.onDelivered(0, 5);
    sf.onUndelivered();
    CHECK(sf.currentSf() == 9);
}

TEST_CASE("escalation stops at SF12")
{
    AdaptiveSf sf;
    for (int i = 0; i < 40; ++i) {
        sf.onUndelivered();
    }
    CHECK(sf.currentSf() == kMaxSf);
    CHECK(sf.currentSf() == 12);
}

TEST_CASE("five ACKs near the demodulation floor escalate")
{
    // "when the last 5 ACKs show SNR below the demodulation floor + 3 dB"
    AdaptiveSf sf;
    const int8_t nearFloor = static_cast<int8_t>(demodulationFloorDb(9) + 1);
    for (int i = 0; i < 4; ++i) {
        sf.onDelivered(0, nearFloor);
        CHECK(sf.currentSf() == 9);  // four is not five
    }
    sf.onDelivered(0, nearFloor);
    CHECK(sf.currentSf() == 10);
}

TEST_CASE("a single bad ACK among good ones does not escalate")
{
    AdaptiveSf sf;
    for (int i = 0; i < 4; ++i) {
        sf.onDelivered(0, 8);
    }
    sf.onDelivered(0, static_cast<int8_t>(demodulationFloorDb(9)));
    CHECK(sf.currentSf() == 9);
}

TEST_CASE("ten first-attempt successes with margin de-escalate")
{
    AdaptiveSf sf;
    for (int i = 0; i < 40; ++i) {
        sf.onUndelivered();
    }
    REQUIRE(sf.currentSf() == 12);

    // A comfortable link: well above the floor of the SF we would drop to.
    for (int i = 0; i < kDeescalateAfterSuccesses; ++i) {
        sf.onDelivered(0, 10);
    }
    CHECK(sf.currentSf() == 11);
}

TEST_CASE("a delivery that needed a retry does not count towards de-escalation")
{
    // A message that took a retry is evidence the link is marginal, not good.
    AdaptiveSf sf;
    for (int i = 0; i < 40; ++i) {
        sf.onUndelivered();
    }
    REQUIRE(sf.currentSf() == 12);

    for (int i = 0; i < 9; ++i) {
        sf.onDelivered(0, 10);
    }
    sf.onDelivered(1, 10);  // succeeded on a retry -- resets the run
    CHECK(sf.currentSf() == 12);
    CHECK(sf.consecutiveFirstAttemptSuccesses() == 0);
}

TEST_CASE("de-escalation needs margin above the floor of the SF it would move to")
{
    /*
     * Judging the margin against the CURRENT floor would let the link drop a step
     * and fall straight off the cliff it thought it had 10 dB above.
     */
    AdaptiveSf sf;
    for (int i = 0; i < 40; ++i) {
        sf.onUndelivered();
    }
    REQUIRE(sf.currentSf() == 12);

    // Comfortable for SF12 (-20 floor) but not for SF11 (-17 + 10 = -7).
    for (int i = 0; i < 20; ++i) {
        sf.onDelivered(0, -9);
    }
    CHECK(sf.currentSf() == 12);
}

TEST_CASE("SF does not move in the middle of a retry sequence")
{
    // docs/test-plan.md 2.14.
    AdaptiveSf sf;
    sf.onTransmitStart(0);
    CHECK_FALSE(sf.locked());
    sf.onTransmitStart(1);
    CHECK(sf.locked());
    sf.onTransmitStart(2);
    CHECK(sf.locked());

    // The lock lifts only when the message reaches an outcome.
    sf.onUndelivered();
    CHECK_FALSE(sf.locked());
}

TEST_CASE("silence returns both sides to the rendezvous configuration")
{
    /*
     * CLAUDE.md 2.5: "After 3 x beaconInterval with no contact, both sides return
     * to the rendezvous configuration rather than scanning the SF set. Scanning
     * costs receive time at every SF and is exactly what you cannot afford when
     * the link is already bad."  docs/test-plan.md 2.13.
     */
    AdaptiveSf sf;
    for (int i = 0; i < 6; ++i) {
        sf.onUndelivered();
    }
    REQUIRE(sf.currentSf() > kRendezvousSf);

    constexpr uint32_t kBeaconMs = 10u * 60u * 1000u;
    CHECK_FALSE(sf.tick(kBeaconMs, kBeaconMs));
    CHECK_FALSE(sf.tick(3u * kBeaconMs - 1u, kBeaconMs));
    CHECK(sf.currentSf() > kRendezvousSf);

    CHECK(sf.tick(3u * kBeaconMs, kBeaconMs));
    CHECK(sf.currentSf() == kRendezvousSf);

    // And it does not keep reporting a change once it is already home.
    CHECK_FALSE(sf.tick(10u * kBeaconMs, kBeaconMs));
}

TEST_CASE("a very long beacon interval does not overflow into an instant timeout")
{
    AdaptiveSf sf;
    sf.onUndelivered();
    sf.onUndelivered();
    REQUIRE(sf.currentSf() == 10);

    // 3 x this wraps a uint32. It must not fire.
    constexpr uint32_t kHugeBeaconMs = 2000u * 1000u * 1000u;
    CHECK_FALSE(sf.tick(1000u, kHugeBeaconMs));
    CHECK(sf.currentSf() == 10);
}

TEST_CASE("a steady good link does not oscillate")
{
    /*
     * docs/test-plan.md 2.12: "no oscillation over 30 min". Hysteresis bugs only
     * show up over time, which is why this drives a thousand deliveries on an
     * unchanging link and checks the SF never moves after it settles.
     */
    AdaptiveSf sf;
    // Settle: a good link at the rendezvous SF has nowhere to go but down, and
    // SF9 is not the floor, so it will drop to SF7 and stay.
    for (int i = 0; i < 1000; ++i) {
        sf.onDelivered(0, 15);
    }
    const uint8_t settled = sf.currentSf();
    CHECK(settled == kMinSf);

    for (int i = 0; i < 1000; ++i) {
        sf.onDelivered(0, 15);
        CHECK(sf.currentSf() == settled);
    }
}

TEST_CASE("a marginal link settles instead of hunting")
{
    /*
     * The nastier case: SNR that is comfortable at SF10 and marginal at SF9, so a
     * naive controller ping-pongs between them for ever.
     */
    AdaptiveSf sf;
    uint8_t previous = sf.currentSf();
    int changes = 0;

    for (int i = 0; i < 2000; ++i) {
        // -11 dB: above SF10's floor + 3 (-12), below SF9's de-escalation
        // threshold for SF8 (-10 + 10 = 0).
        sf.onDelivered(0, -11);
        if (sf.currentSf() != previous) {
            ++changes;
            previous = sf.currentSf();
        }
    }
    // It may move a step or two while it finds its level, but it must not hunt.
    CHECK(changes <= 3);
}

TEST_CASE("the demodulation floor drops with the spreading factor")
{
    for (uint8_t sf = kMinSf; sf < kMaxSf; ++sf) {
        CAPTURE(sf);
        CHECK(demodulationFloorDb(static_cast<uint8_t>(sf + 1)) < demodulationFloorDb(sf));
    }
    CHECK(demodulationFloorDb(9) == -12);
    CHECK(demodulationFloorDb(12) == -20);
}
