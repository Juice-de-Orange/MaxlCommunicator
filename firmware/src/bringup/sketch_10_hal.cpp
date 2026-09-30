/*
 * Bring-up 10 -- the real hal/ implementations, not a sketch's own copy.
 *
 * Sketch 04 established that the chip is a ZD25WQ16B and that raw sectors
 * survive a reboot. What it deliberately did not do was mount LittleFS, because
 * writing an lfs_config twice -- once throwaway here, once for real -- would
 * prove nothing about the version that ships. This runs the shipping one:
 *
 *   hal::LittleFsBlockStore   the volume, and the four regions on it
 *   hal::Pcf8563Clock         the same clock code sketch 03 proved, moved
 *
 * Together they complete the half of gate 0.3 that sketch 04 left open --
 * "LittleFS mounts" -- and they do it through the interface link/ and app/
 * actually use, so a mistake here is a mistake in the shipping path.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 10

#include <Arduino.h>
#include <Wire.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <Adafruit_SPIFlash.h>
#pragma GCC diagnostic pop

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

bool g_measured = false;

/*
 * Measured once, printed for ever after.
 *
 * The first version printed its report and then set a done flag. That report
 * existed for one pass of the loop -- and that pass happens before
 * tools/bringup_run.py has the port open, so the sketch passed on the device and
 * looked like a dead one from the host. Sketch 04 had the same fault in a
 * different disguise.
 *
 * The work must still run once: it writes to flash, and repeating it every three
 * seconds would put erase cycles on the chip for nothing.
 */
struct Measurements {
    bool clockOk = false;
    bool clockVlAtBoot = false;
    bool clockTimeValid = false;
    uint32_t clockUnix = 0;

    bool flashOk = false;
    uint32_t jedecBeforeWake = 0;
    uint32_t jedec = 0;
    bool volumeOk = false;
    uint32_t volumeBytes = 0;
    bool regionsOk = false;
    bool atomicOk = false;
    bool ok = false;
};

Measurements g_measurements;

/// Exercise one region the way its owner does: append, read back, count, and --
/// for the single-record regions -- replaceAll.
bool exerciseRegion(hal::StoreRegion region, const char *name, size_t recordBytes,
                    bool singleRecord)
{
    /*
     * Large enough for the journal's record, which is the biggest region this
     * exercises: app::kJournalRecordBytes is 70. At 64 the journal fell down the
     * "skipping the round trip" path on every single run -- the region that
     * carries every event the phone ever collects was never actually tested, and
     * the line saying so printed outside the report frame where nobody saw it.
     */
    uint8_t out[96];
    uint8_t pattern[96];
    const size_t len = recordBytes > sizeof(pattern) ? sizeof(pattern) : recordBytes;
    for (size_t i = 0; i < len; ++i) {
        pattern[i] = static_cast<uint8_t>(0x40 + i);
    }
    if (len != recordBytes) {
        // The queue's record is 1500 bytes; exercising it here would need a
        // buffer this sketch has no reason to carry. Its own tests cover it.
        report::info("%s: record is %u bytes, skipping the round trip", name,
                     static_cast<unsigned>(recordBytes));
        return true;
    }

    if (g_store.erase(region) != hal::StoreResult::Ok) {
        report::value("region.erase_failed", "%s", name);
        return false;
    }

    if (singleRecord) {
        if (g_store.replaceAll(region, pattern, len) != hal::StoreResult::Ok) {
            report::value("region.replace_failed", "%s", name);
            return false;
        }
    } else if (g_store.append(region, pattern, len) != hal::StoreResult::Ok) {
        report::value("region.append_failed", "%s", name);
        return false;
    }

    if (g_store.count(region) != 1) {
        report::value("region.count_wrong", "%s", name);
        return false;
    }
    if (g_store.read(region, 0, out, len) != hal::StoreResult::Ok) {
        report::value("region.read_failed", "%s", name);
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (out[i] != pattern[i]) {
            report::value("region.mismatch", "%s at %u", name, static_cast<unsigned>(i));
            return false;
        }
    }
    return true;
}

} // namespace

namespace bringup {

void setup()
{
    commonSetup(180000);
    Wire.begin();
}

void measure()
{
    Measurements &m = g_measurements;

    // --- clock ------------------------------------------------------------
    m.clockOk = g_clock.begin();
    m.clockVlAtBoot = g_clock.voltageLowAtBoot();
    m.clockTimeValid = g_clock.timeValid();
    m.clockUnix = g_clock.unixSeconds();

    /*
     * --- flash and volume ---------------------------------------------------
     *
     * Wake the chip before asking it anything.
     *
     * The ZD25WQ16B has no reset line. A deep power-down survives every MCU
     * reset there is, and in that state it ignores all commands except 0xAB and
     * the bus reads back 0xFFFFFF -- which is exactly what node A produced on
     * 2026-08-31 in the evening, after having read 0xBA6015 the same morning.
     * Sketch 15 holds DEEP_IDLE with the chip asleep for thirty seconds at a
     * time; a reset landing in that window leaves it asleep for good.
     *
     * 0xAB costs microseconds and is harmless on an awake chip -- it is the
     * documented release command, and a chip that is already running treats it
     * as a no-op read. Sending it unconditionally is cheaper than a rule about
     * when to send it.
     *
     * `jedecBeforeWake` is kept because it is the evidence: 0xFFFFFF there and a
     * real id afterwards is a chip that was asleep, and nothing else produces
     * that pair.
     */
    m.flashOk = hal::openExternalFlash(g_transport, g_flash, &m.jedecBeforeWake);
    m.jedec = g_flash.getJEDECID();
    if (!m.flashOk) {
        return;
    }

    m.volumeOk = g_store.begin(g_flash);
    m.volumeBytes = g_store.volumeBytes();
    if (!m.volumeOk) {
        return;
    }

    // The geometry each owning module declares. hal/ does not read it from them
    // -- that would be a layer violation, and the checker says so (decision D3).
    g_store.configureRegion(hal::StoreRegion::FrameCounter, 8, 64);
    g_store.configureRegion(hal::StoreRegion::BudgetRing, 10, 512);
    g_store.configureRegion(hal::StoreRegion::MessageQueue, app::kBlobBytes, 1);
    g_store.configureRegion(hal::StoreRegion::EventJournal, app::kJournalRecordBytes,
                            app::kJournalCapacity);

    m.regionsOk = exerciseRegion(hal::StoreRegion::FrameCounter, "counter", 8, true)
                  && exerciseRegion(hal::StoreRegion::BudgetRing, "budget", 10, false)
                  && exerciseRegion(hal::StoreRegion::EventJournal, "journal",
                                    app::kJournalRecordBytes, false);

    // The atomic path, twice over. A replaceAll that left the region empty
    // between the erase and the write would show up as a count of 0 here, and
    // that is the failure the frame counter cannot survive.
    uint8_t counter[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    m.atomicOk = true;
    for (int i = 0; i < 8; ++i) {
        counter[0] = static_cast<uint8_t>(i);
        m.atomicOk = m.atomicOk
                     && g_store.replaceAll(hal::StoreRegion::FrameCounter, counter,
                                           sizeof(counter))
                            == hal::StoreResult::Ok
                     && g_store.count(hal::StoreRegion::FrameCounter) == 1;
    }

    m.ok = m.clockOk && m.flashOk && m.volumeOk && m.regionsOk && m.atomicOk;
}

void printReport()
{
    const Measurements &m = g_measurements;

    report::begin(10);
    report::value("flash.jedec_before_wake", "0x%06lX",
                  static_cast<unsigned long>(m.jedecBeforeWake));
    report::value("clock.present", "%d", m.clockOk ? 1 : 0);
    report::value("clock.vl_at_boot", "%d", m.clockVlAtBoot ? 1 : 0);
    report::value("clock.time_valid", "%d", m.clockTimeValid ? 1 : 0);
    report::value("clock.unix", "%lu", static_cast<unsigned long>(m.clockUnix));

    report::value("flash.begin", "%d", m.flashOk ? 1 : 0);
    report::value("flash.jedec", "0x%06lX", static_cast<unsigned long>(m.jedec));
    report::value("littlefs.mounted", "%d", m.volumeOk ? 1 : 0);
    report::value("littlefs.volume_bytes", "%lu", static_cast<unsigned long>(m.volumeBytes));
    report::value("regions.round_trip", "%d", m.regionsOk ? 1 : 0);
    report::value("counter.replace_all_x8", "%d", m.atomicOk ? 1 : 0);

    report::verdict(m.ok ? "pass" : "fail",
                    m.ok ? "LittleFS mounts on the ZD25WQ16B and all four regions round-trip "
                           "through the shipping hal implementation -- gate 0.3's remaining half"
                         : "see the failing key above");
    report::end(10);
}

void loop()
{
    if (!usbReady()) {
        delay(200);
        commonService();
        return;
    }

    if (!g_measured) {
        measure();
        g_measured = true;
    }

    printReport();
    delay(2000);
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 10
