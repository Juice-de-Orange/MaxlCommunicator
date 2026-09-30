/*
 * Bring-up 18 -- the frame counter across fifty forced power cycles.
 *
 * docs/test-plan.md 2.6: "50 forced power cycles, some mid-transmit ->
 * Counter never repeats or goes backwards across the whole log."
 *
 * This is the gate that makes CLAUDE.md 2.1 true rather than intended. The CCM
 * nonce is built from src and counter; a counter that repeats after a reboot
 * means a nonce that repeats under a shared key, and CCM breaks open at that
 * point. It is also why 2.1 asks for the counter to be reserved in blocks and
 * resumed from the high-water mark rather than from the last value used.
 *
 * "Some mid-transmit" is the part that needs a radio, and it is the interesting
 * part: a reset while a frame is in the air is when the reserved block has been
 * drawn from but not yet written back. Every fifth cycle resets during a
 * transmission rather than between them.
 *
 * THE LOG LIVES IN FLASH, BECAUSE THAT IS THE POINT
 *
 * Fifty cycles cannot be held in RAM across fifty resets. Each boot appends what
 * it saw as one 16-byte record on hal::StoreRegion::BenchScratch, and the last
 * boot reads all fifty back and checks them against each other. Checking each
 * cycle against only the previous one would miss a counter that went backwards
 * and then forwards again.
 *
 * The first run of this sketch (2026-08-31, node B) did neither of the things
 * it claimed. It read app::Journal::highestCounter() -- which was its own cycle
 * number, because the scratch log lived in the journal's region -- and called
 * it the frame counter; and its verify() flagged the final, non-drawing boot as
 * a repeat. Now it reads Node::frameCounterPeek()/frameCounterReservedUpTo(),
 * the log has its own region (the node journals real events into the journal
 * region since the send path works), and the verifying boot appends nothing so
 * there is nothing to misjudge.
 *
 * NEVER RUN IT WITHOUT AN ANTENNA. It transmits, at nobody, on g3.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 18

#include <Arduino.h>
#include <Wire.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <Adafruit_SPIFlash.h>
#include <Adafruit_TinyUSB.h>
#pragma GCC diagnostic pop

#include <string.h>

#include "app/node.h"
#include "bench_config.h"
#include "bench_key.h"
#include "common.h"
#include "hal/block_store_littlefs.h"
#include "hal/external_flash.h"
#include "hal/clock_pcf8563.h"
#include "hal/key_store_internal.h"
#include "hal/radio_sx1262.h"
#include "report.h"

namespace {

Adafruit_FlashTransport_QSPI g_transport;
Adafruit_SPIFlash g_flash(&g_transport);

hal::LittleFsBlockStore g_store;
hal::Pcf8563Clock g_clock;
hal::InternalKeyStore g_keys;
hal::Sx1262Radio g_radio;
app::Node g_node{g_clock, g_store, g_keys};

class MiddleJitter : public link::IJitterSource {
public:
    uint32_t next(uint32_t bound) override { return bound == 0 ? 0 : (bound - 1) / 2; }
};
MiddleJitter g_jitter;

constexpr size_t kTargetCycles = 50;

/// Every fifth reset happens with a frame in the air rather than between them.
constexpr size_t kMidTransmitEvery = 5;

/// Long enough for the host to see the report and interrupt; short enough that
/// fifty of them are ten minutes rather than an hour.
constexpr uint32_t kWindowMs = 4000;

/// One observation per boot: cycle u32, counter at boot u32, counter after
/// draws u32, mid-transmit flag u32.
constexpr size_t kScratchRecordBytes = 16;

bool g_storeOk = false;
/// True when the key came from MAXL_DEV_KEY, i.e. when another board can match it.
bool g_keyShared = false;
bool g_nodeOk = false;
bool g_radioOk = false;
size_t g_cycle = 0;
uint32_t g_windowOpenedAt = 0;
bool g_reported = false;

uint32_t g_counterAtBoot = 0;
uint32_t g_reservedAtBoot = 0;
uint32_t g_counterAfterDraws = 0;

struct Verdicts {
    bool ran = false;
    size_t cyclesSeen = 0;
    bool neverRepeated = true;
    bool neverWentBackwards = true;
    uint32_t firstCounter = 0;
    uint32_t lastCounter = 0;
    size_t midTransmitResets = 0;
};

Verdicts g_verdicts;

void putU32(uint8_t *out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t readU32(const uint8_t *in)
{
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}


/// Append this boot's observations: cycle, counter at boot, counter after draws.
bool recordCycle(size_t cycle, uint32_t atBoot, uint32_t afterDraws, bool midTransmit)
{
    uint8_t body[kScratchRecordBytes];
    putU32(body + 0, static_cast<uint32_t>(cycle));
    putU32(body + 4, atBoot);
    putU32(body + 8, afterDraws);
    putU32(body + 12, midTransmit ? 1u : 0u);
    return g_store.append(hal::StoreRegion::BenchScratch, body, sizeof(body))
           == hal::StoreResult::Ok;
}

/*
 * The whole log at once.
 *
 * The counters a cycle used are [atBoot, afterDraws); cycles must not overlap,
 * and nothing may ever move backwards. `atBoot` is peek() -- the NEXT value a
 * draw would return -- so the boot after a cycle that ended at afterDraws may
 * legitimately read atBoot == afterDraws (nothing drawn in between) or above it
 * (the reservation block skipped ahead, which CLAUDE.md 2.1 requires after a
 * mid-transmit reset). Only atBoot BELOW a previous afterDraws is a repeat.
 * Comparing each cycle against the running maximum rather than only its
 * neighbour catches a counter that went backwards and then forwards again --
 * exactly the shape a half-written reservation block would produce.
 */
void verify(Verdicts &v)
{
    v.cyclesSeen = g_store.count(hal::StoreRegion::BenchScratch);
    if (v.cyclesSeen == 0) {
        return;
    }

    uint32_t highestUsed = 0;
    bool first = true;
    for (size_t i = 0; i < v.cyclesSeen; ++i) {
        uint8_t body[kScratchRecordBytes];
        if (g_store.read(hal::StoreRegion::BenchScratch, i, body, sizeof(body))
            != hal::StoreResult::Ok) {
            continue;
        }
        const uint32_t atBoot = readU32(body + 4);
        const uint32_t afterDraws = readU32(body + 8);
        const bool midTransmit = readU32(body + 12) != 0;

        if (midTransmit) {
            ++v.midTransmitResets;
        }
        if (first) {
            v.firstCounter = atBoot;
            first = false;
        }
        v.lastCounter = afterDraws;

        if (atBoot < highestUsed) {
            // This boot would hand out counters an earlier cycle already used.
            v.neverRepeated = false;
            v.neverWentBackwards = false;
        }
        if (afterDraws < atBoot) {
            v.neverWentBackwards = false;
        }
        if (afterDraws > highestUsed) {
            highestUsed = afterDraws;
        }
    }
    v.ran = true;
}

void printReport(const char *phase)
{
    report::begin(18);
    report::value("phase", "%s", phase);
    report::value("store.mounted", "%d", g_storeOk ? 1 : 0);
    report::value("node.ready", "%d", g_nodeOk ? 1 : 0);
    /*
     * Which node this image is, and whether its key can match another board's.
     *
     * A two-node run whose halves disagree about either fails as a total absence
     * of reception, which is the same symptom as a dead radio, a wrong frequency
     * and a missing antenna. Reporting both turns that into a glance.
     */
    report::value("node.id", "0x%04X", static_cast<unsigned>(bench::nodeId()));
    report::value("node.peer", "0x%04X", static_cast<unsigned>(bench::peerId()));
    report::value("key.shared", "%d", g_keyShared ? 1 : 0);
    report::value("radio.ready", "%d", g_radioOk ? 1 : 0);
    report::value("cycle", "%u", static_cast<unsigned>(g_cycle));
    report::value("cycle.target", "%u", static_cast<unsigned>(kTargetCycles));
    report::value("counter.at_boot", "%lu", static_cast<unsigned long>(g_counterAtBoot));
    report::value("counter.reserved_at_boot", "%lu",
                  static_cast<unsigned long>(g_reservedAtBoot));
    report::value("counter.after_draws", "%lu",
                  static_cast<unsigned long>(g_counterAfterDraws));

    if (g_verdicts.ran) {
        report::value("log.cycles", "%u", static_cast<unsigned>(g_verdicts.cyclesSeen));
        report::value("log.mid_transmit_resets", "%u",
                      static_cast<unsigned>(g_verdicts.midTransmitResets));
        report::value("log.first_counter", "%lu",
                      static_cast<unsigned long>(g_verdicts.firstCounter));
        report::value("log.last_counter", "%lu",
                      static_cast<unsigned long>(g_verdicts.lastCounter));
        report::value("log.never_repeated", "%d", g_verdicts.neverRepeated ? 1 : 0);
        report::value("log.never_backwards", "%d", g_verdicts.neverWentBackwards ? 1 : 0);

        const bool ok = g_verdicts.cyclesSeen >= kTargetCycles && g_verdicts.neverRepeated &&
                        g_verdicts.neverWentBackwards && g_verdicts.midTransmitResets > 0;
        report::value("gate_2_6.pass", "%d", ok ? 1 : 0);
        report::verdict(ok ? "pass" : "fail",
                        ok ? "the frame counter never repeated and never went backwards across "
                             "fifty forced power cycles, some of them mid-transmit"
                           : "see the failing key above");
    } else {
        report::verdict("inconclusive", "still cycling; the last boot decides");
    }
    report::end(18);
}

} // namespace

namespace bringup {

void setup()
{
    commonSetup(180000);
    Wire.begin();
    g_clock.begin();
    g_keys.begin();

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
    g_store.configureRegion(hal::StoreRegion::BenchScratch, kScratchRecordBytes,
                            kTargetCycles + 8);

    g_nodeOk = g_node.begin(bench::nodeId());

    uint8_t key[16];
    g_keyShared = bench::networkKey(key);
    g_node.provisionKey(0, bench::kNetId, key);

    const hal::Modulation rendezvous{9, 869575000u, 22};
    g_radioOk = g_radio.begin(rendezvous);
    if (g_radioOk) {
        g_node.attachRadio(g_radio, g_jitter);
    }
    // The gate compares airtimes and lockouts against the SF9 table; adaptive
    // SF moving mid-run would quietly change both.
    bench::pinFixedSf9(g_node);
}

void loop()
{
    if (!usbReady() || !g_storeOk) {
        delay(100);
        commonService();
        return;
    }

    g_cycle = g_store.count(hal::StoreRegion::BenchScratch);

    if (g_cycle >= kTargetCycles) {
        verify(g_verdicts);
        printReport("done");
        // Leave nothing behind. The next run starts over.
        g_store.erase(hal::StoreRegion::BenchScratch);
        for (;;) {
            printReport("done");
            delay(3000);
            commonService();
        }
    }

    /*
     * What the counter looked like before this boot's window drew anything, and
     * what the reservation resumed from. CLAUDE.md 2.1: resume from the
     * reserved high-water mark, never from the last value used. These are the
     * real FrameCounter figures -- the first run of this sketch read the
     * journal's counter and measured its own loop variable.
     */
    g_counterAtBoot = g_node.frameCounterPeek();
    g_reservedAtBoot = g_node.frameCounterReservedUpTo();

    // Draw counters by queueing and pumping. Each accepted message consumes one.
    const bool midTransmit = (g_cycle % kMidTransmitEvery) == (kMidTransmitEvery - 1);
    for (int i = 0; i < 3; ++i) {
        char text[16];
        snprintf(text, sizeof(text), "g26-%02u-%d", static_cast<unsigned>(g_cycle), i);
        g_node.sendText(bench::peerId(), reinterpret_cast<const uint8_t *>(text), strlen(text));
    }
    const uint32_t now = millis();
    g_node.tick(now);
    g_node.pumpRadio(now);
    g_counterAfterDraws = g_node.frameCounterPeek();

    /*
     * The cycle number is the record count, so a record that does not land
     * means the next boot sees the same number, writes again, and resets again
     * -- fifty cycles becomes an endless one. A failure therefore stops rather
     * than resets. Sketch 16 carries the same guard for the same reason.
     */
    if (!recordCycle(g_cycle, g_counterAtBoot, g_counterAfterDraws, midTransmit)) {
        printReport("record_failed");
        for (;;) {
            printReport("record_failed");
            delay(3000);
            commonService();
        }
    }

    if (!g_reported) {
        printReport("cycling");
        g_reported = true;
    }

    if (g_windowOpenedAt == 0) {
        g_windowOpenedAt = millis();
    }
    if (millis() - g_windowOpenedAt < kWindowMs) {
        g_radio.service();
        delay(20);
        commonService();
        return;
    }

    if (midTransmit) {
        // Start something and do not wait for it. The reservation has been drawn
        // from; whether it was written back is what this cycle is asking.
        g_node.pumpRadio(millis());
        report::info("18: resetting MID-TRANSMIT, cycle %u", static_cast<unsigned>(g_cycle));
    } else {
        report::info("18: resetting between frames, cycle %u", static_cast<unsigned>(g_cycle));
    }
    Serial.flush();
    delay(20);

    TinyUSBDevice.detach();
    delay(250);
    NVIC_SystemReset();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 18
