#include "clock_pcf8563.h"

#include <Arduino.h>
#include <Wire.h>

namespace hal {
namespace {

constexpr uint8_t kRegSeconds = 0x02;

uint8_t toBcd(uint8_t value)
{
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

uint8_t fromBcd(uint8_t value)
{
    return static_cast<uint8_t>(((value >> 4) * 10) + (value & 0x0F));
}

/// Days from 1970-01-01 to a civil date. Howard Hinnant's algorithm -- exact in
/// integers, and no library.
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

bool readRegisters(uint8_t reg, uint8_t *data, size_t len)
{
    Wire.beginTransmission(Pcf8563Clock::kAddress);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom(Pcf8563Clock::kAddress, static_cast<uint8_t>(len)) != len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        data[i] = static_cast<uint8_t>(Wire.read());
    }
    return true;
}

bool writeRegisters(uint8_t reg, const uint8_t *data, size_t len)
{
    Wire.beginTransmission(Pcf8563Clock::kAddress);
    Wire.write(reg);
    for (size_t i = 0; i < len; ++i) {
        Wire.write(data[i]);
    }
    return Wire.endTransmission() == 0;
}

} // namespace

bool Pcf8563Clock::begin()
{
    Wire.beginTransmission(kAddress);
    present_ = Wire.endTransmission() == 0;
    if (!present_) {
        return false;
    }

    uint8_t seconds = 0;
    if (!readRegisters(kRegSeconds, &seconds, 1)) {
        present_ = false;
        return false;
    }

    /*
     * Bit 7 of the seconds register is VL -- the chip's own statement that its
     * supply dropped far enough that the time is not to be trusted. Believing it
     * anyway is how a node reconstructs a duty cycle budget against a clock that
     * is hours out and then transmits sooner than the regulation allows.
     */
    voltageLowAtBoot_ = (seconds & 0x80) != 0;
    trusted_ = !voltageLowAtBoot_;
    return true;
}

uint32_t Pcf8563Clock::monotonicMs() const
{
    return millis();
}

bool Pcf8563Clock::timeValid() const
{
    return present_ && trusted_;
}

uint32_t Pcf8563Clock::unixSeconds() const
{
    if (!present_) {
        return 0;
    }
    uint8_t regs[7];
    if (!readRegisters(kRegSeconds, regs, sizeof(regs))) {
        return 0;
    }
    if ((regs[0] & 0x80) != 0) {
        // VL came up while running. The clock is no longer trustworthy and the
        // node must stop transmitting, not carry on with a plausible-looking
        // number.
        trusted_ = false;
        return 0;
    }

    const unsigned second = fromBcd(static_cast<uint8_t>(regs[0] & 0x7F));
    const unsigned minute = fromBcd(static_cast<uint8_t>(regs[1] & 0x7F));
    const unsigned hour = fromBcd(static_cast<uint8_t>(regs[2] & 0x3F));
    const unsigned day = fromBcd(static_cast<uint8_t>(regs[3] & 0x3F));
    const unsigned month = fromBcd(static_cast<uint8_t>(regs[5] & 0x1F));
    const unsigned year = 2000u + fromBcd(regs[6]);

    const int64_t days = daysFromCivil(static_cast<int64_t>(year), month, day);
    return static_cast<uint32_t>(days * 86400 + hour * 3600 + minute * 60 + second);
}

bool Pcf8563Clock::setUnixSeconds(uint32_t seconds)
{
    if (!present_) {
        return false;
    }

    const int64_t days = static_cast<int64_t>(seconds) / 86400;
    const uint32_t secondsOfDay = seconds % 86400u;

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civilFromDays(days, year, month, day);

    // 1970-01-01 was a Thursday. The chip counts 0..6 and does not care which
    // day maps to which number, only that it is consistent.
    const uint8_t weekday = static_cast<uint8_t>((days + 4) % 7);

    uint8_t regs[7];
    regs[0] = toBcd(static_cast<uint8_t>(secondsOfDay % 60)); // writing this clears VL
    regs[1] = toBcd(static_cast<uint8_t>((secondsOfDay / 60) % 60));
    regs[2] = toBcd(static_cast<uint8_t>(secondsOfDay / 3600));
    regs[3] = toBcd(static_cast<uint8_t>(day));
    regs[4] = weekday;
    regs[5] = toBcd(static_cast<uint8_t>(month)); // century bit 0 = 20xx
    regs[6] = toBcd(static_cast<uint8_t>(year - 2000));

    if (!writeRegisters(kRegSeconds, regs, sizeof(regs))) {
        return false;
    }
    voltageLowAtBoot_ = false;
    trusted_ = true;
    return true;
}

} // namespace hal
