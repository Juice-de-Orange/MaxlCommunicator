#include "hal/sensor_bme280.h"

#include <Arduino.h>
#include <Wire.h>

namespace hal {
namespace {

constexpr uint8_t kRegChipId = 0xD0;
constexpr uint8_t kRegReset = 0xE0;
constexpr uint8_t kRegCtrlHum = 0xF2;
constexpr uint8_t kRegStatus = 0xF3;
constexpr uint8_t kRegCtrlMeas = 0xF4;
constexpr uint8_t kRegConfig = 0xF5;
constexpr uint8_t kRegPressMsb = 0xF7;

constexpr uint8_t kRegCalibT = 0x88;   ///< 0x88..0x9F, 24 bytes: T1..T3, P1..P9
constexpr uint8_t kRegCalibH1 = 0xA1;
constexpr uint8_t kRegCalibH2 = 0xE1;  ///< 0xE1..0xE7, 7 bytes

constexpr uint8_t kOversample1x = 0x01;
constexpr uint8_t kModeForced = 0x01;

/// Bit 3 of 0xF3. Set while a conversion is running.
constexpr uint8_t kStatusMeasuring = 0x08;

uint16_t u16le(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
int16_t s16le(const uint8_t *p) { return static_cast<int16_t>(u16le(p)); }

} // namespace

bool Bme280Sensor::readRegisters(uint8_t reg, uint8_t *out, uint8_t len) const
{
    Wire.beginTransmission(address_);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom(address_, len) != len) {
        return false;
    }
    for (uint8_t i = 0; i < len; ++i) {
        out[i] = static_cast<uint8_t>(Wire.read());
    }
    return true;
}

bool Bme280Sensor::writeRegister(uint8_t reg, uint8_t value) const
{
    Wire.beginTransmission(address_);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool Bme280Sensor::probe(uint8_t address)
{
    address_ = address;
    uint8_t id = 0;
    if (!readRegisters(kRegChipId, &id, 1)) {
        address_ = 0;
        return false;
    }
    chipId_ = id;
    return id == kChipIdBme280 || id == kChipIdBmp280;
}

bool Bme280Sensor::readCalibration()
{
    uint8_t block[26] = {0};
    if (!readRegisters(kRegCalibT, block, 24)) {
        return false;
    }
    cal_.digT1 = u16le(block + 0);
    cal_.digT2 = s16le(block + 2);
    cal_.digT3 = s16le(block + 4);
    cal_.digP1 = u16le(block + 6);
    cal_.digP2 = s16le(block + 8);
    cal_.digP3 = s16le(block + 10);
    cal_.digP4 = s16le(block + 12);
    cal_.digP5 = s16le(block + 14);
    cal_.digP6 = s16le(block + 16);
    cal_.digP7 = s16le(block + 18);
    cal_.digP8 = s16le(block + 20);
    cal_.digP9 = s16le(block + 22);

    if (chipId_ != kChipIdBme280) {
        return true; // a BMP280 has no humidity calibration to read
    }

    uint8_t h1 = 0;
    if (!readRegisters(kRegCalibH1, &h1, 1)) {
        return false;
    }
    cal_.digH1 = h1;

    uint8_t h[7] = {0};
    if (!readRegisters(kRegCalibH2, h, 7)) {
        return false;
    }
    cal_.digH2 = s16le(h + 0);
    cal_.digH3 = h[2];

    /*
     * H4 and H5 share a nibble in register 0xE5, which is the one place in this
     * part's calibration that is genuinely easy to get wrong:
     *   H4 = 0xE4 << 4 | (0xE5 & 0x0F)
     *   H5 = 0xE6 << 4 | (0xE5 >> 4)
     * Both are signed 12-bit values sitting in 16-bit words.
     */
    cal_.digH4 = static_cast<int16_t>((static_cast<int16_t>(static_cast<int8_t>(h[3])) << 4) |
                                      (h[4] & 0x0F));
    cal_.digH5 = static_cast<int16_t>((static_cast<int16_t>(static_cast<int8_t>(h[5])) << 4) |
                                      (h[4] >> 4));
    cal_.digH6 = static_cast<int8_t>(h[6]);
    return true;
}

bool Bme280Sensor::begin()
{
    ready_ = false;
    chipId_ = 0;

    if (!probe(kAddress) && !probe(kAddressAlt)) {
        address_ = 0;
        return false;
    }

    if (!readCalibration()) {
        return false;
    }

    // 1x oversampling on all three channels and no IIR filter. The part's own
    // noise at 1x is well inside gate 1.3's tolerance (2 degC, 5 %RH, 3 hPa),
    // and every extra sample is conversion time the device spends awake.
    if (chipId_ == kChipIdBme280 && !writeRegister(kRegCtrlHum, kOversample1x)) {
        return false;
    }
    if (!writeRegister(kRegConfig, 0x00)) {
        return false;
    }

    ready_ = true;
    return true;
}

SensorSample Bme280Sensor::read()
{
    SensorSample sample;
    if (!ready_) {
        return sample;
    }

    // ctrl_meas also carries the mode, so writing it is what starts the
    // conversion. Humidity oversampling in ctrl_hum only takes effect when
    // ctrl_meas is written -- a datasheet quirk, and the reason the order here
    // is not free to change.
    const uint8_t ctrl = static_cast<uint8_t>((kOversample1x << 5) | (kOversample1x << 2) | kModeForced);
    if (!writeRegister(kRegCtrlMeas, ctrl)) {
        return sample;
    }

    // A 1x conversion of all three channels is under 10 ms. The bound is
    // generous and the loop exits on the status bit, so it costs nothing when
    // the part is quick.
    const uint32_t deadline = millis() + 100;
    for (;;) {
        uint8_t status = 0;
        if (!readRegisters(kRegStatus, &status, 1)) {
            return sample;
        }
        if ((status & kStatusMeasuring) == 0) {
            break;
        }
        if (static_cast<int32_t>(millis() - deadline) >= 0) {
            return sample;
        }
        delay(1);
    }

    uint8_t raw[8] = {0};
    if (!readRegisters(kRegPressMsb, raw, 8)) {
        return sample;
    }

    Bme280Raw values;
    values.pressure = (static_cast<int32_t>(raw[0]) << 12) | (static_cast<int32_t>(raw[1]) << 4) |
                      (raw[2] >> 4);
    values.temperature = (static_cast<int32_t>(raw[3]) << 12) | (static_cast<int32_t>(raw[4]) << 4) |
                         (raw[5] >> 4);
    values.humidity = (static_cast<int32_t>(raw[6]) << 8) | raw[7];

    const Bme280Compensated out = bme280Compensate(cal_, values);
    sample.tempCentiC = static_cast<int16_t>(out.tempCentiC);
    sample.pressurePa = out.pressurePa;
    sample.valid = true;

    if (chipId_ == kChipIdBme280) {
        sample.humidityCentiPct = static_cast<uint16_t>(out.humidityCentiPct);
        sample.humidityValid = true;
    }

    return sample;
}

} // namespace hal
