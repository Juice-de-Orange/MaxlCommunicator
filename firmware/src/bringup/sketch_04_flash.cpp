/*
 * Bring-up 04 -- the external SPI NOR flash.
 *
 * docs/test-plan.md gate 0.3: "External flash JEDEC ID matches ZD25WQ16B;
 * LittleFS mounts, survives 100 write/reboot cycles."
 *
 * This sketch covers the JEDEC identification, the raw erase/write/read cycle
 * and the 100 reboot cycles. LittleFS is deliberately not mounted here: the
 * mount belongs to hal/block_store_littlefs, which needs an lfs_config bound to
 * this driver, and writing that twice -- once throwaway here and once for real
 * -- would prove nothing about the version that ships.
 *
 * The reboot loop drives itself. A host that has to shepherd 100 flash-and-read
 * rounds is a host that will get bored and stop at 12. But an unattended reset
 * loop is also how a device becomes unreachable: if it resets before USB comes
 * up, there is no CDC to touch at 1200 baud and no expiry for the dead man's
 * timer either, because every boot re-arms it. So every cycle holds a window
 * open first -- USB enumerates, the host can interrupt with STOP, and only then
 * does the cycle run. Slower, and always recoverable.
 *
 * Two details of that window are load-bearing, and the first version of this
 * sketch had both wrong. It wedged the host's xHCI controller at cycle 78 and
 * cost the run:
 *
 *     usb 1-1: new full-speed USB device number 102
 *     cdc_acm 1-1:1.0: ttyACM0: USB ACM device
 *     usb 1-1: USB disconnect, device number 102      <- same second
 *     usb 1-1: device not accepting address 103, error -71
 *     usb 1-1: WARN: invalid context state for evaluate context command
 *
 *   * The window has to start when USB becomes ready, not at boot. Enumeration
 *     takes a few seconds on its own, so a window measured from boot has often
 *     already elapsed by the time the host finishes -- and the device then
 *     resets milliseconds after the port appears, over and over.
 *   * The device has to detach before it resets. Vanishing in the middle of the
 *     host's setup sequence is what produced error -71, and after enough of
 *     those the controller stops offering the port at all. Recovering that
 *     needs root or a hand on the cable; neither is available at 3 a.m.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 4

#include <Arduino.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <Adafruit_SPIFlash.h>
#include <Adafruit_TinyUSB.h>
#pragma GCC diagnostic pop

#include "common.h"
#include "report.h"

namespace {

Adafruit_FlashTransport_QSPI g_transport;
Adafruit_SPIFlash g_flash(&g_transport);

/// LilyGO fits either part depending on supply (variant.h). Both descriptors are
/// passed to begin() explicitly: ZD25WQ16B is defined in the library's
/// flash_devices.h but is not in the list begin() uses when it is given nothing,
/// so the no-argument call returns false on this board with a perfectly healthy
/// chip attached. Measured -- that is what the first run of this sketch did.
const SPIFlash_Device_t kCandidates[] = {ZD25WQ16B, MX25R1635F};

constexpr uint32_t kJedecZd25wq16b = 0xBA6015;
constexpr uint32_t kJedecMx25r1635f = 0xC22815;

constexpr uint8_t kCmdReadJedecId = 0x9F;

constexpr uint32_t kTargetCycles = 100;

/// How long the host gets to see the device on each cycle, measured from the
/// moment enumeration completes. Long enough to open the port, read the report
/// and send STOP.
constexpr uint32_t kWindowMs = 6000;

/// The state record lives in the last sector so that the eventual LittleFS
/// volume, which starts at offset 0, never collides with it.
constexpr uint32_t kRecordMagic = 0x4D584C46; // "MXLF"
constexpr uint32_t kSectorSize = 4096;

/// Deep power-down and release. CLAUDE.md 3.0 requires the chip to be parked
/// whenever it is idle -- roughly 12 uA against a sub-20 uA budget for the whole
/// DEEP_IDLE state, so it is most of that budget on its own. Measuring the
/// current needs an instrument (gate 0.4); what can be established here is
/// whether the chip actually obeys the command.
constexpr uint8_t kCmdDeepPowerDown = 0xB9;
constexpr uint8_t kCmdReleaseDeepPowerDown = 0xAB;

struct Record {
    uint32_t magic;
    uint32_t cycle;
    uint32_t pattern;
    uint32_t check;
};

uint32_t checkOf(const Record &record)
{
    // Not a CRC -- just enough that a half-erased record cannot read as valid.
    return record.magic ^ (record.cycle * 2654435761u) ^ record.pattern;
}

uint32_t recordAddress()
{
    return g_flash.size() - kSectorSize;
}

bool readRecord(Record &out)
{
    if (g_flash.readBuffer(recordAddress(), reinterpret_cast<uint8_t *>(&out), sizeof(out))
        != sizeof(out)) {
        return false;
    }
    return out.magic == kRecordMagic && out.check == checkOf(out);
}

bool writeRecord(const Record &in)
{
    Record record = in;
    record.magic = kRecordMagic;
    record.check = checkOf(record);
    if (!g_flash.eraseSector(recordAddress() / kSectorSize)) {
        return false;
    }
    return g_flash.writeBuffer(recordAddress(), reinterpret_cast<const uint8_t *>(&record),
                               sizeof(record))
           == sizeof(record);
}

/// Erase, write a pattern, read it back. A sector well away from the record so
/// that a failure here cannot destroy the cycle count that reports it.
bool patternRoundTrip(uint32_t seed, uint32_t &mismatches)
{
    constexpr uint32_t kTestSector = 16;
    constexpr size_t kChunk = 256;

    uint8_t written[kChunk];
    uint8_t read[kChunk];
    for (size_t i = 0; i < kChunk; ++i) {
        written[i] = static_cast<uint8_t>((seed >> ((i % 4) * 8)) ^ (i * 31u));
    }

    if (!g_flash.eraseSector(kTestSector)) {
        return false;
    }
    const uint32_t address = kTestSector * kSectorSize;
    if (g_flash.writeBuffer(address, written, kChunk) != kChunk) {
        return false;
    }
    if (g_flash.readBuffer(address, read, kChunk) != kChunk) {
        return false;
    }
    mismatches = 0;
    for (size_t i = 0; i < kChunk; ++i) {
        if (read[i] != written[i]) {
            ++mismatches;
        }
    }
    return true;
}

bool g_flashOk = false;
uint32_t g_jedec = 0;
uint32_t g_jedecRaw = 0;
Record g_record{};
bool g_recordValid = false;
bool g_stopped = false;
bool g_reported = false;
uint32_t g_windowOpenedAt = 0;

void pumpStop()
{
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c == 'S' || c == 's') {
            g_stopped = true;
            report::info("STOP received -- the cycle loop is halted");
        }
    }
}

void reportState(const char *phase)
{
    report::begin(4);
    report::info("phase=%s qspi sck=P1.14 cs=P1.15 io0=P1.12 io1=P1.13 io2=P0.07 io3=P0.05", phase);
    report::value("flash.begin", "%d", g_flashOk ? 1 : 0);
    report::value("flash.jedec_raw", "0x%06lX", static_cast<unsigned long>(g_jedecRaw));
    report::value("flash.jedec", "0x%06lX", static_cast<unsigned long>(g_jedec));
    report::value("flash.part", "%s",
                  g_jedecRaw == kJedecZd25wq16b   ? "ZD25WQ16B"
                  : g_jedecRaw == kJedecMx25r1635f ? "MX25R1635F"
                                                   : "unknown");
    report::value("flash.size_bytes", "%lu", static_cast<unsigned long>(g_flash.size()));
    report::value("cycle.record_valid", "%d", g_recordValid ? 1 : 0);
    report::value("cycle.count", "%lu", static_cast<unsigned long>(g_record.cycle));
    report::value("cycle.target", "%lu", static_cast<unsigned long>(kTargetCycles));
    report::end(4);
}

} // namespace

namespace bringup {

void setup()
{
    commonSetup(120000);

    // Read the identity through the transport first, independent of whether the
    // driver recognises the part. A begin() that fails must not also cost the
    // one number that says which chip is on the board.
    g_transport.begin();
    uint8_t jedec[3] = {0, 0, 0};
    if (g_transport.readCommand(kCmdReadJedecId, jedec, sizeof(jedec))) {
        g_jedecRaw = (static_cast<uint32_t>(jedec[0]) << 16)
                     | (static_cast<uint32_t>(jedec[1]) << 8)
                     | static_cast<uint32_t>(jedec[2]);
    }

    g_flashOk = g_flash.begin(kCandidates, sizeof(kCandidates) / sizeof(kCandidates[0]));
    if (g_flashOk) {
        g_jedec = g_flash.getJEDECID();
        g_recordValid = readRecord(g_record);
        if (!g_recordValid) {
            g_record.cycle = 0;
            g_record.pattern = 0x1234ABCD;
        }
    }
    g_windowOpenedAt = 0; // starts when USB is actually up, see loop()
}

void loop()
{
    pumpStop();

    // The window starts when the host has the device, not when the device
    // booted. Measured from boot it is usually already over by the time
    // enumeration finishes, which turns "a window on every cycle" into "no
    // window at all".
    if (g_windowOpenedAt == 0 && usbReady()) {
        g_windowOpenedAt = millis();
    }
    const bool windowElapsed = g_windowOpenedAt != 0 && (millis() - g_windowOpenedAt) > kWindowMs;
    if (!g_reported && usbReady()) {
        reportState(g_record.cycle >= kTargetCycles ? "done" : "cycling");
        g_reported = true;
    }

    if (!windowElapsed || !usbReady()) {
        delay(50);
        commonService();
        return;
    }

    if (g_stopped || !g_flashOk) {
        if (!g_flashOk) {
            report::verdict("fail", "Adafruit_SPIFlash::begin() returned false");
        }
        delay(500);
        commonService();
        return;
    }

    if (g_record.cycle >= kTargetCycles) {
        // Final report: the pattern round trip and the deep power-down check are
        // done once, at the end, so they do not add 100 erase cycles of their own.
        //
        // The begin/end pair is not decoration. tools/bringup_run.py collects
        // from a "MAXL-BRINGUP 04 begin" through the matching "end" and stops;
        // without the frame it reads this report for ever and reports no
        // complete cycle. The first version of this block omitted it, and the
        // result was a gate that had actually passed on the device sitting
        // uncollected behind a 900 second timeout.
        report::begin(4);

        uint32_t mismatches = 0;
        const bool roundTrip = patternRoundTrip(0xC0FFEE, mismatches);
        report::value("pattern.round_trip", "%d", roundTrip ? 1 : 0);
        report::value("pattern.mismatched_bytes", "%lu", static_cast<unsigned long>(mismatches));

        // Deep power-down: after 0xB9 the chip should stop answering, and 0xAB
        // should bring it back. If it answers throughout, it never slept and
        // CLAUDE.md 3.0's idle budget is built on something that is not happening.
        g_transport.runCommand(kCmdDeepPowerDown);
        delay(5);
        const uint32_t whileAsleep = g_flash.getJEDECID();
        g_transport.runCommand(kCmdReleaseDeepPowerDown);
        delay(5);
        const uint32_t afterWake = g_flash.getJEDECID();
        report::value("dpd.jedec_while_asleep", "0x%06lX", static_cast<unsigned long>(whileAsleep));
        report::value("dpd.jedec_after_wake", "0x%06lX", static_cast<unsigned long>(afterWake));
        report::value("dpd.entered", "%d", whileAsleep != g_jedecRaw ? 1 : 0);
        report::value("dpd.woke", "%d", afterWake == g_jedecRaw ? 1 : 0);

        const bool ok = (g_jedecRaw == kJedecZd25wq16b || g_jedecRaw == kJedecMx25r1635f)
                        && roundTrip && mismatches == 0 && g_record.cycle >= kTargetCycles;
        report::verdict(ok ? "pass" : "fail",
                        "JEDEC id, 100 write/reboot cycles and a pattern round trip");
        report::info("LittleFS mount is not covered here -- see hal/block_store_littlefs");
        report::end(4);
        delay(3000);
        commonService();
        return;
    }

    // One cycle: verify what the previous boot wrote, write the next record,
    // read it straight back, then reset. Verifying before writing is what makes
    // this a persistence test rather than a write test.
    Record verify{};
    const bool previousSurvived = (g_record.cycle == 0) || readRecord(verify);
    if (!previousSurvived || (g_record.cycle > 0 && verify.cycle != g_record.cycle)) {
        report::value("cycle.failed_at", "%lu", static_cast<unsigned long>(g_record.cycle));
        report::verdict("fail", "the record written before the last reset did not survive it");
        g_stopped = true;
        delay(500);
        commonService();
        return;
    }

    Record next = g_record;
    next.cycle += 1;
    next.pattern = next.pattern * 1664525u + 1013904223u;
    if (!writeRecord(next)) {
        report::value("cycle.failed_at", "%lu", static_cast<unsigned long>(g_record.cycle));
        report::verdict("fail", "write of the cycle record failed");
        g_stopped = true;
        delay(500);
        commonService();
        return;
    }

    report::info("cycle %lu written, resetting", static_cast<unsigned long>(next.cycle));
    Serial.flush();
    delay(50);

    // Detach before resetting. Disappearing mid-transaction is what wedged the
    // host controller on the first run; a clean disconnect lets it tear the
    // device down in order.
    TinyUSBDevice.detach();
    delay(250);
    NVIC_SystemReset();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 4
