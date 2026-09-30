/*
 * The refresh policy, which is where docs/test-plan.md gate 4.1 actually lives.
 *
 * 4.1 asks for zero partial refreshes across an hour with no state change, and
 * its note names the bug it exists to catch: "a redraw triggered by a value that
 * technically changed". An hour of wall clock is not a thing a host test can
 * have, so what is checked here is the property that would make that hour pass
 * -- ten thousand updates with contentChanged false produce nothing at all.
 */

#include "doctest.h"

#include "hal/refresh_policy.h"

using namespace hal;

TEST_CASE("the first push is always full, because the glass is unknown")
{
    RefreshPolicy policy;
    CHECK(policy.decide(true, false) == RefreshKind::Full);
    CHECK(policy.decide(true, false) == RefreshKind::Partial);
}

TEST_CASE("nothing changed means nothing happens -- gate 4.1")
{
    RefreshPolicy policy;
    REQUIRE(policy.decide(true, false) == RefreshKind::Full);

    for (int i = 0; i < 10000; ++i) {
        REQUIRE(policy.decide(false, false) == RefreshKind::None);
    }
    CHECK(policy.partialCount() == 0);
    CHECK(policy.fullCount() == 1);
    CHECK(policy.skippedCount() == 10000);
}

TEST_CASE("ghosting is counted in partials performed, not in seconds elapsed")
{
    RefreshPolicy policy;
    REQUIRE(policy.decide(true, false) == RefreshKind::Full);

    for (int i = 0; i < RefreshPolicy::kPartialsBeforeFull; ++i) {
        CAPTURE(i);
        REQUIRE(policy.decide(true, false) == RefreshKind::Partial);
    }
    CHECK(policy.partialsSinceFull() == RefreshPolicy::kPartialsBeforeFull);

    // The seventeenth change is the one that clears the ghost.
    CHECK(policy.decide(true, false) == RefreshKind::Full);
    CHECK(policy.partialsSinceFull() == 0);
}

TEST_CASE("idle time does not bring the full refresh any closer")
{
    RefreshPolicy policy;
    REQUIRE(policy.decide(true, false) == RefreshKind::Full);

    for (int round = 0; round < 5; ++round) {
        REQUIRE(policy.decide(true, false) == RefreshKind::Partial);
        for (int idle = 0; idle < 1000; ++idle) {
            REQUIRE(policy.decide(false, false) == RefreshKind::None);
        }
    }
    CHECK(policy.partialsSinceFull() == 5);
    CHECK(policy.fullCount() == 1);
}

TEST_CASE("a screen change is full even when the two screens render identically")
{
    RefreshPolicy policy;
    REQUIRE(policy.decide(true, false) == RefreshKind::Full);
    REQUIRE(policy.decide(true, false) == RefreshKind::Partial);

    // Two empty lists look the same. Underneath them the ghost does not.
    CHECK(policy.decide(false, true) == RefreshKind::Full);
    CHECK(policy.partialsSinceFull() == 0);
}

TEST_CASE("reset forces the next push full")
{
    RefreshPolicy policy;
    REQUIRE(policy.decide(true, false) == RefreshKind::Full);
    REQUIRE(policy.decide(true, false) == RefreshKind::Partial);

    policy.reset();
    CHECK(policy.decide(false, false) == RefreshKind::Full);
}
