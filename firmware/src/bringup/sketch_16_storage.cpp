/*
 * Bring-up 16 -- the three storage gates, on real flash and across a real reset.
 *
 * docs/test-plan.md 3.1, 3.2 and 3.3 have been "met in simulation, not claimed"
 * since the application layer was written. The simulation uses a fake block
 * store with fault injection, which is the right tool for the failure paths and
 * says nothing about LittleFS on a ZD25WQ16B or about what survives an actual
 * NVIC_SystemReset.
 *
 *   3.1  Queue survives reboot with 20 pending messages
 *        -> all present, order preserved, no duplicates
 *   3.2  Queue full behaviour
 *        -> oldest DELIVERED dropped first; user-visible; never a crash
 *   3.3  Journal wraparound
 *        -> fill the region; oldest acked entries reclaimed correctly
 *
 * Only 3.1 needs the reset, and it needs a real one -- so this follows sketch
 * 04's shape: a window in which the host can see the report and interrupt,
 * TinyUSBDevice.detach() before resetting, and the dead man's timer armed
 * throughout. The missing detach() is what wedged the laptop's xHCI controller
 * in session 3.
 *
 * THE PHASE IS READ OUT OF THE FLASH, NOT REMEMBERED
 *
 * There is no phase counter anywhere. On boot the queue is restored and looked
 * at: twenty entries carrying this sketch's marker text means we are on the far
 * side of the reset, anything else means we are on the near side. Self
 * describing, and re-runnable without a special "start over" path -- which
 * matters because GPREGRET2 does not survive here (the UF2 bootloader clears it
 * before application code runs, found in session 3).
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 16

#include <Arduino.h>
#include <Wire.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <Adafruit_SPIFlash.h>
#include <Adafruit_TinyUSB.h>
#pragma GCC diagnostic pop

#include <string.h>

#include "app/journal.h"
#include "app/message_queue.h"
#include "common.h"
#include "hal/block_store_littlefs.h"
#include "hal/external_flash.h"
#include "hal/clock_pcf8563.h"
#include "report.h"

namespace {

Adafruit_FlashTransport_QSPI g_transport;
Adafruit_SPIFlash g_flash(&g_transport);

hal::LittleFsBlockStore g_store;
hal::Pcf8563Clock g_clock;

/// Gate 3.1's number.
constexpr size_t kPendingMessages = 20;

/// The window before the reset, so the host can see the first report and stop
/// this if it wants to. Sketch 04 measured what happens without one.
constexpr uint32_t kWindowMs = 6000;

/// Marks a message as ours. Anything else in the queue means a previous run of
/// something else left it there, and this sketch starts over rather than
/// reading it as its own.
const char kMarker[] = "g31-";

bool g_storeOk = false;
bool g_afterReset = false;
bool g_reported = false;
bool g_done = false;
uint32_t g_windowOpenedAt = 0;

struct Results {
    // --- 3.1 ---
    size_t restored = 0;
    bool allPresent = false;
    bool orderPreserved = false;
    bool noDuplicates = false;

    // --- 3.2 ---
    bool queueFullRan = false;
    size_t sizeAtCapacity = 0;
    bool evictedDelivered = false;
    bool keptUndelivered = false;
    bool truncatedOnFull = false;   ///< D18: a failure was reduced, message taken
    size_t truncatedHeads = 0;
    bool refusedWhenNoneDelivered = false;

    // --- 3.3 ---
    bool journalRan = false;
    size_t journalHeld = 0;
    uint32_t journalLost = 0;
    bool reclaimedOnAck = false;
    bool oldestSurvivorCorrect = false;
};

Results g_results;

/*
 * File scope, and this is not a style preference.
 *
 * The core gives loop() a 4 kB FreeRTOS stack (LOOP_STACK_SZ in
 * cores/nRF5/main.cpp, 1024 words). app::Journal holds 128 records of 70 bytes
 * -- 8960 bytes -- and app::MessageQueue another 1.6 kB. Declaring either as a
 * local overruns that stack on the first instruction that touches it.
 *
 * The first version of this sketch did exactly that, and the symptom was not a
 * compiler error or a crash message: the device enumerated, faulted before udev
 * had created the port node, and vanished from USB entirely. With CFG_DEBUG=1
 * the fault handler halts at a breakpoint instead of resetting, so the dead
 * man's timer never ran either -- and because PIN_PWR_ON latches the battery,
 * unplugging USB does not reset it. Only the reset button does. It cost an
 * afternoon of device time.
 *
 * platformio.ini now passes -Wframe-larger-than so the compiler catches the
 * next one.
 */
app::MessageQueue g_queue{g_store};
app::Journal g_journal{g_store};

void markerText(char *out, size_t size, size_t index)
{
    // "g31-07". Fixed width so the order check can compare strings directly.
    snprintf(out, size, "%s%02u", kMarker, static_cast<unsigned>(index));
}

bool looksLikeOurQueue(const app::MessageQueue &queue)
{
    if (queue.size() != kPendingMessages) {
        return false;
    }
    return memcmp(queue.at(0).text, kMarker, sizeof(kMarker) - 1) == 0;
}

/// Fill the queue with the twenty that must survive, then persist.
bool writePending(app::MessageQueue &queue)
{
    for (size_t i = 0; i < kPendingMessages; ++i) {
        char text[16];
        markerText(text, sizeof(text), i);
        // dst carries the index too, so the order check does not rest on the
        // text alone.
        const app::Accept accepted = queue.submit(static_cast<uint16_t>(1000 + i), text,
                                                  strlen(text), g_clock.unixSeconds());
        if (accepted != app::Accept::Ok) {
            return false;
        }
    }
    return queue.persist();
}

/// Gate 3.1: what came back across the reset.
void checkRestored(const app::MessageQueue &queue, Results &r)
{
    r.restored = queue.size();
    r.allPresent = queue.size() == kPendingMessages;
    if (!r.allPresent) {
        return;
    }

    r.orderPreserved = true;
    r.noDuplicates = true;
    for (size_t i = 0; i < kPendingMessages; ++i) {
        char expected[16];
        markerText(expected, sizeof(expected), i);

        const app::QueuedMessage &entry = queue.at(i);
        if (entry.dst != static_cast<uint16_t>(1000 + i) ||
            entry.textLen != strlen(expected) ||
            memcmp(entry.text, expected, entry.textLen) != 0) {
            r.orderPreserved = false;
        }

        // Ids come from a monotonic supply; a duplicate would mean the restore
        // read one record twice.
        for (size_t j = i + 1; j < kPendingMessages; ++j) {
            if (queue.at(i).id == queue.at(j).id) {
                r.noDuplicates = false;
            }
        }
    }
}

/*
 * Gate 3.2. The queue holds 24; it already holds 20.
 *
 * Four more fill it. Then one more must evict the oldest DELIVERED entry and
 * nothing else -- CLAUDE.md 2.4 says an undelivered frame "is not silently
 * dropped", and evicting one to make room is precisely that.
 */
void checkQueueFull(app::MessageQueue &queue, Results &r)
{
    const uint32_t now = g_clock.unixSeconds();

    // Mark two as delivered and one as undelivered, so the eviction has a real
    // choice to get wrong.
    const uint32_t firstDeliveredId = queue.at(0).id;
    queue.setState(queue.at(0).id, app::MessageState::Delivered);
    queue.setState(queue.at(1).id, app::MessageState::Undelivered);
    queue.setState(queue.at(2).id, app::MessageState::Delivered);
    const uint32_t undeliveredId = queue.at(1).id;
    const uint32_t secondDeliveredId = queue.at(2).id;

    while (queue.size() < app::kQueueCapacity) {
        char text[16];
        markerText(text, sizeof(text), queue.size());
        if (queue.submit(9000, text, strlen(text), now) != app::Accept::Ok) {
            return;
        }
    }
    r.sizeAtCapacity = queue.size();

    // One more: the OLDEST DELIVERED must go, and the undelivered must stay.
    if (queue.submit(9999, "evict", 5, now) != app::Accept::Ok) {
        return;
    }

    bool sawFirstDelivered = false;
    bool sawUndelivered = false;
    bool sawSecondDelivered = false;
    for (size_t i = 0; i < queue.size(); ++i) {
        if (queue.at(i).id == firstDeliveredId) sawFirstDelivered = true;
        if (queue.at(i).id == undeliveredId) sawUndelivered = true;
        if (queue.at(i).id == secondDeliveredId) sawSecondDelivered = true;
    }
    r.evictedDelivered = !sawFirstDelivered && sawSecondDelivered;
    r.keptUndelivered = sawUndelivered;

    /*
     * D18 from here on. The first run of this sketch tried to park the
     * remaining delivered entry with setState(Delivered -> InFlight) -- which
     * the terminal-state guard silently refuses, so the "nothing evictable"
     * fixture never existed and the check failed against a correct queue.
     * Terminal to terminal passes the guard, and the return value is checked
     * now.
     */
    if (!queue.setState(secondDeliveredId, app::MessageState::Undelivered, 3)) {
        return;
    }
    while (queue.size() < app::kQueueCapacity) {
        if (queue.submit(9001, "fill", 4, now) != app::Accept::Ok) {
            return;
        }
    }

    // Full, nothing delivered, failures present: the oldest failure is reduced
    // to its head and the message is taken (decision D18).
    const size_t stubsBefore = queue.stubCount();
    r.truncatedOnFull = queue.submit(9002, "over", 4, now) == app::Accept::Ok &&
                        queue.stubCount() == stubsBefore + 1;

    // Reduce every remaining failure the same way; then, with everything still
    // in progress, the queue must REFUSE rather than drop something.
    while (queue.countInState(app::MessageState::Undelivered) > 0) {
        if (queue.submit(9003, "mehr", 4, now) != app::Accept::Ok) {
            return;
        }
    }
    r.truncatedHeads = queue.stubCount();
    r.refusedWhenNoneDelivered = queue.submit(9004, "zuviel", 6, now) == app::Accept::Full;
    r.queueFullRan = true;
}

/*
 * Gate 3.3. The journal holds 128 entries and only acknowledge() frees space.
 *
 * Filling past that must overwrite the oldest and count what was lost --
 * app/journal.h: "a hole nobody knows about is worse than one that is
 * reported". Then acknowledging up to a counter must reclaim exactly those and
 * leave the rest.
 */
void checkJournal(app::Journal &journal, Results &r)
{
    /*
     * Start from an empty region. The restore that ran before this call brought
     * back whatever the previous run -- or the node itself -- left behind, and
     * append() refuses any counter at or below the highest it holds. The first
     * run of this sketch skipped this and its fixture died on append(1, ...)
     * against a single stale record: g33.held = 0, exactly the report it
     * produced. The erase used to sit AFTER the check, cleaning up for a next
     * run instead of preparing this one.
     */
    if (g_store.erase(hal::StoreRegion::EventJournal) != hal::StoreResult::Ok) {
        return;
    }
    journal.restore();

    const size_t overfill = app::kJournalCapacity + 40;
    uint8_t body[8];

    for (size_t i = 0; i < overfill; ++i) {
        // Counters come from the same monotonic supply as frame counters and
        // start at 1 -- zero means "no counter" everywhere else.
        const uint32_t counter = static_cast<uint32_t>(i + 1);
        memcpy(body, &counter, sizeof(counter));
        memcpy(body + 4, &counter, sizeof(counter));
        if (!journal.append(counter, 0x81, body, sizeof(body))) {
            return;
        }
        /*
         * append() persists the whole region every time: 168 erase-and-rewrite
         * passes over 128 x 70 bytes of LittleFS, in one loop() iteration. The
         * dead man stands at 180 s and nothing here poked it -- the likeliest
         * story of the run that ended in the bootloader with no report.
         */
        bringup::commonService();
    }

    r.journalHeld = journal.size();
    r.journalLost = journal.lostEntries();

    // Overfilled by 40, so 40 must have been reported lost and the region must
    // hold exactly its capacity.
    const bool heldRight = journal.size() == app::kJournalCapacity;
    const bool lostRight = journal.lostEntries() == 40;

    // The oldest survivor is the 41st appended.
    const uint32_t expectedOldest = 41;
    r.oldestSurvivorCorrect = heldRight && lostRight && journal.at(0).counter == expectedOldest;

    // Acknowledge half of what is held; exactly that many must go.
    const size_t before = journal.size();
    const uint32_t ackUpTo = journal.at(before / 2).counter;
    const size_t freed = journal.acknowledge(ackUpTo);
    r.reclaimedOnAck = freed == (before / 2) + 1 && journal.size() == before - freed &&
                       journal.size() > 0 && journal.at(0).counter > ackUpTo;

    r.journalRan = true;
}

void printReport(const char *phase)
{
    const Results &r = g_results;

    report::begin(16);
    report::value("phase", "%s", phase);
    report::value("store.mounted", "%d", g_storeOk ? 1 : 0);
    report::value("after_reset", "%d", g_afterReset ? 1 : 0);

    if (!g_afterReset) {
        report::value("queue.written", "%u", static_cast<unsigned>(kPendingMessages));
        report::info("16: resetting to prove the queue survives it -- gate 3.1");
        report::verdict("inconclusive", "the near side of the reset; the report after it decides");
        report::end(16);
        return;
    }

    // --- 3.1 ---
    report::value("g31.restored", "%u", static_cast<unsigned>(r.restored));
    report::value("g31.all_present", "%d", r.allPresent ? 1 : 0);
    report::value("g31.order_preserved", "%d", r.orderPreserved ? 1 : 0);
    report::value("g31.no_duplicates", "%d", r.noDuplicates ? 1 : 0);
    const bool gate31 = r.allPresent && r.orderPreserved && r.noDuplicates;
    report::value("gate_3_1.pass", "%d", gate31 ? 1 : 0);

    // --- 3.2 ---
    report::value("g32.size_at_capacity", "%u", static_cast<unsigned>(r.sizeAtCapacity));
    report::value("g32.evicted_oldest_delivered", "%d", r.evictedDelivered ? 1 : 0);
    report::value("g32.kept_undelivered", "%d", r.keptUndelivered ? 1 : 0);
    report::value("g32.truncated_on_full", "%d", r.truncatedOnFull ? 1 : 0);
    report::value("g32.truncated_heads", "%u", static_cast<unsigned>(r.truncatedHeads));
    report::value("g32.refused_when_none_delivered", "%d", r.refusedWhenNoneDelivered ? 1 : 0);
    const bool gate32 = r.queueFullRan && r.sizeAtCapacity == app::kQueueCapacity &&
                        r.evictedDelivered && r.keptUndelivered && r.truncatedOnFull &&
                        r.refusedWhenNoneDelivered;
    report::value("gate_3_2.pass", "%d", gate32 ? 1 : 0);

    // --- 3.3 ---
    report::value("g33.held", "%u", static_cast<unsigned>(r.journalHeld));
    report::value("g33.capacity", "%u", static_cast<unsigned>(app::kJournalCapacity));
    report::value("g33.lost", "%lu", static_cast<unsigned long>(r.journalLost));
    report::value("g33.oldest_survivor_correct", "%d", r.oldestSurvivorCorrect ? 1 : 0);
    report::value("g33.reclaimed_on_ack", "%d", r.reclaimedOnAck ? 1 : 0);
    const bool gate33 = r.journalRan && r.oldestSurvivorCorrect && r.reclaimedOnAck;
    report::value("gate_3_3.pass", "%d", gate33 ? 1 : 0);

    const bool ok = gate31 && gate32 && gate33;
    report::verdict(ok ? "pass" : "fail",
                    ok ? "gates 3.1, 3.2 and 3.3 on LittleFS on the ZD25WQ16B, 3.1 across a "
                         "real NVIC_SystemReset"
                       : "see the failing key above");
    report::end(16);
}

} // namespace

namespace bringup {

void setup()
{
    commonSetup(180000);
    Wire.begin();
    g_clock.begin();

    if (hal::openExternalFlash(g_transport, g_flash)) {
        g_storeOk = g_store.begin(g_flash);
    }
    if (!g_storeOk) {
        return;
    }

    g_store.configureRegion(hal::StoreRegion::FrameCounter, 8, 64);
    g_store.configureRegion(hal::StoreRegion::BudgetRing, 10, 512);
    g_store.configureRegion(hal::StoreRegion::MessageQueue, app::kBlobBytes, 1);
    g_store.configureRegion(hal::StoreRegion::EventJournal, app::kJournalRecordBytes,
                            app::kJournalCapacity);
}

void loop()
{
    if (!usbReady()) {
        delay(100);
        commonService();
        return;
    }

    if (!g_storeOk) {
        report::begin(16);
        report::verdict("fail", "the block store did not mount");
        report::end(16);
        delay(3000);
        commonService();
        return;
    }

    if (!g_done) {
        g_queue.restore();
        g_afterReset = looksLikeOurQueue(g_queue);

        if (g_afterReset) {
            checkRestored(g_queue, g_results);
            checkQueueFull(g_queue, g_results);

            g_journal.restore();
            checkJournal(g_journal, g_results);

            // Leave nothing behind: the next run starts from an empty queue and
            // detects the near side of the reset again.
            g_store.erase(hal::StoreRegion::MessageQueue);
            g_store.erase(hal::StoreRegion::EventJournal);
        } else {
            // Start clean, then write the twenty that have to survive.
            g_store.erase(hal::StoreRegion::MessageQueue);
            g_queue.restore();

            /*
             * Written AND read back before anything resets.
             *
             * The phase is worked out by looking at the flash. If the write
             * silently failed, the next boot would find no queue of ours, write
             * again, and reset again -- for ever, re-enumerating USB every few
             * seconds. That is precisely the shape that wedged the host's xHCI
             * controller in session 3, and it is not a risk worth taking to save
             * one read.
             *
             * So: verify here, and if it did not take, report and stay put.
             */
            if (!writePending(g_queue)) {
                report::begin(16);
                report::verdict("fail", "could not write the 20 pending messages");
                report::end(16);
                g_done = true;
                delay(3000);
                commonService();
                return;
            }

            g_queue.restore();
            if (!looksLikeOurQueue(g_queue)) {
                report::begin(16);
                report::value("store.mounted", "%d", 1);
                report::value("write.read_back", "%d", 0);
                report::verdict("fail",
                                "the 20 messages did not read back from flash -- not "
                                "resetting, because a reset here would loop for ever");
                report::end(16);
                g_done = true;
                g_reported = true;
                g_afterReset = true; // stops the reset path below
                delay(3000);
                commonService();
                return;
            }
        }
        g_done = true;
    }

    if (!g_reported) {
        printReport(g_afterReset ? "after" : "before");
        g_reported = true;
    }

    if (g_afterReset) {
        // Nothing left to do; keep reporting so a late reader sees it.
        printReport("after");
        delay(3000);
        commonService();
        return;
    }

    // The window: the host has seen the report and may interrupt. Measured from
    // USB being ready, not from boot -- from boot it is usually already over by
    // the time enumeration finishes, which is how sketch 04 wedged a host port.
    if (g_windowOpenedAt == 0) {
        g_windowOpenedAt = millis();
    }
    if (millis() - g_windowOpenedAt < kWindowMs) {
        delay(50);
        commonService();
        return;
    }

    report::info("16: resetting now");
    Serial.flush();
    delay(50);

    // Detach before resetting. Vanishing mid-transaction is what produced
    // "device not accepting address ... error -71" and cost a night.
    TinyUSBDevice.detach();
    delay(250);
    NVIC_SystemReset();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 16
