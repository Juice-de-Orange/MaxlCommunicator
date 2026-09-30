/*
 * The application layer: message queue, event journal, peer table, scheduler.
 *
 * These are the four gates in docs/test-plan.md phase 3, and every one of them
 * is about something surviving: a message across a reboot, an event across a
 * failed sync, a schedule across six hours of accumulated jitter.
 */

#include "doctest.h"

#include <cstring>
#include <string>

#include "app/journal.h"
#include "app/message_queue.h"
#include "app/peer_state.h"
#include "app/scheduler.h"
#include "fakes/fake_block_store.h"

using namespace app;

namespace {

Accept submitText(MessageQueue &queue, uint16_t dst, const std::string &text, uint32_t now,
                  uint32_t *id = nullptr)
{
    return queue.submit(dst, text.data(), text.size(), now, id);
}

std::string textOf(const QueuedMessage &message)
{
    return std::string(message.text, message.textLen);
}

} // namespace

TEST_CASE("gate 3.1 -- the queue survives a reboot with 20 pending messages")
{
    fakes::FakeBlockStore store;

    uint32_t ids[20];
    {
        MessageQueue queue(store);
        for (int i = 0; i < 20; ++i) {
            const std::string text = "Test text " + std::to_string(i);
            REQUIRE(submitText(queue, 2, text, 1000u + static_cast<uint32_t>(i), &ids[i])
                    == Accept::Ok);
        }
        CHECK(queue.size() == 20);
    }

    // The reboot: a brand-new queue over the same store, exactly as boot does it.
    MessageQueue restored(store);
    CHECK(restored.restore() == 20);
    REQUIRE(restored.size() == 20);

    SUBCASE("all present, in order, with their text")
    {
        for (size_t i = 0; i < 20; ++i) {
            CHECK(restored.at(i).id == ids[i]);
            CHECK(textOf(restored.at(i)) == "Test text " + std::to_string(i));
            CHECK(restored.at(i).dst == 2);
            CHECK(restored.at(i).state == MessageState::Pending);
        }
    }

    SUBCASE("no duplicates -- every id appears exactly once")
    {
        for (size_t i = 0; i < restored.size(); ++i) {
            int seen = 0;
            for (size_t j = 0; j < restored.size(); ++j) {
                if (restored.at(j).id == restored.at(i).id) {
                    ++seen;
                }
            }
            CHECK(seen == 1);
        }
    }

    SUBCASE("ids handed out after the reboot do not collide with the ones on flash")
    {
        // The whole point of persisting nextId. A reused id would make the UI
        // show a delivery result against the wrong message.
        restored.setState(ids[0], MessageState::Delivered);
        restored.reclaimDelivered();
        uint32_t fresh = 0;
        REQUIRE(submitText(restored, 2, "danach", 2000, &fresh) == Accept::Ok);
        for (size_t i = 0; i < 20; ++i) {
            CHECK(fresh != ids[i]);
        }
    }
}

TEST_CASE("gate 3.1 -- delivery state survives the reboot too")
{
    fakes::FakeBlockStore store;
    uint32_t queued = 0;
    uint32_t gaveUp = 0;
    {
        MessageQueue queue(store);
        submitText(queue, 2, "wartet", 1000, &queued);
        submitText(queue, 2, "vergeblich", 1001, &gaveUp);
        queue.setState(queued, MessageState::Queued, 0, 1200);
        queue.setState(gaveUp, MessageState::Undelivered, 3);
    }

    MessageQueue restored(store);
    restored.restore();
    // CLAUDE.md 2.4: the three states mean different things to the user, and a
    // reboot that flattened them into "pending" would tell them the device is
    // still trying when it has given up.
    CHECK(restored.at(0).state == MessageState::Queued);
    CHECK(restored.at(0).releaseAtUnix == 1200);
    CHECK(restored.at(1).state == MessageState::Undelivered);
    CHECK(restored.at(1).attempts == 3);
}

TEST_CASE("gate 3.2 -- queue full behaviour")
{
    fakes::FakeBlockStore store;
    MessageQueue queue(store);

    uint32_t ids[kQueueCapacity];
    for (size_t i = 0; i < kQueueCapacity; ++i) {
        REQUIRE(submitText(queue, 2, "voll " + std::to_string(i), 1000, &ids[i]) == Accept::Ok);
    }
    REQUIRE(queue.size() == kQueueCapacity);

    SUBCASE("with nothing delivered, a new message is refused rather than evicting one")
    {
        // Refusing is visible -- docs/bridge-protocol.md has ERR_QUEUE_FULL for
        // it. Evicting a message the user is still waiting on is not.
        CHECK(submitText(queue, 2, "zu spaet", 2000) == Accept::Full);
        CHECK(queue.size() == kQueueCapacity);
        // And nothing was lost in the attempt.
        CHECK(textOf(queue.at(0)) == "voll 0");
    }

    SUBCASE("the oldest delivered message is the one that goes")
    {
        queue.setState(ids[3], MessageState::Delivered);
        queue.setState(ids[7], MessageState::Delivered);

        REQUIRE(submitText(queue, 2, "neu", 2000) == Accept::Ok);
        CHECK(queue.size() == kQueueCapacity);

        // ids[3] was the older of the two delivered entries.
        bool three = false;
        bool seven = false;
        for (size_t i = 0; i < queue.size(); ++i) {
            three = three || queue.at(i).id == ids[3];
            seven = seven || queue.at(i).id == ids[7];
        }
        CHECK_FALSE(three);
        CHECK(seven);
    }

    SUBCASE("an undelivered message is truncated to its head, never silently dropped")
    {
        /*
         * CLAUDE.md 2.4: after three attempts a frame "is marked undelivered
         * and surfaced in the UI. It is not silently dropped." Until D18 this
         * meant Full -- and a queue of 24 failures walled the outbox shut for
         * ever (measured 2026-08-31: 23 undelivered, one in flight, every new
         * message refused). D18, decided the same day: the oldest
         * failure is reduced to dst, time and attempts on the stub ring, marked
         * as truncated, and the new message is taken. The failure stays
         * surfaced; only its prose is gone.
         */
        for (size_t i = 0; i < kQueueCapacity; ++i) {
            queue.setState(ids[i], MessageState::Undelivered, 3);
        }
        CHECK(submitText(queue, 2, "does not evict the oldest", 2000) == Accept::Ok);
        CHECK(queue.countInState(MessageState::Undelivered) == kQueueCapacity - 1);
        REQUIRE(queue.stubCount() == 1);
        CHECK(queue.stubAt(0).attempts == 3);
    }

    SUBCASE("a text longer than the frame allows is refused, not truncated")
    {
        MessageQueue empty(store);
        empty.restore();
        empty.reclaimDelivered();
        const std::string tooLong(link::kMaxTextBytes + 1, 'x');
        CHECK(empty.submit(2, tooLong.data(), tooLong.size(), 1000) == Accept::TextTooLong);
    }
}

TEST_CASE("a terminal state is terminal")
{
    fakes::FakeBlockStore store;
    MessageQueue queue(store);
    uint32_t id = 0;
    submitText(queue, 2, "einmal", 1000, &id);

    queue.setState(id, MessageState::Undelivered, 3);
    // A late ACK must not restart the machinery for a message the user has
    // already been told about.
    CHECK_FALSE(queue.setState(id, MessageState::InFlight, 1));
    CHECK(queue.at(0).state == MessageState::Undelivered);
    // But a genuinely later delivery is allowed to correct the record.
    CHECK(queue.setState(id, MessageState::Delivered, 3));
}

TEST_CASE("a storage failure is reported, not swallowed")
{
    fakes::FakeBlockStore store;
    MessageQueue queue(store);
    store.failWritesAfter(0);
    // A message the user believes is queued and that a reboot loses is worse
    // than one that was refused in front of them.
    CHECK(submitText(queue, 2, "won't work", 1000) == Accept::StorageFailed);
    CHECK(queue.size() == 0);
}

TEST_CASE("a foreign record cannot silence the journal for ever")
{
    /*
     * sketch_10_hal.cpp exercises the journal region by leaving behind one
     * record of the pattern 0x40,0x41,0x42,... and never cleaning up. Its first
     * four bytes restore as counter 0x43424140 -- 1.13 billion. append() then
     * refuses every real event, silently, because it requires counters to
     * increase and nothing this device draws will ever out-rank that.
     *
     * Node A ran 20.8 h on 2026-08-31 and reported journal.entries = 1 with all
     * 21 of its own events swallowed, and reflashing does not help: the region
     * survives. restore() takes the counter high-water mark so that a record
     * this device cannot have written is dropped instead of trusted.
     */
    fakes::FakeBlockStore store;
    const uint8_t body[4] = {1, 2, 3, 4};

    // The leftover, byte for byte as bring-up 10 writes it.
    uint8_t poison[kJournalRecordBytes];
    for (size_t i = 0; i < sizeof(poison); ++i) {
        poison[i] = static_cast<uint8_t>(0x40 + i);
    }
    REQUIRE(store.append(hal::StoreRegion::EventJournal, poison, sizeof(poison))
            == hal::StoreResult::Ok);

    SUBCASE("unbounded, it poisons the journal -- the behaviour that was observed")
    {
        Journal journal(store);
        CHECK(journal.restore() == 1);
        CHECK(journal.highestCounter() == 0x43424140u);
        // Every real event refused, and nothing says so.
        CHECK_FALSE(journal.append(1, 0x81, body, sizeof(body)));
        CHECK_FALSE(journal.append(500, 0x81, body, sizeof(body)));
        CHECK(journal.size() == 1);
    }

    SUBCASE("bounded by the counter high-water mark, it is dropped")
    {
        Journal journal(store);
        // A device that has reserved counters up to 512 cannot have written
        // 1.13 billion.
        CHECK(journal.restore(512) == 0);
        CHECK(journal.append(1, 0x81, body, sizeof(body)));
        CHECK(journal.append(2, 0x81, body, sizeof(body)));
        CHECK(journal.size() == 2);
    }

    SUBCASE("a record the device really did write is kept")
    {
        Journal journal(store);
        journal.restore(512);
        REQUIRE(journal.append(7, 0x81, body, sizeof(body)));
        REQUIRE(journal.persist());

        Journal reloaded(store);
        CHECK(reloaded.restore(512) == 1);
        CHECK(reloaded.highestCounter() == 7);
    }
}

TEST_CASE("gate 3.3 -- journal wraparound and reclamation")
{
    fakes::FakeBlockStore store;
    Journal journal(store);
    const uint8_t body[4] = {1, 2, 3, 4};

    SUBCASE("acknowledged entries are reclaimed, oldest first")
    {
        for (uint32_t i = 1; i <= 20; ++i) {
            REQUIRE(journal.append(i, 0x81, body, sizeof(body)));
        }
        CHECK(journal.size() == 20);

        CHECK(journal.acknowledge(8) == 8);
        CHECK(journal.size() == 12);
        CHECK(journal.oldestCounter() == 9);
        CHECK(journal.highestCounter() == 20);
        CHECK(journal.lostEntries() == 0);
    }

    SUBCASE("a full journal of acknowledged entries makes room without losing anything")
    {
        for (uint32_t i = 1; i <= kJournalCapacity; ++i) {
            REQUIRE(journal.append(i, 0x81, body, sizeof(body)));
        }
        CHECK(journal.size() == kJournalCapacity);

        journal.acknowledge(kJournalCapacity / 2);
        for (uint32_t i = 1; i <= kJournalCapacity / 2; ++i) {
            REQUIRE(journal.append(static_cast<uint32_t>(kJournalCapacity) + i, 0x81, body,
                                   sizeof(body)));
        }
        CHECK(journal.lostEntries() == 0);
        CHECK(journal.size() == kJournalCapacity);
    }

    SUBCASE("a full journal of UNacknowledged entries drops the oldest and says so")
    {
        // Storage is finite and the radio does not stop. What matters is that
        // the hole is counted -- a gap nobody knows about is worse than one that
        // is reported.
        for (uint32_t i = 1; i <= kJournalCapacity + 5; ++i) {
            REQUIRE(journal.append(i, 0x81, body, sizeof(body)));
        }
        CHECK(journal.size() == kJournalCapacity);
        CHECK(journal.lostEntries() == 5);
        CHECK(journal.oldestCounter() == 6);
    }

    SUBCASE("fetch returns what the phone asked for, in order")
    {
        for (uint32_t i = 1; i <= 40; ++i) {
            journal.append(i, 0x81, body, sizeof(body));
        }
        JournalEntry out[10];
        CHECK(journal.fetch(25, out, 10) == 10);
        for (size_t i = 0; i < 10; ++i) {
            CHECK(out[i].counter == 26 + i);
        }
        // "GET_QUEUE in a loop until it returns fewer events than maxEvents."
        CHECK(journal.fetch(35, out, 10) == 5);
        CHECK(journal.fetch(40, out, 10) == 0);
    }

    SUBCASE("only acknowledge frees space -- fetching does not")
    {
        for (uint32_t i = 1; i <= 10; ++i) {
            journal.append(i, 0x81, body, sizeof(body));
        }
        JournalEntry out[10];
        journal.fetch(0, out, 10);
        journal.fetch(0, out, 10);
        // A phone that died between GET_QUEUE and ACK_QUEUE costs a repeated
        // transfer, never an event (docs/bridge-protocol.md section 3).
        CHECK(journal.size() == 10);
    }

    SUBCASE("survives a reboot")
    {
        for (uint32_t i = 1; i <= 30; ++i) {
            journal.append(i, 0x82, body, sizeof(body));
        }
        journal.acknowledge(10);

        Journal restored(store);
        CHECK(restored.restore() == 20);
        CHECK(restored.oldestCounter() == 11);
        CHECK(restored.highestCounter() == 30);
    }

    SUBCASE("a counter that does not advance is refused")
    {
        REQUIRE(journal.append(5, 0x81, body, sizeof(body)));
        CHECK_FALSE(journal.append(5, 0x81, body, sizeof(body)));
        CHECK_FALSE(journal.append(4, 0x81, body, sizeof(body)));
        CHECK(journal.size() == 1);
    }
}

TEST_CASE("gate 3.4 -- the telemetry interval holds over six hours")
{
    Scheduler scheduler;
    constexpr uint32_t kIntervalS = 600; // ten minutes
    constexpr uint32_t kSixHoursMs = 6u * 3600u * 1000u;

    scheduler.setInterval(Task::Telemetry, kIntervalS, 0);

    // A loop that is never punctual: work takes a variable few milliseconds, as
    // it does on a device that sometimes redraws an e-paper panel between polls.
    uint32_t now = 0;
    uint32_t jitter = 12345;
    uint32_t firings = 0;
    while (now < kSixHoursMs) {
        jitter = jitter * 1664525u + 1013904223u;
        now += 900 + (jitter % 400); // 0.9 to 1.3 s between polls
        if (scheduler.due(Task::Telemetry, now)) {
            ++firings;
        }
    }

    const uint32_t expected = kSixHoursMs / (kIntervalS * 1000u); // 36
    CHECK(firings >= expected - 1);
    CHECK(firings <= expected + 1);

    // The gate's own wording: honoured within 10 %.
    const double mean = static_cast<double>(kSixHoursMs) / static_cast<double>(firings);
    CHECK(mean > kIntervalS * 1000.0 * 0.9);
    CHECK(mean < kIntervalS * 1000.0 * 1.1);
}

TEST_CASE("the scheduler keeps its phase rather than restarting the interval")
{
    Scheduler scheduler;
    scheduler.setInterval(Task::Beacon, 10, 0);

    // Polled consistently late. Restarting from `now` would push every firing
    // 500 ms further out and accumulate; keeping the phase does not.
    uint32_t firings = 0;
    for (uint32_t now = 0; now <= 100'000; now += 500) {
        if (scheduler.due(Task::Beacon, now + 400)) {
            ++firings;
        }
    }
    CHECK(firings == 10);
}

TEST_CASE("the scheduler does not fire a backlog after a long gap")
{
    Scheduler scheduler;
    scheduler.setInterval(Task::Telemetry, 60, 0);

    // Asleep for an hour. Catching up by firing sixty times would spend the
    // whole duty cycle allowance on backlog.
    CHECK(scheduler.due(Task::Telemetry, 3'600'000));
    CHECK_FALSE(scheduler.due(Task::Telemetry, 3'600'001));
    CHECK(scheduler.firings(Task::Telemetry) == 1);
}

TEST_CASE("the scheduler survives the millis() wrap")
{
    Scheduler scheduler;
    // Just under 49 days -- a node meant to run a fortnight per charge will meet
    // this on a shelf, and a naive now >= nextAt comparison stops firing forever.
    const uint32_t nearWrap = 0xFFFFFF00u;
    scheduler.setInterval(Task::Telemetry, 1, nearWrap);
    CHECK_FALSE(scheduler.due(Task::Telemetry, nearWrap + 500));
    CHECK(scheduler.due(Task::Telemetry, nearWrap + 1100)); // wrapped
}

TEST_CASE("an interval of zero disables the task")
{
    Scheduler scheduler;
    // Config TLV 0x07 documents 0 as off.
    scheduler.setInterval(Task::Telemetry, 0, 0);
    CHECK_FALSE(scheduler.due(Task::Telemetry, 10'000'000));
    CHECK(scheduler.untilNext(0, 60'000) == 60'000);
}

TEST_CASE("peer table")
{
    PeerTable peers;

    SUBCASE("first contact creates the peer")
    {
        peers.heard(2, -91, 7, 9, 1000);
        const Peer *peer = peers.find(2);
        REQUIRE(peer != nullptr);
        CHECK(peer->lastRssi == -91);
        CHECK(peer->lastSnr == 7);
        CHECK(peer->lastSf == 9);
        CHECK(peer->framesReceived == 1);
        CHECK(peers.size() == 1);
    }

    SUBCASE("any received frame counts as contact")
    {
        // CLAUDE.md 2.5: "treat any received frame as an implicit beacon".
        peers.heard(2, -90, 6, 9, 1000);
        peers.heard(2, -95, 4, 9, 1060);
        CHECK(peers.find(2)->framesReceived == 2);
        CHECK(peers.find(2)->lastSeenUnix == 1060);
        CHECK(peers.size() == 1);
    }

    SUBCASE("contact lapses after three beacon intervals")
    {
        peers.heard(2, -90, 6, 9, 1000);
        CHECK_FALSE(peers.contactLapsed(2, 1000 + 3 * 300, 300));
        CHECK(peers.contactLapsed(2, 1000 + 3 * 300 + 1, 300));
    }

    SUBCASE("a peer never heard from has lapsed by definition")
    {
        CHECK(peers.contactLapsed(99, 5000, 300));
    }

    SUBCASE("a wall clock correction does not look like a lapse")
    {
        // SET_TIME or a GNSS fix can move the clock backwards by hours. Treating
        // that as an enormous age would drop a working link to the rendezvous
        // configuration for no reason at all.
        peers.heard(2, -90, 6, 9, 2'000'000);
        CHECK_FALSE(peers.contactLapsed(2, 1'000'000, 300));
    }

    SUBCASE("a ninth peer displaces the one silent longest")
    {
        for (uint16_t i = 0; i < kMaxPeers; ++i) {
            peers.heard(static_cast<uint16_t>(10 + i), -90, 6, 9, 1000u + i);
        }
        CHECK(peers.size() == kMaxPeers);
        peers.heard(99, -80, 8, 9, 5000);
        CHECK(peers.find(10) == nullptr); // the quietest
        CHECK(peers.find(99) != nullptr);
        CHECK(peers.size() == kMaxPeers);
    }

    SUBCASE("position is recorded separately from link quality")
    {
        peers.heard(2, -90, 6, 9, 1000);
        CHECK_FALSE(peers.find(2)->hasPosition);
        peers.position(2, 474'800'000, 130'400'000, 430, 1005);
        CHECK(peers.find(2)->hasPosition);
        CHECK(peers.find(2)->latE7 == 474'800'000);
        CHECK(peers.find(2)->altM == 430);
    }
}

TEST_CASE("a message that was in flight when the power went is Pending again")
{
    /*
     * Measured on node A, 2026-08-31: a queue of 24 with 9 entries stuck in
     * InFlight across a power cycle, nothing promoted, nothing transmitted, and
     * every new message refused with ERR_QUEUE_FULL.
     *
     * InFlight means the ARQ holds it -- and the ARQ is RAM. After a reset there
     * is no slot, no timer, no attempt count, and app::Node::promoteQueued looks
     * only at Pending. The entry can reach no outcome at all, which is the
     * silent drop CLAUDE.md 2.4 forbids.
     *
     * Gate 3.1 missed it because its twenty messages were still Pending: they
     * had never been sent, so there was nothing to strand.
     */
    fakes::FakeBlockStore store;
    store.configure(hal::StoreRegion::MessageQueue, kBlobBytes, 1);

    uint32_t strandedId = 0;
    {
        MessageQueue queue(store);
        REQUIRE(submitText(queue, 2, "one", 1000, &strandedId) == Accept::Ok);
        uint32_t second = 0;
        REQUIRE(submitText(queue, 2, "two", 1000, &second) == Accept::Ok);

        // The first goes out and is waiting for an ACK; the second is still
        // waiting its turn. Then the power goes.
        REQUIRE(queue.setState(strandedId, MessageState::InFlight, 1));
        REQUIRE(queue.countInState(MessageState::InFlight) == 1);
        REQUIRE(queue.countInState(MessageState::Pending) == 1);
        REQUIRE(queue.persist());
    }

    MessageQueue revived(store);
    REQUIRE(revived.restore() == 2);

    CHECK(revived.countInState(MessageState::InFlight) == 0);
    CHECK(revived.countInState(MessageState::Pending) == 2);

    // And it is offered again, from the first attempt -- the attempt count
    // belonged to an ARQ that no longer exists.
    const QueuedMessage *next = revived.nextSendable();
    REQUIRE(next != nullptr);
    CHECK(next->id == strandedId);
    CHECK(next->attempts == 0);

    // The revival is on flash, so a second reset does not have to rediscover it.
    MessageQueue again(store);
    REQUIRE(again.restore() == 2);
    CHECK(again.countInState(MessageState::Pending) == 2);
}

TEST_CASE("submit assigns ascending seqs, and a reboot does not restart them at zero")
{
    /*
     * Until 2026-08-31 nothing assigned QueuedMessage::seq at all: every frame
     * went on air as seq 0 and the receiver's dedupe rejected the whole session
     * after the first frame. The seq comes from submit, and it survives a
     * reboot -- a sender that restarts at 0 walks straight into the peer's
     * 8-deep seq history and its first messages are "delivered" without ever
     * being delivered.
     */
    fakes::FakeBlockStore store;

    {
        MessageQueue queue(store);
        for (int i = 0; i < 5; ++i) {
            REQUIRE(submitText(queue, 2, "msg", 1000) == Accept::Ok);
        }
        for (size_t i = 0; i < 5; ++i) {
            CHECK(queue.at(i).seq == i);
        }
    }

    MessageQueue restored(store);
    restored.restore();
    uint32_t id = 0;
    REQUIRE(submitText(restored, 2, "danach", 2000, &id) == Accept::Ok);
    // Not 0, and not any seq the peer could still remember.
    CHECK(restored.at(restored.size() - 1).seq == 5);
}

TEST_CASE("a legacy blob without a stored seq skips past the peer's dedupe history")
{
    fakes::FakeBlockStore store;

    {
        MessageQueue queue(store);
        for (int i = 0; i < 3; ++i) {
            REQUIRE(submitText(queue, 2, "alt", 1000) == Accept::Ok);
        }
    }

    // Age the blob: byte 7 is the "seq stored" flag, zero in every blob written
    // before 2026-08-31.
    uint8_t blob[kBlobBytes];
    REQUIRE(store.read(hal::StoreRegion::MessageQueue, 0, blob, kBlobBytes)
            == hal::StoreResult::Ok);
    blob[6] = 0;
    blob[7] = 0;
    REQUIRE(store.replaceAll(hal::StoreRegion::MessageQueue, blob, kBlobBytes)
            == hal::StoreResult::Ok);

    MessageQueue restored(store);
    restored.restore();
    REQUIRE(submitText(restored, 2, "neu", 2000) == Accept::Ok);
    // Highest stored seq is 2; +1 for the next, +8 past link::kSeqHistory.
    CHECK(restored.at(restored.size() - 1).seq == 11);
}

TEST_CASE("gate 3.2 / D18 -- a full queue with nothing delivered truncates the oldest failure")
{
    fakes::FakeBlockStore store;
    MessageQueue queue(store);

    uint32_t failedId = 0;
    REQUIRE(submitText(queue, 7, "will fail here", 1000, &failedId) == Accept::Ok);
    REQUIRE(queue.setState(failedId, MessageState::Undelivered, 4));
    for (uint32_t i = 1; i < kQueueCapacity; ++i) {
        REQUIRE(submitText(queue, 2, "wartet", 1000u + i) == Accept::Ok);
    }
    REQUIRE(queue.size() == kQueueCapacity);
    REQUIRE(queue.countInState(MessageState::Delivered) == 0);

    // The 25th message: room is made by reducing the failure to its head, not
    // by dropping it silently and not by refusing the new message.
    REQUIRE(submitText(queue, 2, "neu", 5000) == Accept::Ok);
    CHECK(queue.size() == kQueueCapacity);
    REQUIRE(queue.stubCount() == 1);
    CHECK(queue.stubAt(0).dst == 7);
    CHECK(queue.stubAt(0).attempts == 4);
    CHECK(queue.stubAt(0).createdAtUnix == 1000);

    // Nothing delivered and nothing undelivered left: now, and only now, Full.
    CHECK(submitText(queue, 2, "zu viel", 6000) == Accept::Full);

    // The heads survive a reboot with the rest of the queue.
    MessageQueue restored(store);
    restored.restore();
    REQUIRE(restored.stubCount() == 1);
    CHECK(restored.stubAt(0).dst == 7);
    CHECK(restored.stubAt(0).createdAtUnix == 1000);
}

TEST_CASE("D18 -- a delivered entry is still evicted before anything is truncated")
{
    fakes::FakeBlockStore store;
    MessageQueue queue(store);

    uint32_t deliveredId = 0;
    uint32_t undeliveredId = 0;
    REQUIRE(submitText(queue, 2, "kam an", 1000, &deliveredId) == Accept::Ok);
    REQUIRE(submitText(queue, 3, "kam nie an", 1001, &undeliveredId) == Accept::Ok);
    REQUIRE(queue.setState(deliveredId, MessageState::Delivered, 1));
    REQUIRE(queue.setState(undeliveredId, MessageState::Undelivered, 4));
    while (queue.size() < kQueueCapacity) {
        REQUIRE(submitText(queue, 2, "wartet", 2000) == Accept::Ok);
    }

    REQUIRE(submitText(queue, 2, "neu", 3000) == Accept::Ok);
    // The delivered entry went; the failure kept its text.
    CHECK(queue.stubCount() == 0);
    CHECK(queue.countInState(MessageState::Undelivered) == 1);
}

TEST_CASE("D18 -- the seventeenth head pushes the oldest off the ring, and it is counted")
{
    fakes::FakeBlockStore store;
    MessageQueue queue(store);

    while (queue.size() < kQueueCapacity) {
        REQUIRE(submitText(queue, 2, "wartet", 1000) == Accept::Ok);
    }

    for (uint32_t k = 0; k < kMaxStubs + 1; ++k) {
        // Fail the oldest waiting entry, then submit: the failure is truncated.
        const QueuedMessage &oldest = queue.at(0);
        REQUIRE(queue.setState(oldest.id, MessageState::Undelivered, 4));
        REQUIRE(submitText(queue, static_cast<uint16_t>(100 + k), "nachschub", 2000u + k)
                == Accept::Ok);
    }

    CHECK(queue.stubCount() == kMaxStubs);
    CHECK(queue.stubsDropped() == 1);
}
