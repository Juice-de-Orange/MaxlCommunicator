/*
 * Bring-up 02 -- the I2C bus, and what is actually on it.
 *
 * SDA P0.26 / SCL P0.27 per docs/hardware/pinmap.md. Two devices are expected:
 * the BME280 at 0x76 or 0x77 and the PCF8563 real-time clock at 0x51. Both hang
 * off VDD_POWR, which initVariant() has already enabled through PIN_PWR_ON.
 *
 * The scan alone would only prove that something acknowledges an address, so
 * each device is then identified from its own registers:
 *
 *   BME280   register 0xD0 is the chip id. 0x60 is a BME280, 0x58 a BMP280 and
 *            0x61 a BME680. That distinction matters: CLAUDE.md 0 says a BME280
 *            is fitted, and a BMP280 has no humidity sensor at all, which would
 *            silently remove a field from the TELEMETRY payload in 2.2.
 *
 *   PCF8563  has no id register. Register 0x02 bit 7 is VL -- "voltage low",
 *            set by the chip itself when its supply has dropped far enough that
 *            the time is no longer trustworthy. Reading it answers open decision
 *            D2 directly: if VL is clear on a device that has been unpowered,
 *            the clock is backed; if it is set on every cold start, it is not.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 2

#include <Arduino.h>
#include <Wire.h>

#include "common.h"
#include "report.h"

namespace {

constexpr uint8_t kAddrRtc = 0x51;
constexpr uint8_t kAddrBmeLow = 0x76;
constexpr uint8_t kAddrBmeHigh = 0x77;

constexpr uint8_t kBmeRegChipId = 0xD0;
constexpr uint8_t kPcfRegSeconds = 0x02;

bool present(uint8_t address)
{
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

/// Returns false if the device did not answer, so a missing chip cannot be
/// mistaken for a register that happens to read 0x00 or 0xFF.
bool readRegister(uint8_t address, uint8_t reg, uint8_t &out)
{
    Wire.beginTransmission(address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom(address, static_cast<uint8_t>(1)) != 1) {
        return false;
    }
    out = static_cast<uint8_t>(Wire.read());
    return true;
}

const char *bmeChipName(uint8_t id)
{
    switch (id) {
    case 0x60:
        return "BME280";
    case 0x58:
        return "BMP280 (no humidity)";
    case 0x61:
        return "BME680";
    default:
        return "unknown";
    }
}

void runOnce()
{
    report::begin(2);
    report::info("sda=P0.26 scl=P0.27, both devices on VDD_POWR via PIN_PWR_ON");

    uint8_t found = 0;
    for (uint8_t address = 0x08; address < 0x78; ++address) {
        if (present(address)) {
            ++found;
            report::value("i2c.addr", "0x%02X", address);
        }
    }
    report::value("i2c.count", "%u", static_cast<unsigned>(found));

    // --- BME280 -----------------------------------------------------------
    uint8_t bmeAddr = 0;
    if (present(kAddrBmeHigh)) {
        bmeAddr = kAddrBmeHigh;
    } else if (present(kAddrBmeLow)) {
        bmeAddr = kAddrBmeLow;
    }

    bool sensorOk = false;
    if (bmeAddr == 0) {
        report::value("bme.addr", "none");
    } else {
        report::value("bme.addr", "0x%02X", bmeAddr);
        uint8_t chipId = 0;
        if (readRegister(bmeAddr, kBmeRegChipId, chipId)) {
            report::value("bme.chipid", "0x%02X (%s)", chipId, bmeChipName(chipId));
            sensorOk = (chipId == 0x60);
        } else {
            report::value("bme.chipid", "read failed");
        }
    }

    // --- PCF8563 ----------------------------------------------------------
    bool rtcOk = false;
    if (!present(kAddrRtc)) {
        report::value("rtc.addr", "none");
    } else {
        report::value("rtc.addr", "0x%02X", kAddrRtc);
        uint8_t seconds = 0;
        if (readRegister(kAddrRtc, kPcfRegSeconds, seconds)) {
            const bool voltageLow = (seconds & 0x80) != 0;
            const uint8_t bcd = static_cast<uint8_t>(seconds & 0x7F);
            report::value("rtc.seconds", "%u", static_cast<unsigned>((bcd >> 4) * 10 + (bcd & 0x0F)));
            // D2: set means the chip lost enough supply to distrust its own time.
            report::value("rtc.vl_flag", "%d", voltageLow ? 1 : 0);
            report::value("rtc.time_trusted", "%d", voltageLow ? 0 : 1);
            rtcOk = true;
        } else {
            report::value("rtc.seconds", "read failed");
        }
    }

    if (sensorOk && rtcOk) {
        report::verdict("pass", "BME280 and PCF8563 both answer and identify");
    } else {
        report::verdict("fail", "expected a BME280 at 0x76/0x77 and a PCF8563 at 0x51");
    }
    report::end(2);
}

} // namespace

namespace bringup {

void setup()
{
    commonSetup(90000);
    Wire.begin();
}

void loop()
{
    runOnce();
    delay(2000);
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 2
