/*
 * Replay window and ARQ duplicate suppression.
 *
 * docs/test-plan.md 2.3 is the hardware gate: "Replay -- retransmit a captured
 * frame. Rejected by the replay window, 20 trials." The logic behind it is here.
 */

#include "doctest.h"

#include "fakes/fake_block_store.h"
#include "link/replay.h"

using namespace link;
using fakes::FakeBlockStore;

TEST_CASE("the first frame from a peer establishes the mark")
{
    ReplayGuard guard;
    CHECK_FALSE(guard.knowsPeer(0x0002));
    CHECK(guard.admit(0x0002, 5000, 1) == ReplayVerdict::Accept);
    CHECK(guard.knowsPeer(0x0002));
    CHECK(guard.highWaterMark(0x0002) == 5000);
}

TEST_CASE("a captured frame replayed verbatim is rejected")
{
    ReplayGuard guard;
    REQUIRE(guard.admit(0x0002, 100, 1) == ReplayVerdict::Accept);
    // Same counter again -- the exact attack the window exists for.
    CHECK(guard.admit(0x0002, 100, 1) == ReplayVerdict::Replayed);
    // Twenty more times, as the gate asks.
    for (int i = 0; i < 20; ++i) {
        CHECK(guard.admit(0x0002, 100, 1) == ReplayVerdict::Replayed);
    }
}

TEST_CASE("frames older than the window are rejected outright")
{
    ReplayGuard guard;
    REQUIRE(guard.admit(0x0002, 1000, 1) == ReplayVerdict::Accept);

    // Just inside the window: accepted, because honest reordering happens when a
    // retry is scheduled behind a budget block and a later frame overtakes it.
    CHECK(guard.admit(0x0002, 1000 - (kReplayWindowSize - 1), 2) == ReplayVerdict::Accept);
    // Exactly at the edge: too old to judge.
    CHECK(guard.admit(0x0002, 1000 - kReplayWindowSize, 3) == ReplayVerdict::Replayed);
    CHECK(guard.admit(0x0002, 1, 4) == ReplayVerdict::Replayed);
}

TEST_CASE("out-of-order frames inside the window are accepted once each")
{
    ReplayGuard guard;
    REQUIRE(guard.admit(0x0002, 100, 1) == ReplayVerdict::Accept);
    REQUIRE(guard.admit(0x0002, 105, 2) == ReplayVerdict::Accept);

    // 101..104 arrive late. Each is new, and each is accepted exactly once.
    for (uint32_t counter = 101; counter <= 104; ++counter) {
        CAPTURE(counter);
        CHECK(guard.admit(0x0002, counter, static_cast<uint8_t>(counter)) ==
              ReplayVerdict::Accept);
        CHECK(guard.admit(0x0002, counter, static_cast<uint8_t>(counter)) ==
              ReplayVerdict::Replayed);
    }
    CHECK(guard.highWaterMark(0x0002) == 105);
}

TEST_CASE("a big forward jump does not open a hole behind it")
{
    ReplayGuard guard;
    REQUIRE(guard.admit(0x0002, 100, 1) == ReplayVerdict::Accept);
    // The peer was reserving counter blocks while out of contact.
    REQUIRE(guard.admit(0x0002, 100 + 256, 2) == ReplayVerdict::Accept);

    // Everything the jump swept past is now older than the window.
    CHECK(guard.admit(0x0002, 100, 3) == ReplayVerdict::Replayed);
    CHECK(guard.admit(0x0002, 200, 4) == ReplayVerdict::Replayed);
    // And the new region behind the mark still works.
    CHECK(guard.admit(0x0002, 100 + 255, 5) == ReplayVerdict::Accept);
}

TEST_CASE("an ARQ retry is a duplicate, not a replay")
{
    /*
     * CLAUDE.md 2.4: the receiver "dedupes on (src, seq)". A retry carries a NEW
     * counter -- the sender never reuses one -- so the replay window accepts it.
     * It is the seq that says the application has seen this message already.
     *
     * The distinction matters: a replay is discarded silently, a duplicate must
     * still be ACKed or the sender keeps retrying.
     */
    ReplayGuard guard;
    REQUIRE(guard.admit(0x0002, 100, 42) == ReplayVerdict::Accept);
    CHECK(guard.admit(0x0002, 101, 42) == ReplayVerdict::Duplicate);
    // A different seq at the same time is ordinary new traffic.
    CHECK(guard.admit(0x0002, 102, 43) == ReplayVerdict::Accept);
}

TEST_CASE("peers are tracked independently")
{
    ReplayGuard guard;
    REQUIRE(guard.admit(0x0002, 500, 1) == ReplayVerdict::Accept);
    // The same counter from a different node is not a replay -- the nonce is
    // (src, counter), so these are different frames entirely.
    CHECK(guard.admit(0x0003, 500, 1) == ReplayVerdict::Accept);
    CHECK(guard.highWaterMark(0x0002) == 500);
    CHECK(guard.highWaterMark(0x0003) == 500);
}

TEST_CASE("the peer table refuses rather than evicting")
{
    /*
     * Evicting a peer would reset its window, so an attacker able to flood the
     * table with new source addresses could then replay against the evicted one.
     * Refusing the ninth peer is the safe failure.
     */
    ReplayGuard guard;
    for (uint16_t i = 0; i < kMaxPeers; ++i) {
        CAPTURE(i);
        CHECK(guard.admit(static_cast<uint16_t>(100 + i), 1, 1) == ReplayVerdict::Accept);
    }
    CHECK(guard.admit(999, 1, 1) == ReplayVerdict::NoRoom);

    // The established peers keep working, and keep rejecting replays.
    CHECK(guard.admit(100, 1, 1) == ReplayVerdict::Replayed);
    CHECK(guard.admit(100, 2, 2) == ReplayVerdict::Accept);
}

TEST_CASE("inspect does not change anything")
{
    ReplayGuard guard;
    REQUIRE(guard.admit(0x0002, 100, 1) == ReplayVerdict::Accept);
    CHECK(guard.inspect(0x0002, 101, 2) == ReplayVerdict::Accept);
    CHECK(guard.highWaterMark(0x0002) == 100);
    CHECK(guard.admit(0x0002, 101, 2) == ReplayVerdict::Accept);
}

TEST_CASE("reset forgets peers but is never applied to the local counter")
{
    // versioning-and-updates.md 5: FACTORY_RESET clears peer state. It does NOT
    // reset the frame counter, which link/counter.h owns and which survives
    // everything -- a device back from a reset with a counter of zero would reuse
    // nonces against any peer that still remembered it.
    ReplayGuard guard;
    REQUIRE(guard.admit(0x0002, 100, 1) == ReplayVerdict::Accept);
    guard.reset();
    CHECK_FALSE(guard.knowsPeer(0x0002));
    CHECK(guard.admit(0x0002, 100, 1) == ReplayVerdict::Accept);
}

TEST_CASE("frames outside the dedupe leave the seq history untouched")
{
    /*
     * An ACK echoes the seq of the frame it acknowledges, and both sides count
     * their seqs from zero. Recording it would make the peer's next honest
     * message with that seq look like a retry: acknowledged, never delivered.
     * Only ACK_REQ frames -- the only ones that retry -- take part in the
     * dedupe (link/replay.h).
     */
    link::ReplayGuard guard;

    // The peer's ACK, seq 7, not participating: accepted, not recorded.
    CHECK(guard.admit(2, 10, 7, false) == link::ReplayVerdict::Accept);
    // The peer's own later message with the same seq is NOT a duplicate.
    CHECK(guard.admit(2, 11, 7, true) == link::ReplayVerdict::Accept);
    // But a real retry of that message -- same seq, fresh counter -- is.
    CHECK(guard.admit(2, 12, 7, true) == link::ReplayVerdict::Duplicate);
    // And a non-participating frame never trips the check either way.
    CHECK(guard.admit(2, 13, 7, false) == link::ReplayVerdict::Accept);
}

// --- persistence across a reboot (issue #2) ---------------------------------

TEST_CASE("the mark survives a power cut and everything at or below it is rejected")
{
    FakeBlockStore store;
    {
        ReplayGuard guard;
        REQUIRE(guard.begin(store));
        REQUIRE(guard.admit(0x0002, 1000, 1) == ReplayVerdict::Accept);
        REQUIRE(guard.admit(0x0002, 1003, 2) == ReplayVerdict::Accept);
    }
    store.powerCut();

    ReplayGuard rebooted;
    REQUIRE(rebooted.begin(store));
    CHECK(rebooted.knowsPeer(0x0002));
    // Both frames seen before the reboot, and the reordering slot between them
    // that was never seen: a receiver that cannot tell must refuse.
    CHECK(rebooted.admit(0x0002, 1000, 1) == ReplayVerdict::Replayed);
    CHECK(rebooted.admit(0x0002, 1003, 2) == ReplayVerdict::Replayed);
    CHECK(rebooted.admit(0x0002, 1001, 3) == ReplayVerdict::Replayed);
    // Up to the reserved mark, too -- that is the price of not writing per frame.
    CHECK(rebooted.admit(0x0002, 1000 + kReplayMarkBlock, 4) == ReplayVerdict::Replayed);
    // Past it, the peer is heard again.
    CHECK(rebooted.admit(0x0002, 1001 + kReplayMarkBlock, 5) == ReplayVerdict::Accept);
}

TEST_CASE("the mark is written once per block, not once per frame")
{
    FakeBlockStore store;
    ReplayGuard guard;
    REQUIRE(guard.begin(store));
    store.resetCounters();

    // The first frame reserves; the next kReplayMarkBlock counters are covered.
    for (uint32_t c = 500; c <= 500 + kReplayMarkBlock; ++c) {
        REQUIRE(guard.admit(0x0002, c, static_cast<uint8_t>(c)) == ReplayVerdict::Accept);
    }
    CHECK(store.syncs == 1);
    REQUIRE(guard.admit(0x0002, 501 + kReplayMarkBlock, 7) == ReplayVerdict::Accept);
    CHECK(store.syncs == 2);
}

TEST_CASE("every peer comes back, through compaction too")
{
    FakeBlockStore store;
    {
        ReplayGuard guard;
        REQUIRE(guard.begin(store));
        // Far more reservations than the region holds records, across three
        // peers: the region compacts, and no peer may be forgotten by it.
        for (uint32_t step = 0; step < 3 * kReplayRecordCapacity; ++step) {
            const uint16_t src = static_cast<uint16_t>(2 + step % 3);
            const uint32_t counter = 100 + step * (kReplayMarkBlock + 1);
            REQUIRE(guard.admit(src, counter, static_cast<uint8_t>(step)) == ReplayVerdict::Accept);
        }
    }
    store.powerCut();
    CHECK(store.count(hal::StoreRegion::ReplayMarks) <= kReplayRecordCapacity);

    ReplayGuard rebooted;
    REQUIRE(rebooted.begin(store));
    for (uint16_t src = 2; src <= 4; ++src) {
        CAPTURE(src);
        CHECK(rebooted.knowsPeer(src));
        // The last counter each peer had accepted is a replay now.
        uint32_t last = 0;
        for (uint32_t step = 0; step < 3 * kReplayRecordCapacity; ++step) {
            if (2 + step % 3 == src) {
                last = 100 + step * (kReplayMarkBlock + 1);
            }
        }
        CHECK(rebooted.admit(src, last, 0xEE) == ReplayVerdict::Replayed);
        CHECK(rebooted.highWaterMark(src) == last + kReplayMarkBlock);
    }
}

TEST_CASE("a torn record cannot move a mark backwards")
{
    FakeBlockStore store;
    {
        ReplayGuard guard;
        REQUIRE(guard.begin(store));
        REQUIRE(guard.admit(0x0002, 9000, 1) == ReplayVerdict::Accept);
    }
    // A trailing record whose check does not match: a write the power cut tore.
    uint8_t torn[kReplayRecordBytes] = {};
    torn[0] = 1;
    torn[4] = 0x02;
    torn[6] = 0x05;  // mark 5, far below the real one
    REQUIRE(store.append(hal::StoreRegion::ReplayMarks, torn, sizeof(torn))
            == hal::StoreResult::Ok);

    ReplayGuard rebooted;
    REQUIRE(rebooted.begin(store));
    CHECK(rebooted.highWaterMark(0x0002) == 9000 + kReplayMarkBlock);
    CHECK(rebooted.admit(0x0002, 9000, 1) == ReplayVerdict::Replayed);
}

TEST_CASE("a mark that cannot be persisted is a frame that is not accepted")
{
    FakeBlockStore store;
    ReplayGuard guard;
    REQUIRE(guard.begin(store));
    REQUIRE(guard.admit(0x0002, 100, 1) == ReplayVerdict::Accept);

    store.failWritesAfter(0);
    // Under the persisted mark: already covered, judged normally.
    CHECK(guard.admit(0x0002, 105, 2) == ReplayVerdict::Accept);
    // Past it: would need a new mark on flash, and there is none to be had.
    CHECK(guard.admit(0x0002, 100 + kReplayMarkBlock + 1, 3) == ReplayVerdict::NotPersisted);
    CHECK(guard.highWaterMark(0x0002) == 105);
    // A new peer likewise.
    CHECK(guard.admit(0x0003, 1, 1) == ReplayVerdict::NotPersisted);
    CHECK_FALSE(guard.knowsPeer(0x0003));
}

TEST_CASE("a region that is not configured fails closed")
{
    FakeBlockStore store;
    store.configure(hal::StoreRegion::ReplayMarks, 8, 64);  // wrong record size
    ReplayGuard guard;
    CHECK_FALSE(guard.begin(store));
    CHECK(guard.admit(0x0002, 100, 1) == ReplayVerdict::NotPersisted);
}

TEST_CASE("reset forgets the RAM history but not the persisted marks")
{
    FakeBlockStore store;
    ReplayGuard guard;
    REQUIRE(guard.begin(store));
    REQUIRE(guard.admit(0x0002, 100, 1) == ReplayVerdict::Accept);
    guard.reset();
    CHECK(guard.admit(0x0002, 100, 1) == ReplayVerdict::Replayed);
}
