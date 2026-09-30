/*
 * Bring-up 19 -- two nodes, one link. The test link/ was written for.
 *
 * `link/` has been finished for three sittings and has never spoken to anything
 * that answers. This is that: the same image on both boards, told apart by
 * MAXL_NODE_ID, sharing one key through MAXL_DEV_KEY.
 *
 *     MAXL_DEV_KEY=<32 hex> MAXL_NODE_ID=1 MAXL_BRINGUP=19 pio run -e bringup \
 *         -t upload --upload-port $(tools/nodes.py --port-of A)
 *     MAXL_DEV_KEY=<same>   MAXL_NODE_ID=2 MAXL_BRINGUP=19 pio run -e bringup \
 *         -t upload --upload-port $(tools/nodes.py --port-of B)
 *
 * Node 1 sends, node 2 answers -- but only because ACKs are what the ARQ does;
 * nothing here is asymmetric except which side calls sendText. Both count the
 * same things, and either may be the one with the cable.
 *
 * THE CABLE IS THE CONSTRAINT. One USB socket on the development machine died on
 * 2026-08-31, so only one board can be attached at a time. This sketch therefore
 * never waits for USB: the state machine runs on battery and the report is
 * printed whenever a host happens to be listening. Sketch 13 established the
 * pattern; the difference is that here it is not optional.
 *
 * WHAT IT COVERS
 *
 *   gate 2.1  frames exchanged at the rendezvous configuration, zero MIC
 *             failures on the receiver
 *   gate 2.4  every ACK_REQ frame acknowledged, and the RSSI/SNR the ACK carried
 *             recorded against the peer
 *
 * WHAT IT DOES NOT COVER, AND WHY
 *
 *   gates 2.2 and 2.3 need frames that are deliberately corrupted or repeated,
 *   which means putting a chosen buffer on the air. Node::buildFrame is private
 *   and should stay so -- it is the only thing standing between the application
 *   and a frame without a counter. Doing it properly needs a bench seam on
 *   app::Node rather than a second frame builder in a sketch, and inventing that
 *   under time pressure is how a test ends up proving the sketch rather than the
 *   firmware. Both are covered in test/sim/two_node.cpp today.
 *
 * THE SNIFF INTERVAL IS 500 ms HERE, NOT THE 2 s DEFAULT.
 *
 * The preamble is derived from it (CLAUDE.md 2.3), and at 2 s that is 489
 * symbols -- 2.2 s of airtime per frame, so the per-frame lockout alone puts a
 * hundred frames at over half an hour of wall clock. At 500 ms it is 123 symbols
 * and about 0.7 s, which is the same table, a row further up. Both nodes derive
 * it from the same function, so both agree, which is the property that matters.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 19

#include <Arduino.h>
#include <Wire.h>
#include <string.h>

// The core's SPI.h shadows two SPISettings members, and -Wshadow is an error
// in this project's own sources. Same guard as everywhere else that opens the
// external flash.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <Adafruit_SPIFlash.h>
#pragma GCC diagnostic pop

#include "bench_key.h"
#include "common.h"
#include "report.h"

#include "app/node.h"
#include "hal/block_store_littlefs.h"
#include "hal/clock_pcf8563.h"
#include "hal/external_flash.h"
#include "hal/key_store_internal.h"
#include "hal/radio_sx1262.h"
#include "link/arq.h"

namespace bringup {
namespace {

/*
 * The three knobs gate 2.1's full runs need, overridable from the environment
 * through scripts/bringup_flag.py -- MAXL_LINK_MESSAGES=100 MAXL_LINK_SF=12
 * MAXL_LINK_START_DELAY_MS=0 for a hub where both boards flash back to back.
 */
#ifndef MAXL_LINK_MESSAGES
#define MAXL_LINK_MESSAGES 20
#endif
#ifndef MAXL_LINK_SF
#define MAXL_LINK_SF 9
#endif
#ifndef MAXL_LINK_START_DELAY_MS
#define MAXL_LINK_START_DELAY_MS 180000
#endif

/// How many messages node 1 offers. Gate 2.1 wants 100; 20 is a first bench run.
constexpr uint32_t kMessages = MAXL_LINK_MESSAGES;

/// Gate 2.1 runs at SF7, SF9 and SF12; the rendezvous default is 9. Both nodes
/// must be flashed with the same value -- the sniffer's preamble follows it.
constexpr uint8_t kSf = MAXL_LINK_SF;

/// See the header: a row further up CLAUDE.md 2.3's table, not a different table.
constexpr uint16_t kSniffIntervalMs = 500;

constexpr uint32_t kReportEveryMs = 5000;

/*
 * How long node 1 waits before its first message.
 *
 * Only one board can be on USB at a time here, so the two are flashed one after
 * the other, and node 1 would otherwise spend its whole allowance talking to a
 * peer that is still being programmed. Each of those messages costs three ARQ
 * attempts and then sits in the queue as Undelivered for ever (D18) -- twenty of
 * them would fill it and the run would be over before it began.
 *
 * Three minutes is a cable swap and an upload with room to spare. It is a bench
 * convenience and nothing in link/ knows about it.
 */
constexpr uint32_t kStartDelayMs = MAXL_LINK_START_DELAY_MS;

Adafruit_FlashTransport_QSPI g_transport;
Adafruit_SPIFlash g_flash(&g_transport);

hal::LittleFsBlockStore g_store;
hal::Pcf8563Clock g_clock;
hal::InternalKeyStore g_keys;
hal::Sx1262Radio g_radio;

app::Node g_node{g_clock, g_store, g_keys};

/// Deterministic jitter: CLAUDE.md 2.4 wants +-25 % on the backoff, and for a
/// measurement the middle of the range is what makes two runs comparable.
class MiddleJitter : public link::IJitterSource {
public:
    uint32_t next(uint32_t bound) override { return bound == 0 ? 0 : (bound - 1) / 2; }
};
MiddleJitter g_jitter;

bool g_storeOk = false;
bool g_nodeOk = false;
bool g_radioOk = false;
bool g_keyShared = false;
bool g_configApplied = false;

uint32_t g_sent = 0;          ///< accepted by the queue, node 1 only
uint32_t g_submitFailures = 0;
uint8_t g_lastSubmitError = 0;
uint32_t g_lastReportMs = 0;
uint32_t g_startedMs = 0;

/*
 * One at a time.
 *
 * The queue holds 24 and the ARQ window is smaller than that, so filling it
 * would only mean waiting somewhere else -- and it would hide gate 2.4 behind a
 * pipeline: with several frames outstanding, "every ACK_REQ frame acknowledged"
 * stops being readable off a delivered count.
 */
void feed()
{
    if (bench::nodeId() != 1 || g_sent >= kMessages) {
        return;
    }
    if (millis() - g_startedMs < kStartDelayMs) {
        return;
    }
    if (g_node.queue().countInState(app::MessageState::Pending) > 0
        || g_node.queue().countInState(app::MessageState::InFlight) > 0) {
        return;
    }

    char text[24];
    snprintf(text, sizeof(text), "link %lu of %lu",
             static_cast<unsigned long>(g_sent + 1), static_cast<unsigned long>(kMessages));
    const uint8_t result =
        g_node.sendText(bench::peerId(), reinterpret_cast<const uint8_t *>(text), strlen(text));
    if (result == 0) {
        ++g_sent;
    } else {
        ++g_submitFailures;
        g_lastSubmitError = result;
    }
}

#ifdef MAXL_BENCH
/*
 * Gates 2.2 and 2.3, sender side. After the ordinary exchange is done, node 1
 * puts three kinds of chosen frames on the air through the bench seam -- every
 * one budget-gated by Node::benchTransmit, because the bench has no licence the
 * application lacks:
 *
 *   tamper_payload  a valid frame with one bit of ciphertext flipped
 *   tamper_header   a valid frame with one bit of the counter flipped (the
 *                   header is AAD, so the MIC must fail; netId or dst would be
 *                   dropped by cheaper checks before the MIC ever ran)
 *   replay          one valid frame delivered, then byte-exact repeats
 *
 * The judgement lives on the receiver: its rx.mic_failures must count 40 and
 * its rx.replays_rejected the repeats, with text events unchanged. This side
 * only reports what it sent.
 */
enum class BenchPhase : uint8_t { Idle, TamperPayload, TamperHeader, Replay, Done };
BenchPhase g_benchPhase = BenchPhase::Idle;
constexpr uint32_t kBenchFramesPerPhase = 20;
uint32_t g_tamperPayloadSent = 0;
uint32_t g_tamperHeaderSent = 0;
uint32_t g_replaysSent = 0;
uint8_t g_replayFrame[link::kMaxFrameBytes];
size_t g_replayFrameLen = 0;
uint8_t g_benchSeq = 0xB0;

void benchStep()
{
    if (bench::nodeId() != 1 || g_node.benchTxBusy()) {
        return;
    }

    if (g_benchPhase == BenchPhase::Idle) {
        const bool mainDone =
            g_sent >= kMessages
            && g_node.queue().countInState(app::MessageState::Pending) == 0
            && g_node.queue().countInState(app::MessageState::InFlight) == 0;
        if (!mainDone) {
            return;
        }
        g_benchPhase = BenchPhase::TamperPayload;
        report::info("19: bench phases begin -- gates 2.2/2.3");
    }

    uint8_t frame[link::kMaxFrameBytes];

    if (g_benchPhase == BenchPhase::TamperPayload || g_benchPhase == BenchPhase::TamperHeader) {
        const bool header = g_benchPhase == BenchPhase::TamperHeader;
        uint32_t &sent = header ? g_tamperHeaderSent : g_tamperPayloadSent;
        if (sent >= kBenchFramesPerPhase) {
            g_benchPhase = header ? BenchPhase::Replay : BenchPhase::TamperHeader;
            return;
        }
        char text[24];
        snprintf(text, sizeof(text), "tamper %lu", static_cast<unsigned long>(sent));
        const size_t len = g_node.benchFrame(bench::peerId(),
                                             reinterpret_cast<const uint8_t *>(text),
                                             strlen(text), g_benchSeq++, frame, sizeof(frame));
        if (len == 0) {
            return;
        }
        if (header) {
            // Bit (sent % 8) of the counter's low byte. Inside the AAD, past
            // the netId/dst checks, so the rejection must come from the MIC.
            frame[6] ^= static_cast<uint8_t>(1u << (sent % 8));
        } else {
            // Bit (sent % 8) of the first ciphertext byte.
            frame[12] ^= static_cast<uint8_t>(1u << (sent % 8));
        }
        if (g_node.benchTransmit(frame, len) == 0) {
            ++sent;
        }
        return;
    }

    if (g_benchPhase == BenchPhase::Replay) {
        if (g_replayFrameLen == 0) {
            // The one honest frame the repeats will copy. It reaches B's
            // application once; every repeat after it must not.
            const char text[] = "replay probe";
            g_replayFrameLen = g_node.benchFrame(bench::peerId(),
                                                 reinterpret_cast<const uint8_t *>(text),
                                                 sizeof(text) - 1, g_benchSeq, g_replayFrame,
                                                 sizeof(g_replayFrame));
            if (g_replayFrameLen == 0) {
                return;
            }
        }
        if (g_replaysSent > kBenchFramesPerPhase) {
            g_benchPhase = BenchPhase::Done;
            report::info("19: bench phases done");
            return;
        }
        // Send 0 is the original; 1..20 are the byte-exact repeats.
        if (g_node.benchTransmit(g_replayFrame, g_replayFrameLen) == 0) {
            ++g_replaysSent;
        }
        return;
    }
}
#endif // MAXL_BENCH

void printReport()
{
    const app::Peer *peer = g_node.peers().find(bench::peerId());
    /*
     * Cumulative, not the queue's occupancy.
     *
     * The queue holds 24 and evicts the oldest DELIVERED entry when it is full
     * (gate 3.2), so countInState(Delivered) saturates in the low twenties.
     * docs/test-plan.md asks gate 2.1 for 100 frames -- read off the queue,
     * that gate could never pass, and would have reported a failure on a run in
     * which all 100 were acknowledged.
     */
    const uint32_t delivered = g_node.deliveredTotal();
    const uint32_t undelivered = g_node.undeliveredTotal();

    report::begin(19);
    report::value("node.id", "0x%04X", static_cast<unsigned>(bench::nodeId()));
    report::value("node.peer", "0x%04X", static_cast<unsigned>(bench::peerId()));
    report::value("key.shared", "%d", g_keyShared ? 1 : 0);
    report::value("config.applied", "%d", g_configApplied ? 1 : 0);
    report::value("sniff_interval_ms", "%u", static_cast<unsigned>(kSniffIntervalMs));

    // A bus that had to be freed at boot is a fact about the hardware, not an
    // aside: before hal::recoverI2cBus existed this image did not boot at all.
    {
        const hal::I2cRecovery i2c = i2cRecovery();
        report::value("i2c.was_stuck", "%d", i2c.wasStuck ? 1 : 0);
        if (i2c.wasStuck) {
            report::value("i2c.recovered", "%d", i2c.recovered ? 1 : 0);
            report::value("i2c.pulses", "%u", static_cast<unsigned>(i2c.pulses));
        }
    }
    report::value("store.mounted", "%d", g_storeOk ? 1 : 0);
    report::value("node.ready", "%d", g_nodeOk ? 1 : 0);
    report::value("radio.ready", "%d", g_radioOk ? 1 : 0);
    report::value("tx.allowed", "%d", g_node.transmitAllowed() ? 1 : 0);
    report::value("elapsed_s", "%lu",
                  static_cast<unsigned long>((millis() - g_startedMs) / 1000u));

    // --- the sending side ---------------------------------------------------
    if (bench::nodeId() == 1) {
        const uint32_t elapsed = millis() - g_startedMs;
        report::value("tx.starts_in_s", "%lu",
                      static_cast<unsigned long>(elapsed >= kStartDelayMs
                                                     ? 0u
                                                     : (kStartDelayMs - elapsed) / 1000u));
    }
    report::value("tx.target", "%lu", static_cast<unsigned long>(kMessages));
    report::value("tx.accepted", "%lu", static_cast<unsigned long>(g_sent));
    report::value("tx.submit_failures", "%lu", static_cast<unsigned long>(g_submitFailures));
    report::value("tx.last_submit_error", "0x%02X", g_lastSubmitError);
    report::value("tx.requested", "%lu", static_cast<unsigned long>(g_node.txRequested()));
    report::value("tx.refused_by_radio", "%lu", static_cast<unsigned long>(g_node.txRefused()));
    report::value("tx.build_failures", "%lu",
                  static_cast<unsigned long>(g_node.buildFailures()));
    report::value("tx.delivered", "%lu", static_cast<unsigned long>(delivered));
    report::value("tx.undelivered", "%lu", static_cast<unsigned long>(undelivered));
    // The working set beside the history, so a full queue is still visible.
    report::value("queue.delivered", "%u",
                  static_cast<unsigned>(g_node.queue().countInState(app::MessageState::Delivered)));
    report::value("queue.undelivered", "%u",
                  static_cast<unsigned>(
                      g_node.queue().countInState(app::MessageState::Undelivered)));

    // The budget, so a two-node run can be read against CLAUDE.md 2.3's table
    // without flashing sketch 17.
    {
        uint8_t body[13];
        if (g_node.budget(body, sizeof(body)) == sizeof(body)) {
            const uint32_t usedMs = static_cast<uint32_t>(body[1]) |
                                    (static_cast<uint32_t>(body[2]) << 8) |
                                    (static_cast<uint32_t>(body[3]) << 16) |
                                    (static_cast<uint32_t>(body[4]) << 24);
            const uint32_t limitMs = static_cast<uint32_t>(body[5]) |
                                     (static_cast<uint32_t>(body[6]) << 8) |
                                     (static_cast<uint32_t>(body[7]) << 16) |
                                     (static_cast<uint32_t>(body[8]) << 24);
            report::value("budget.used_ms", "%lu", static_cast<unsigned long>(usedMs));
            report::value("budget.limit_ms", "%lu", static_cast<unsigned long>(limitMs));
        }
    }

    // --- the receiving side -------------------------------------------------
    report::value("rx.frames", "%lu",
                  static_cast<unsigned long>(peer != nullptr ? peer->framesReceived : 0));
    report::value("rx.mic_failures", "%lu", static_cast<unsigned long>(g_node.micFailures()));
    report::value("rx.replays_rejected", "%lu",
                  static_cast<unsigned long>(g_node.replaysRejected()));
    report::value("rx.duplicates_reacked", "%lu",
                  static_cast<unsigned long>(g_node.duplicatesReacked()));
    report::value("journal.entries", "%u", static_cast<unsigned>(g_node.journal().size()));
    report::value("node.current_sf", "%u", static_cast<unsigned>(g_node.currentSf()));
    /*
     * The raw GetPacketStatus word of the last received frame:
     * RssiPkt<<16 | SnrPkt<<8 | SignalRssiPkt. This is the measurement that
     * decides whether the RadioLib byte-order finding (radio_sx1262.cpp) is
     * right on air: RSSI in bits 23:16 plausible, bits 7:0 near zero.
     */
    report::value("radio.packet_status_raw", "0x%06lX",
                  static_cast<unsigned long>(g_radio.lastPacketStatusRaw()));
    /*
     * Whether the receiver is actually duty cycling, and what ended its windows.
     *
     * radio.duty_cycle_active = 0 means the chip is in continuous receive: it
     * hears everything and spends the whole power budget doing it, and RadioLib
     * reports success either way (CLAUDE.md 2.3, radio_sx1262.cpp). Without this
     * line the first air test of the fixed duty cycle could not tell the two
     * apart -- which is exactly what happened on 2026-09-01.
     *
     * The two counters are why a receive window ended with no frame. They are
     * ordinary radio weather and are not errors; they are here because a window
     * that ends without re-arming leaves the node permanently deaf, and "quiet"
     * and "dead" are indistinguishable from the link layer.
     */
    report::value("radio.duty_cycle_active", "%d", g_radio.rxDutyCycleActive() ? 1 : 0);
    report::value("radio.rx_crc_failures", "%lu",
                  static_cast<unsigned long>(g_radio.rxCrcFailures()));
    report::value("radio.rx_empty_irqs", "%lu",
                  static_cast<unsigned long>(g_radio.rxEmptyIrqs()));

    // --- what the ACK carried, which is the substance of gate 2.4 -----------
    report::value("ack.seen", "%lu", static_cast<unsigned long>(g_node.ackSeenTotal()));
    // What THIS node put on the air as ACKs. tx.requested counts only the ARQ
    // data path, so without this the receiving node reports zero transmissions
    // however much work it is doing.
    report::value("ack.sent", "%lu", static_cast<unsigned long>(g_node.ackTxTotal()));
    // Did the continuous window mechanism run at all, and how long is it? Two
    // bench cycles were spent on 2026-09-01 not knowing.
    report::value("ack.window_ms", "%lu",
                  static_cast<unsigned long>(link::ackWindowMs(g_node.currentSf())));
    report::value("ack.window_opens", "%lu",
                  static_cast<unsigned long>(g_node.ackWindowOpens()));
    report::value("ack.window_open_now", "%d", g_node.inAckWindow() ? 1 : 0);
    report::value("ack.last_rssi", "%d", static_cast<int>(g_node.lastAckRssi()));
    report::value("ack.last_snr", "%d", static_cast<int>(g_node.lastAckSnr()));
    if (peer != nullptr) {
        report::value("peer.last_rssi", "%d", static_cast<int>(peer->lastRssi));
        report::value("peer.last_snr", "%d", static_cast<int>(peer->lastSnr));
        report::value("peer.last_sf", "%u", static_cast<unsigned>(peer->lastSf));
    } else {
        report::info("19: no peer heard yet");
    }

    /*
     * Gate 2.1 on the sender is delivered == target with no undelivered: a
     * delivery is an ACK that verified, so it is evidence about the receiver's
     * MIC as much as about this side's. On the receiver it is rx.frames against
     * what the other board says it sent, with rx.mic_failures at zero -- and
     * that comparison needs both reports, which is why both sides print the
     * same keys.
     */
#ifdef MAXL_BENCH
    if (bench::nodeId() == 1) {
        static const char *kPhaseNames[] = {"idle", "tamper_payload", "tamper_header", "replay",
                                            "done"};
        report::value("bench.phase", "%s",
                      kPhaseNames[static_cast<size_t>(g_benchPhase)]);
        report::value("bench.tamper_payload_sent", "%lu",
                      static_cast<unsigned long>(g_tamperPayloadSent));
        report::value("bench.tamper_header_sent", "%lu",
                      static_cast<unsigned long>(g_tamperHeaderSent));
        report::value("bench.replays_sent", "%lu", static_cast<unsigned long>(g_replaysSent));
    }
#endif

    if (bench::nodeId() == 1) {
        const bool gate21 =
            g_sent == kMessages && delivered == kMessages && undelivered == 0;
        report::value("gate_2_1.pass", "%d", gate21 ? 1 : 0);
        /*
         * Gate 2.4 is "every ACK_REQ frame acknowledged; RSSI/SNR in the ACK
         * match the receiver's log" (docs/test-plan.md). So it asks three
         * things, and each must be able to fail on its own:
         *
         *   gate21                  every message was acknowledged at all
         *   ackSeenTotal >= target  an ACK verified for each of them -- and not
         *                           read off the peer table, where a plain
         *                           received frame would also set the metrics
         *   lastAckRssi < 0         the value the ACK carried is a measurement
         *
         * The last clause is deliberately a plausibility test rather than a
         * "was it set" flag. RadioLib 7.7.1's getRSSI() returns SignalRssiPkt
         * instead of RssiPkt (radio_sx1262.cpp), which reads as 0 dBm on a
         * bench link -- exactly what node B reported on 2026-08-31 beside an
         * SNR of +11 dB. "peer.last_rssi plausibly negative" is what the next
         * two-node run has to confirm, and a flag
         * that merely says "something was written here" would pass on the
         * broken driver and quietly retire the only automatic check for it.
         *
         * The two values are reported below so the "match the receiver's log"
         * half can be done by eye against the other board's report.
         */
        const bool gate24 = gate21 && g_node.ackSeenTotal() >= kMessages
                            && g_node.lastAckRssi() < 0;
        report::value("gate_2_4.pass", "%d", gate24 ? 1 : 0);
    }

    if (!g_keyShared) {
        report::verdict("fail",
                        "no MAXL_DEV_KEY compiled in -- this image derived a key from its own "
                        "DEVICEID and no second board can match it");
    } else if (bench::nodeId() == 1 && g_sent >= kMessages
               && g_node.queue().countInState(app::MessageState::InFlight) == 0) {
        const bool ok = delivered == kMessages && undelivered == 0;
        report::verdict(ok ? "pass" : "fail",
                        "every message delivered and acknowledged, or not");
    } else {
        report::verdict("inconclusive", "still running; the receiver's report is the other half");
    }
    report::end(19);
}

} // namespace

void setup()
{
    // Longer than a twenty message run at 500 ms sniff, and poked every report.
    commonSetup(900000);
    traceStep("wire.begin");
    Wire.begin();
    traceStep("wire.ok");
    g_clock.begin();
    traceStep("clock.ok");
    g_keys.begin();
    traceStep("keys.ok");

    traceStep("flash.open");
    if (hal::openExternalFlash(g_transport, g_flash)) {
        traceStep("flash.opened");
        g_storeOk = g_store.begin(g_flash);
    }
    traceStep("store.ok");
    if (g_storeOk) {
        g_store.configureRegion(hal::StoreRegion::FrameCounter, 8, 64);
        g_store.configureRegion(hal::StoreRegion::BudgetRing, 10, 512);
        g_store.configureRegion(hal::StoreRegion::MessageQueue, app::kBlobBytes, 1);
        g_store.configureRegion(hal::StoreRegion::EventJournal, app::kJournalRecordBytes,
                                app::kJournalCapacity);
        /*
         * Start from an empty outbox.
         *
         * A queue left over from an earlier run is not neutral here: entries
         * that gave up stay Undelivered for ever and nothing reclaims them
         * (decision D18), so twenty-four of them mean this sketch cannot submit
         * a single message and reports a link failure that is really a storage
         * one. Node A arrived at exactly that state on 2026-08-31.
         *
         * Erasing is right for a bench image and would be wrong for the
         * application, which is why it is here and not in app::Node::begin.
         */
        g_store.erase(hal::StoreRegion::MessageQueue);
        /*
         * And the journal, for a reason that cost node A every event it ever
         * recorded.
         *
         * sketch_10_hal.cpp exercises this region by erasing it and leaving one
         * record of the pattern 0x40,0x41,0x42,... behind; it never cleans up.
         * Journal::restore() reads the counter out of bytes 0..3, so that
         * leftover restores as counter 0x43424140 -- 1.13 billion. Journal
         *::append then refuses every real event, silently and without raising
         * count_ (journal.cpp: "Counters are monotonic by construction"), for
         * the rest of the device's life. Node A ran 20.8 h on 2026-08-31 and
         * reported journal.entries = 1 with all 21 of its events swallowed.
         *
         * Erasing here is what makes journal.entries readable as evidence. Like
         * the queue erase above it belongs to a bench image, not to app::Node.
         */
        g_store.erase(hal::StoreRegion::EventJournal);
        traceStep("erase.ok");
        g_nodeOk = g_node.begin(bench::nodeId());
        traceStep("node.begin.ok");
    }

    uint8_t key[16];
    g_keyShared = bench::networkKey(key);
    g_node.provisionKey(0, bench::kNetId, key);
    traceStep("key.ok");

    /*
     * The sniff interval, through the real config path rather than a private
     * field. It has to be set BEFORE attachRadio, which is where the preamble is
     * derived from it and the receive duty cycle starts.
     */
    // Sniff interval, and the SF pinned fixed: adaptive SF (wired 2026-08-31)
    // moving mid-run would break the "both sides agree" property the preamble
    // depends on until gates 2.11-2.14 test the adaptation deliberately.
    const uint8_t tlv[] = {0x02, 0x02, static_cast<uint8_t>(kSniffIntervalMs & 0xFF),
                           static_cast<uint8_t>(kSniffIntervalMs >> 8),
                           0x04, 0x01, 1,
                           0x05, 0x01, kSf};
    uint32_t appliedMask = 0;
    uint8_t unapplied[16];
    size_t unappliedCount = 0;
    g_configApplied = g_node.setConfig(1, tlv, sizeof(tlv), &appliedMask, unapplied,
                                       &unappliedCount) == 0
                      && appliedMask != 0;

    traceStep("config.ok");
    const hal::Modulation rendezvous{kSf, 869575000u, 22};
    traceStep("radio.begin");
    g_radioOk = g_radio.begin(rendezvous);
    traceStep(g_radioOk ? "radio.begin.ok" : "radio.begin.FAILED");
    if (g_radioOk) {
        g_node.attachRadio(g_radio, g_jitter);
        traceStep("attachRadio.ok");
    }

    g_startedMs = millis();
    traceStep("setup.done");
}

void loop()
{
    const uint32_t now = millis();

    /*
     * No wait for USB. The board without a cable is doing the same work as the
     * one with it, and on a machine with one working socket that is the normal
     * case rather than the exception.
     */
    feed();
#ifdef MAXL_BENCH
    benchStep();
#endif
    g_node.tick(now);
    g_node.pumpRadio(now);
    g_radio.service();

    if (usbReady() && static_cast<uint32_t>(now - g_lastReportMs) >= kReportEveryMs) {
        g_lastReportMs = now;
        printReport();
    }

    delay(2);
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 19
