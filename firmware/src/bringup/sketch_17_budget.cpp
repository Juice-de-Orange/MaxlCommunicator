/*
 * Bring-up 17 -- the duty cycle budget, with the radio actually transmitting.
 *
 * docs/test-plan.md:
 *
 *   2.7  Budget saturation -- queue 30 messages back to back
 *        -> measured airtime never exceeds 360 s in any rolling hour
 *   2.8  Budget survives reboot -- saturate, reboot, immediately attempt TX
 *        -> refused; release time correct against the pre-reboot window
 *
 * And its note: "2.7 and 2.8 are the tests that make section 1.2 true rather
 * than aspirational."
 *
 * WHY THIS TAKES AN HOUR AND CANNOT BE SHORTENED
 *
 * CLAUDE.md 1.3 gives g3 360 s of airtime per rolling hour at 10 %. Spending
 * 360 s of airtime therefore takes 3600 s of wall clock, by arithmetic rather
 * than by choice. A run that finished in ten minutes would have proved that the
 * budget was not being enforced.
 *
 * WHAT IT TRANSMITS, AND AT WHOM
 *
 * Nobody. There is one node. The frames go out on g3 869.575 MHz into an
 * attached antenna and nothing receives them -- which the budget does not care
 * about, and which is the whole reason this gate is reachable without a second
 * device.
 *
 * Legal on the terms CLAUDE.md 1.3 sets: the band is g3, the power is capped
 * per band in link/band, and the duty cycle is enforced by the tracker this
 * sketch exists to test. NEVER RUN IT WITHOUT AN ANTENNA.
 *
 * THE KEY IS DERIVED, NOT WRITTEN DOWN
 *
 * The node refuses to transmit without one. CLAUDE.md 6: no secrets in the
 * repository, keys are provisioned at runtime. So it comes from the chip's
 * factory DEVICEID -- unique to this board, reproducible on it, and nowhere in
 * git.
 *
 * 2.8's note asks for the reboot to be a battery pull rather than a clean one.
 * The cell is soldered in, so that half belongs to bring-up 13 and to somebody
 * holding the button. This does the soft-reset half, which decision D2 proposes
 * splitting out as 2.8a.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 17

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

/// A jitter source that is not random. CLAUDE.md 2.4 wants +-25 % jitter on the
/// retry backoff; for a measurement the middle of the range is what makes two
/// runs comparable.
class MiddleJitter : public link::IJitterSource {
public:
    uint32_t next(uint32_t bound) override { return bound == 0 ? 0 : (bound - 1) / 2; }
};

MiddleJitter g_jitter;

/*
 * Gate 2.10 rides along: every transmission this run makes is one sample of
 * computed-versus-measured airtime, and an hour of budget saturation is the
 * best statistics this bench will ever produce for free.
 *
 * The tap sits between the node and the driver because neither side exposes
 * what the other needs: the node never says how long the frame it handed over
 * was, and hal::IRadioLink deliberately does not carry lastAirtimeUs() (D3 --
 * gate 2.10 is exactly the comparison of the two figures, so they are kept on
 * separate sides of the interface). The tap remembers the length on the way
 * down and reads the measurement on the way back up.
 */
class AirtimeTapRadio : public hal::IRadioLink {
public:
    explicit AirtimeTapRadio(hal::Sx1262Radio &inner) : inner_(inner) {}

    void setObserver(hal::IRadioObserver *observer) override { inner_.setObserver(observer); }
    hal::RadioResult setModulation(const hal::Modulation &m) override
    {
        sf_ = m.spreadingFactor;
        return inner_.setModulation(m);
    }
    hal::RadioResult setPreambleLength(uint16_t symbols) override
    {
        preamble_ = symbols;
        return inner_.setPreambleLength(symbols);
    }
    hal::RadioResult transmit(const uint8_t *data, size_t len) override
    {
        const hal::RadioResult result = inner_.transmit(data, len);
        if (result == hal::RadioResult::Ok) {
            pendingLen_ = len;
        }
        return result;
    }
    hal::RadioResult startReceiveDutyCycle(uint8_t minSym) override
    {
        return inner_.startReceiveDutyCycle(minSym);
    }
    bool rxDutyCycleActive() const override { return inner_.rxDutyCycleActive(); }
    hal::RadioResult startReceiveContinuous() override { return inner_.startReceiveContinuous(); }
    hal::RadioResult sleep() override { return inner_.sleep(); }

    /// Call after service(): when a transmission finished, record one sample.
    void sample()
    {
        if (pendingLen_ == 0 || inner_.lastAirtimeUs() == lastSeenUs_) {
            return;
        }
        lastSeenUs_ = inner_.lastAirtimeUs();
        computedUs_ = link::timeOnAirUs(sf_, static_cast<uint8_t>(pendingLen_), preamble_);
        measuredUs_ = lastSeenUs_;
        pendingLen_ = 0;
        ++samples_;

        // Deviation in permille of the computed figure, kept as the worst seen.
        const uint32_t diff = computedUs_ > measuredUs_ ? computedUs_ - measuredUs_
                                                        : measuredUs_ - computedUs_;
        const uint32_t permille = computedUs_ ? (diff * 1000u) / computedUs_ : 1000u;
        if (permille > worstPermille_) {
            worstPermille_ = permille;
        }
    }

    uint32_t samples() const { return samples_; }
    uint32_t computedUs() const { return computedUs_; }
    uint32_t measuredUs() const { return measuredUs_; }
    uint32_t worstPermille() const { return worstPermille_; }

private:
    hal::Sx1262Radio &inner_;
    uint8_t sf_ = 9;
    uint16_t preamble_ = 8;
    size_t pendingLen_ = 0;
    uint32_t lastSeenUs_ = 0;
    uint32_t computedUs_ = 0;
    uint32_t measuredUs_ = 0;
    uint32_t samples_ = 0;
    uint32_t worstPermille_ = 0;
};

AirtimeTapRadio g_tap{g_radio};

/// One hour. Not a parameter: see the header.
constexpr uint32_t kSaturateMs = 3600u * 1000u;

/// Gate 2.7's number.
constexpr size_t kMessagesToQueue = 30;

/// Reported often enough to watch, rarely enough not to drown the log.
constexpr uint32_t kReportIntervalMs = 30000;

constexpr uint8_t kEvtBudgetBytes = 13;

bool g_storeOk = false;
/// True when the key came from MAXL_DEV_KEY, i.e. when another board can match it.
bool g_keyShared = false;
bool g_nodeOk = false;
bool g_radioOk = false;
bool g_afterReset = false;

uint32_t g_startedMs = 0;
uint32_t g_lastReportMs = 0;
uint32_t g_queued = 0;
uint32_t g_accepted = 0;
uint32_t g_refusedBudget = 0;

/*
 * Why the last submission was turned away, and how full the queue was.
 *
 * Added 2026-08-31, after a thirty minute run reported
 * `queue.submitted = 100000, queue.accepted = 0, queue.refused_budget = 0` and
 * could not say a word about the reason. `refused_budget` counted only 0x06, and
 * Node::sendText never returns 0x06 -- the budget's refusal is per frame and
 * happens further down. So the one number that was instrumented was the one
 * number that could not move.
 *
 * A test that runs for an hour and cannot report why it failed has spent the
 * hour on nothing.
 */
uint8_t g_lastSendError = 0;
uint32_t g_sendErrorCounts[8] = {0};

/// The highest airtime the tracker ever admitted to in the rolling hour. Gate
/// 2.7's criterion is that this never passes the limit.
uint32_t g_peakUsedMs = 0;
uint32_t g_limitMs = 0;

/// Read at the very first opportunity after a reset, before anything runs.
uint32_t g_bootUsedMs = 0;
uint32_t g_bootNextTxUnix = 0;
uint32_t g_bootUnix = 0;
uint8_t g_bootSendResult = 0xFF;

uint32_t readU32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/// band:u8, usedMs:u32, limitMs:u32, nextTxUnix:u32
bool readBudget(uint32_t *usedMs, uint32_t *limitMs, uint32_t *nextTxUnix)
{
    uint8_t body[kEvtBudgetBytes];
    if (g_node.budget(body, sizeof(body)) != kEvtBudgetBytes) {
        return false;
    }
    *usedMs = readU32(body + 1);
    *limitMs = readU32(body + 5);
    *nextTxUnix = readU32(body + 9);
    return true;
}

/// The network key, from this chip and nowhere else.

void printReport(const char *phase)
{
    uint32_t used = 0, limit = 0, nextTx = 0;
    const bool haveBudget = readBudget(&used, &limit, &nextTx);
    if (haveBudget && used > g_peakUsedMs) {
        g_peakUsedMs = used;
        g_limitMs = limit;
    }

    report::begin(17);
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
    report::value("tx.allowed", "%d", g_node.transmitAllowed() ? 1 : 0);
    report::value("elapsed_s", "%lu",
                  static_cast<unsigned long>((millis() - g_startedMs) / 1000u));

    report::value("queue.submitted", "%lu", static_cast<unsigned long>(g_queued));
    report::value("queue.accepted", "%lu", static_cast<unsigned long>(g_accepted));
    report::value("queue.refused_budget", "%lu", static_cast<unsigned long>(g_refusedBudget));

    /*
     * The error codes are docs/bridge-protocol.md section 4:
     *   0x02 BAD_LENGTH  0x03 BAD_PARAM   0x05 NO_KEY
     *   0x07 QUEUE_FULL  0x08 NO_TIME     0x0A STORAGE
     * Node::sendText maps Accept::Full to QUEUE_FULL and both TextTooLong and a
     * failed flash write to BAD_PARAM and STORAGE respectively.
     */
    report::value("queue.last_error", "0x%02X", g_lastSendError);
    report::value("queue.err_queue_full", "%lu",
                  static_cast<unsigned long>(g_sendErrorCounts[7]));
    report::value("queue.err_bad_param", "%lu",
                  static_cast<unsigned long>(g_sendErrorCounts[3]));
    report::value("queue.err_no_key", "%lu",
                  static_cast<unsigned long>(g_sendErrorCounts[5]));
    report::value("queue.depth", "%u", static_cast<unsigned>(g_node.queue().size()));

    /*
     * Where the send path stops. Exactly one of these should be moving; which
     * one it is names the fault without another hour of guessing.
     */
    report::value("tx.promote_seen", "%lu", static_cast<unsigned long>(g_node.promoteSeen()));
    report::value("tx.build_failures", "%lu",
                  static_cast<unsigned long>(g_node.buildFailures()));
    report::value("tx.arq_rejects", "%lu", static_cast<unsigned long>(g_node.arqRejects()));
    report::value("tx.requested", "%lu", static_cast<unsigned long>(g_node.txRequested()));
    report::value("tx.refused_by_radio", "%lu",
                  static_cast<unsigned long>(g_node.txRefused()));
    report::value("queue.capacity", "%u", static_cast<unsigned>(app::kQueueCapacity));

    /*
     * The queue by state. Depth alone cannot tell a queue that is working hard
     * from one that is wedged, and on 2026-08-31 the difference was the whole
     * question: 24 entries, nothing promoted, nothing transmitted.
     */
    report::value("queue.pending", "%u",
                  static_cast<unsigned>(g_node.queue().countInState(app::MessageState::Pending)));
    report::value("queue.queued", "%u",
                  static_cast<unsigned>(g_node.queue().countInState(app::MessageState::Queued)));
    report::value("queue.in_flight", "%u",
                  static_cast<unsigned>(g_node.queue().countInState(app::MessageState::InFlight)));
    report::value("queue.delivered", "%u",
                  static_cast<unsigned>(g_node.queue().countInState(app::MessageState::Delivered)));
    report::value("queue.undelivered", "%u",
                  static_cast<unsigned>(
                      g_node.queue().countInState(app::MessageState::Undelivered)));

    report::value("budget.used_ms", "%lu", static_cast<unsigned long>(used));
    report::value("budget.limit_ms", "%lu", static_cast<unsigned long>(limit));
    report::value("budget.peak_used_ms", "%lu", static_cast<unsigned long>(g_peakUsedMs));
    report::value("budget.next_tx_unix", "%lu", static_cast<unsigned long>(nextTx));
    /*
     * The node's own clock, beside the release time it just quoted.
     *
     * Without it the two numbers cannot be compared: a nextTxUnix means nothing
     * except as a distance from now, and reading "now" off the host assumes a
     * synchronisation this report has no business assuming.
     */
    report::value("clock.unix", "%lu", static_cast<unsigned long>(g_clock.unixSeconds()));
    report::value("budget.wait_s", "%ld",
                  static_cast<long>(static_cast<int32_t>(nextTx - g_clock.unixSeconds())));

    // Gate 2.7: the airtime in the rolling hour never passes the limit.
    const bool gate27 = haveBudget && g_limitMs > 0 && g_peakUsedMs <= g_limitMs;
    report::value("gate_2_7.pass", "%d", gate27 ? 1 : 0);

    /*
     * Gate 2.10, sampled on every transmission of this run: the computed
     * airtime (what the budget is charged with) against the driver's measured
     * one. 5 % is the gate's allowance; the measured figure deliberately
     * includes the ramp and the TCXO start, so if SF9 at a long preamble sits
     * inside 50 permille here, the accounting is honest where it matters.
     */
    report::value("airtime.samples", "%lu", static_cast<unsigned long>(g_tap.samples()));
    report::value("airtime.computed_us", "%lu", static_cast<unsigned long>(g_tap.computedUs()));
    report::value("airtime.measured_us", "%lu", static_cast<unsigned long>(g_tap.measuredUs()));
    report::value("airtime.worst_permille", "%lu",
                  static_cast<unsigned long>(g_tap.worstPermille()));
    const bool gate210 = g_tap.samples() > 0 && g_tap.worstPermille() <= 50;
    report::value("gate_2_10.pass", "%d", gate210 ? 1 : 0);

    if (g_afterReset) {
        // Gate 2.8a: what the budget looked like at the first instruction after
        // the reset, and what a transmission attempted right then was told.
        report::value("g28.boot_used_ms", "%lu", static_cast<unsigned long>(g_bootUsedMs));
        report::value("g28.boot_next_tx_unix", "%lu",
                      static_cast<unsigned long>(g_bootNextTxUnix));
        report::value("g28.boot_unix", "%lu", static_cast<unsigned long>(g_bootUnix));
        report::value("g28.send_result", "0x%02X", g_bootSendResult);

        // 0x06 is ERR_BUDGET_EXHAUSTED (docs/bridge-protocol.md section 4).
        // A budget of zero after the reset is not "nothing to report" -- it is
        // gate 2.8a failing, and it must read as a failure rather than as a
        // sketch that never ran.
        const bool refused = g_bootSendResult == 0x06;
        const bool releaseInFuture = g_bootNextTxUnix > g_bootUnix;
        const bool survived = g_bootUsedMs > 0;
        report::value("gate_2_8a.pass", "%d", (refused && releaseInFuture && survived) ? 1 : 0);
        report::info("17: 2.8b -- a real power loss -- needs the button and a hand; see D2");
    }

    report::verdict("inconclusive",
                    "2.7 is decided by peak_used_ms against limit_ms after the full hour; "
                    "2.8a by the boot values above");
    report::end(17);
}

} // namespace

namespace bringup {

void setup()
{
    // Longer than the run, and poked at every report.
    commonSetup(600000);
    Wire.begin();
    g_clock.begin();
    g_keys.begin();

    if (hal::openExternalFlash(g_transport, g_flash)) {
        g_storeOk = g_store.begin(g_flash);
    }
    if (g_storeOk) {
        g_store.configureRegion(hal::StoreRegion::FrameCounter, 8, 64);
        g_store.configureRegion(hal::StoreRegion::BudgetRing, 10, 512);
        g_store.configureRegion(hal::StoreRegion::MessageQueue, app::kBlobBytes, 1);
        g_store.configureRegion(hal::StoreRegion::EventJournal, app::kJournalRecordBytes,
                                app::kJournalCapacity);
        g_nodeOk = g_node.begin(bench::nodeId());
    }

    /*
     * The budget as it stands at the first opportunity after boot, BEFORE
     * anything transmits. On the far side of the reset this is gate 2.8a's whole
     * evidence: a budget that a power cycle can clear is not a budget.
     */
    uint32_t limit = 0;
    readBudget(&g_bootUsedMs, &limit, &g_bootNextTxUnix);
    g_bootUnix = g_clock.unixSeconds();

    /*
     * Which side of the reset this is, decided by RESETREAS and not by the
     * budget.
     *
     * Taking "the budget is non-zero" as the signal would be circular: a budget
     * that failed to survive is exactly what gate 2.8a is asking about, and the
     * sketch would answer by saturating for another hour instead of reporting
     * the failure. RESETREAS bit 2 is SREQ, set by the hardware on
     * NVIC_SystemReset and needing nobody's cooperation -- bring-up 03
     * established that, and that GPREGRET2 does not survive the bootloader.
     */
    constexpr uint32_t kResetReasonSreq = 0x04;
    g_afterReset = (NRF_POWER->RESETREAS & kResetReasonSreq) != 0;
    NRF_POWER->RESETREAS = 0xFFFFFFFFu;  // cumulative until written back

    uint8_t key[16];
    g_keyShared = bench::networkKey(key);
    g_node.provisionKey(0, bench::kNetId, key);

    if (g_afterReset) {
        // Immediately, as the gate says. Before the radio is even attached, so
        // the refusal comes from the budget and from nothing else.
        g_bootSendResult = g_node.sendText(bench::peerId(), reinterpret_cast<const uint8_t *>("now"), 3);
    }

    const hal::Modulation rendezvous{9, 869575000u, 22};
    g_radioOk = g_radio.begin(rendezvous);
    if (g_radioOk) {
        // Through the tap, so gate 2.10 samples every frame this hour sends.
        g_node.attachRadio(g_tap, g_jitter);
    }
    // The airtime figures this run reports are the SF9 table's; adaptive SF
    // moving mid-run would quietly change them (bench_config.h).
    bench::pinFixedSf9(g_node);

    g_startedMs = millis();
}

void loop()
{
    const uint32_t now = millis();

    if (!usbReady()) {
        delay(50);
        commonService();
        return;
    }

    if (g_afterReset) {
        // The far side: report and stop. Nothing more may be transmitted, or the
        // boot values above stop meaning what they say.
        if (static_cast<uint32_t>(now - g_lastReportMs) >= 3000u) {
            g_lastReportMs = now;
            printReport("after_reset");
        }
        delay(50);
        commonService();
        return;
    }

    // Keep the queue fed. Gate 2.7 says thirty back to back; this keeps thirty
    // outstanding for the whole hour, which is the same demand sustained.
    while (g_queued < kMessagesToQueue ||
           (g_node.queue().size() < kMessagesToQueue && g_queued < 100000)) {
        char text[16];
        snprintf(text, sizeof(text), "g27-%05lu", static_cast<unsigned long>(g_queued));
        const uint8_t result =
            g_node.sendText(bench::peerId(), reinterpret_cast<const uint8_t *>(text), strlen(text));
        ++g_queued;
        if (result == 0) {
            ++g_accepted;
        } else {
            g_lastSendError = result;
            if (result < 8) {
                ++g_sendErrorCounts[result];
            }
            if (result == 0x06) {
                ++g_refusedBudget;
            }
            break;
        }
    }

    g_node.tick(now);
    g_node.pumpRadio(now);
    g_radio.service();
    g_tap.sample();

    if (static_cast<uint32_t>(now - g_lastReportMs) >= kReportIntervalMs) {
        g_lastReportMs = now;
        printReport("saturating");
    }

    if (static_cast<uint32_t>(now - g_startedMs) >= kSaturateMs) {
        printReport("saturated");
        report::info("17: the hour is up -- resetting to test 2.8a");
        Serial.flush();
        delay(50);

        // Detach before resetting; see sketch 04 for what happens otherwise.
        TinyUSBDevice.detach();
        delay(250);
        NVIC_SystemReset();
    }

    delay(5);
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 17
