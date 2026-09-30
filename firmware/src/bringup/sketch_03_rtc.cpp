/*
 * Bring-up 03 -- the PCF8563 real-time clock.
 *
 * docs/test-plan.md gate 0.5 and, behind it, CLAUDE.md 1.2: the duty cycle
 * budget is reconstructed against this clock on boot. "An RTC that drifts badly
 * turns a compliance mechanism into a guess." So three separate questions, and
 * they are not the same question:
 *
 *   1. Can it be set and read back?
 *   2. Does it survive a reset of the MCU?  -- the RESET command below
 *   3. How far does it drift?               -- host-side, over hours
 *
 * And a fourth, which is open decision D2: does the clock have a supply of its
 * own, or does it die with VDD_POWR? Gate 2.8 asks for the budget to survive a
 * battery pull, and if the answer here is no, that gate is unachievable as
 * written however correct the firmware is. PWROFF below drives PIN_PWR_ON low
 * and asks the chip whether it is still there.
 *
 * Commands on Serial, one per line:
 *
 *   TIME <unix>   set the clock (UTC)
 *   READ          report the time once
 *   RESET         NVIC_SystemReset -- the MCU restarts, the RTC should not
 *   PWROFF        cut VDD_POWR briefly and re-scan for the chip (D2)
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 3

#include <Arduino.h>
#include <Wire.h>

#include "common.h"
#include "report.h"

namespace {

constexpr uint8_t kAddrRtc = 0x51;
constexpr uint8_t kRegSeconds = 0x02;

/// RESETREAS bit 2, SREQ: the last reset came from NVIC_SystemReset().
///
/// The first attempt at this carried a marker through GPREGRET2 instead, on the
/// assumption that it survives a soft reset and that -- unlike GPREGRET -- the
/// UF2 bootloader does not touch it. Measured: it comes back as 0. The
/// bootloader runs on every reset and clears it. RESETREAS is the right signal
/// anyway; it is set by hardware and needs no cooperation from anyone.
constexpr uint32_t kResetReasonSreq = 0x04;

uint8_t toBcd(uint8_t value)
{
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

uint8_t fromBcd(uint8_t value)
{
    return static_cast<uint8_t>(((value >> 4) * 10) + (value & 0x0F));
}

bool writeRegisters(uint8_t reg, const uint8_t *data, size_t len)
{
    Wire.beginTransmission(kAddrRtc);
    Wire.write(reg);
    for (size_t i = 0; i < len; ++i) {
        Wire.write(data[i]);
    }
    return Wire.endTransmission() == 0;
}

bool readRegisters(uint8_t reg, uint8_t *data, size_t len)
{
    Wire.beginTransmission(kAddrRtc);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom(kAddrRtc, static_cast<uint8_t>(len)) != len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        data[i] = static_cast<uint8_t>(Wire.read());
    }
    return true;
}

/// Days from 1970-01-01 to the given civil date. Howard Hinnant's algorithm --
/// exact in integers, no library, and no 2038 surprise on a 64-bit host because
/// the device side only ever handles the 32-bit seconds it is given.
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int &y, unsigned &m, unsigned &d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    d = doy - (153u * mp + 2u) / 5u + 1u;
    m = mp + (mp < 10 ? 3 : -9);
    y = static_cast<int>(yy + (m <= 2));
}

bool setTime(uint32_t unixSeconds)
{
    const int64_t days = static_cast<int64_t>(unixSeconds) / 86400;
    const uint32_t secondsOfDay = unixSeconds % 86400u;

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civilFromDays(days, year, month, day);

    // 1970-01-01 was a Thursday; the PCF8563 counts 0..6 and does not care which
    // day maps to which number as long as it is consistent.
    const uint8_t weekday = static_cast<uint8_t>((days + 4) % 7);

    uint8_t regs[7];
    regs[0] = toBcd(static_cast<uint8_t>(secondsOfDay % 60));        // clears VL
    regs[1] = toBcd(static_cast<uint8_t>((secondsOfDay / 60) % 60));
    regs[2] = toBcd(static_cast<uint8_t>(secondsOfDay / 3600));
    regs[3] = toBcd(static_cast<uint8_t>(day));
    regs[4] = weekday;
    regs[5] = toBcd(static_cast<uint8_t>(month));                    // century bit 0 = 20xx
    regs[6] = toBcd(static_cast<uint8_t>(year - 2000));
    return writeRegisters(kRegSeconds, regs, sizeof(regs));
}

bool readTime(uint32_t &unixSeconds, bool &voltageLow)
{
    uint8_t regs[7];
    if (!readRegisters(kRegSeconds, regs, sizeof(regs))) {
        return false;
    }
    voltageLow = (regs[0] & 0x80) != 0;

    const unsigned second = fromBcd(static_cast<uint8_t>(regs[0] & 0x7F));
    const unsigned minute = fromBcd(static_cast<uint8_t>(regs[1] & 0x7F));
    const unsigned hour = fromBcd(static_cast<uint8_t>(regs[2] & 0x3F));
    const unsigned day = fromBcd(static_cast<uint8_t>(regs[3] & 0x3F));
    const unsigned month = fromBcd(static_cast<uint8_t>(regs[5] & 0x1F));
    const unsigned year = 2000u + fromBcd(regs[6]);

    const int64_t days = daysFromCivil(static_cast<int64_t>(year), month, day);
    unixSeconds = static_cast<uint32_t>(days * 86400 + hour * 3600 + minute * 60 + second);
    return true;
}

bool rtcPresent()
{
    Wire.beginTransmission(kAddrRtc);
    return Wire.endTransmission() == 0;
}

void reportTime(const char *prefix)
{
    uint32_t now = 0;
    bool voltageLow = false;
    if (!readTime(now, voltageLow)) {
        report::value(prefix, "read failed");
        return;
    }
    report::value(prefix, "%lu", static_cast<unsigned long>(now));
    report::value("rtc.vl_flag", "%d", voltageLow ? 1 : 0);
    report::value("mcu.millis", "%lu", static_cast<unsigned long>(millis()));
}

/// D2. Cut the peripheral rail and ask the chip whether it is still answering.
/// The e-paper control pins are parked as inputs first, exactly as
/// variant_shutdown() does -- left as outputs they leak into an unpowered panel.
void powerCycleRail()
{
    report::info("PWROFF: parking e-paper pins, driving PIN_PWR_ON low for 2000 ms");
    pinMode(PIN_EINK_CS, INPUT);
    pinMode(PIN_EINK_DC, INPUT);
    pinMode(PIN_EINK_RES, INPUT);
    pinMode(PIN_EINK_BUSY, INPUT);

    digitalWrite(PIN_PWR_ON, LOW);
    delay(2000);
    const bool duringCut = rtcPresent();
    digitalWrite(PIN_PWR_ON, HIGH);
    delay(200);
    const bool afterRestore = rtcPresent();

    report::value("rtc.acks_with_pwr_on_low", "%d", duringCut ? 1 : 0);
    report::value("rtc.acks_after_restore", "%d", afterRestore ? 1 : 0);
    if (duringCut) {
        // The schematic has VBUS reaching the same latch node through D5, so
        // with USB attached the rail may simply never drop. That is a fact about
        // the test setup, not about the clock.
        report::info("rail did not drop -- expected while USB is attached (VBUS via D5). "
                     "D2 needs this repeated on battery alone.");
    }
    uint32_t now = 0;
    bool voltageLow = false;
    if (readTime(now, voltageLow)) {
        report::value("rtc.after_rail_cut", "%lu", static_cast<unsigned long>(now));
        report::value("rtc.vl_after_rail_cut", "%d", voltageLow ? 1 : 0);
    }
}

void handleCommand(const char *line)
{
    if (strncmp(line, "TIME ", 5) == 0) {
        const uint32_t value = static_cast<uint32_t>(strtoul(line + 5, nullptr, 10));
        const bool ok = setTime(value);
        report::value("rtc.set", "%lu %s", static_cast<unsigned long>(value), ok ? "ok" : "failed");
        reportTime("rtc.readback");
    } else if (strcmp(line, "READ") == 0) {
        reportTime("rtc.now");
    } else if (strcmp(line, "PWROFF") == 0) {
        powerCycleRail();
    } else if (strcmp(line, "RESET") == 0) {
        report::info("RESET: NVIC_SystemReset, the RTC must not notice");
        Serial.flush();
        delay(50);
        NVIC_SystemReset();
    } else if (line[0] != '\0') {
        report::info("unknown command: %s", line);
    }
}

char g_input[64];
size_t g_inputLen = 0;

bool g_bootAfterReset = false;
uint32_t g_bootResetReason = 0;
bool g_bootReported = false;
uint32_t g_lastBootReport = 0;

void pumpSerial()
{
    while (Serial.available() > 0) {
        const int raw = Serial.read();
        if (raw < 0) {
            break;
        }
        const char c = static_cast<char>(raw);
        if (c == '\n' || c == '\r') {
            if (g_inputLen > 0) {
                g_input[g_inputLen] = '\0';
                handleCommand(g_input);
                g_inputLen = 0;
            }
        } else if (g_inputLen + 1 < sizeof(g_input)) {
            g_input[g_inputLen++] = c;
        }
    }
}

} // namespace

namespace bringup {

void setup()
{
    commonSetup(0xFFFFFFFF); // driven interactively; the host decides when it ends
    Wire.begin();

    // Only latched here. The report itself cannot be printed yet: at this point
    // USB has not enumerated, so anything written to Serial is dropped on the
    // floor -- which is exactly how the first run of this sketch lost its whole
    // boot report after a reset.
    g_bootResetReason = readResetReason();
    g_bootAfterReset = (g_bootResetReason & kResetReasonSreq) != 0;
}

void loop()
{
    // The boot facts are emitted once the host can actually receive them, and
    // then repeated at a slow cadence so a reader that attaches late still gets
    // a complete cycle rather than a fragment.
    if (usbReady() && (!g_bootReported || millis() - g_lastBootReport > 3000)) {
        report::begin(3);
        report::value("boot.after_soft_reset", "%d", g_bootAfterReset ? 1 : 0);
        report::value("boot.reset_reason", "0x%08lX", static_cast<unsigned long>(g_bootResetReason));
        report::value("rtc.present", "%d", rtcPresent() ? 1 : 0);
        reportTime("rtc.at_boot");
        report::info("commands: TIME <unix> | READ | RESET | PWROFF");
        report::end(3);
        g_bootReported = true;
        g_lastBootReport = millis();
    }

    pumpSerial();
    delay(50);
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 3
