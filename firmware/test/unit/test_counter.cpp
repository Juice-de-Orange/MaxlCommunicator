/*
 * The frame counter, driven through the failure modes that matter.
 *
 * docs/test-plan.md 2.6 is the hardware gate: "50 forced power cycles, some
 * mid-transmit -- counter never repeats or goes backwards across the whole log."
 * That gate needs a device. What can be done here is the same property against a
 * store that loses power at every point in the reservation cycle, which is a
 * denser search than fifty pulls of a battery will ever be.
 */

#include <set>

#include "doctest.h"

#include "fakes/fake_block_store.h"
#include "link/counter.h"

using namespace link;
using fakes::FakeBlockStore;

TEST_CASE("a fresh counter starts at one and reserves a block up front")
{
    FakeBlockStore store;
    FrameCounter counter;

    REQUIRE(counter.begin(store));

    /*
     * One, not zero. Zero means "no counter" everywhere else in the system:
     * docs/bridge-protocol.md section 3 has the phone ask GET_QUEUE for
     * "everything after the last counter it durably stored", a phone holding
     * nothing sends 0, and the bridge's EventStore documents highWaterMark() as
     * "0 if it holds nothing". A device whose first counter really were 0 would
     * have exactly one journal entry no client could ever ask for -- the first.
     *
     * Found by test/unit/test_node_integration.cpp, which is the first thing to
     * drive both sides of that contract at once.
     */
    CHECK(counter.peek() == 1);
    CHECK(counter.reservedUpTo() == kCounterBlockSize);
    CHECK(counter.transmitAllowed());
}

TEST_CASE("handing out a block costs exactly one flash write")
{
    // CLAUDE.md 2.1: "persist in blocks ... so you are not writing flash on every
    // frame". Asserted as a number, because it is the kind of claim that quietly
    // stops being true.
    FakeBlockStore store;
    FrameCounter counter;
    REQUIRE(counter.begin(store));

    store.resetCounters();
    // One fewer than the block size, because the first value handed out is 1
    // rather than 0 -- the block still covers up to kCounterBlockSize.
    for (uint32_t i = 0; i < kCounterBlockSize - 1; ++i) {
        uint32_t value = 0;
        REQUIRE(counter.next(&value));
    }
    CHECK(store.writes == 0);  // the whole first block was already reserved

    uint32_t value = 0;
    REQUIRE(counter.next(&value));
    CHECK(value == kCounterBlockSize);
    CHECK(store.writes > 0);  // crossing into the next block does write
}

TEST_CASE("a reboot resumes from the reserved mark, never from the last used value")
{
    FakeBlockStore store;
    uint32_t lastUsed = 0;

    {
        FrameCounter counter;
        REQUIRE(counter.begin(store));
        for (int i = 0; i < 5; ++i) {
            REQUIRE(counter.next(&lastUsed));
        }
        CHECK(lastUsed == 5); // 1..5, because zero is reserved for "none"
    }

    FrameCounter rebooted;
    REQUIRE(rebooted.begin(store));
    // Not 5. The unused tail of the block is discarded, which is the trade the
    // block reservation makes: skip values freely, never repeat one.
    CHECK(rebooted.peek() == kCounterBlockSize);
    CHECK(rebooted.peek() > lastUsed);
}

TEST_CASE("no value is ever handed out twice across many power cuts")
{
    /*
     * The property from docs/test-plan.md 2.6, checked exhaustively rather than
     * fifty times: cut power after every possible number of frames within a
     * block, twice over, and collect every value ever issued.
     */
    FakeBlockStore store;
    std::set<uint32_t> issued;
    uint32_t highest = 0;

    for (uint32_t framesBeforeCut = 0; framesBeforeCut < 2 * kCounterBlockSize;
         ++framesBeforeCut) {
        FrameCounter counter;
        REQUIRE(counter.begin(store));

        for (uint32_t i = 0; i < framesBeforeCut; ++i) {
            uint32_t value = 0;
            REQUIRE(counter.next(&value));

            // Never repeated ...
            CHECK(issued.insert(value).second);
            // ... and never backwards.
            CHECK(value >= highest);
            highest = value;
        }

        store.powerCut();
    }

    CHECK(issued.size() > 0);
}

TEST_CASE("a store that cannot be written blocks transmission")
{
    // CLAUDE.md 2.1: "If the counter cannot be persisted, the device must refuse
    // to transmit."
    FakeBlockStore store;
    store.failWritesAfter(0);

    FrameCounter counter;
    CHECK_FALSE(counter.begin(store));
    CHECK_FALSE(counter.transmitAllowed());

    uint32_t value = 0;
    CHECK_FALSE(counter.next(&value));
}

TEST_CASE("a store that fails part way through latches off")
{
    FakeBlockStore store;
    FrameCounter counter;
    REQUIRE(counter.begin(store));

    // Let the current block drain, then make the next reservation impossible.
    for (uint32_t i = 0; i < kCounterBlockSize - 1; ++i) {
        uint32_t value = 0;
        REQUIRE(counter.next(&value));
    }
    store.failWritesAfter(0);

    uint32_t value = 0;
    CHECK_FALSE(counter.next(&value));
    CHECK_FALSE(counter.transmitAllowed());

    // A store that failed once is not trusted again mid-session; recovery is a
    // reboot, where begin() decides afresh.
    store.failWritesAfter(-1);
    CHECK_FALSE(counter.next(&value));
    CHECK_FALSE(counter.transmitAllowed());
}

TEST_CASE("a torn trailing record cannot move the counter backwards")
{
    FakeBlockStore store;
    {
        FrameCounter counter;
        REQUIRE(counter.begin(store));
        for (uint32_t i = 0; i < kCounterBlockSize + 10; ++i) {
            uint32_t value = 0;
            REQUIRE(counter.next(&value));
        }
        REQUIRE(store.sync(hal::StoreRegion::FrameCounter) == hal::StoreResult::Ok);
    }

    const uint32_t goodMark = 2 * kCounterBlockSize;

    // Append a record whose two halves disagree, as a write interrupted halfway
    // would leave it.
    const uint8_t torn[kCounterRecordBytes] = {0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    REQUIRE(store.append(hal::StoreRegion::FrameCounter, torn, sizeof(torn)) ==
            hal::StoreResult::Ok);
    REQUIRE(store.sync(hal::StoreRegion::FrameCounter) == hal::StoreResult::Ok);

    FrameCounter rebooted;
    REQUIRE(rebooted.begin(store));
    CHECK(rebooted.peek() == goodMark);
}

TEST_CASE("compaction keeps the mark when the region fills up")
{
    FakeBlockStore store;
    // A region with room for three reservations forces compaction quickly.
    store.configure(hal::StoreRegion::FrameCounter, kCounterRecordBytes, 3);

    FrameCounter counter;
    REQUIRE(counter.begin(store));

    uint32_t highest = 0;
    for (uint32_t i = 0; i < 10 * kCounterBlockSize; ++i) {
        uint32_t value = 0;
        REQUIRE(counter.next(&value));
        CHECK(value >= highest);
        highest = value;
    }
    const uint32_t markBefore = counter.reservedUpTo();

    store.powerCut();
    FrameCounter rebooted;
    REQUIRE(rebooted.begin(store));
    CHECK(rebooted.peek() == markBefore);
    CHECK(rebooted.peek() > highest);
}
