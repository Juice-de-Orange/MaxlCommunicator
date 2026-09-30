/*
 * IEnvironmentSensor on the BME280 at 0x77.
 *
 * Confirmed on node A by bring-up sketch 02, 2026-08-31: address 0x77, chip id
 * 0x60. The id check is not ceremony. 0x58 is a BMP280 -- same package, same
 * address, no humidity sensor -- and under a driver that did not look, the
 * humidity field of every TELEMETRY frame would have been a silent zero.
 *
 * Forced mode, one conversion per read, back to sleep afterwards. Normal mode
 * would have the part converting continuously between the telemetry intervals
 * of CLAUDE.md 3, which on a 2.38 mA budget is not free.
 *
 * The arithmetic is not here. bme280_compensation.cpp holds it, builds on the
 * host and is tested there; what is left in this file is register access, which
 * cannot be tested without the part and should therefore be as small as it can
 * be made.
 */

#ifndef MAXL_HAL_SENSOR_BME280_H
#define MAXL_HAL_SENSOR_BME280_H

#include "hal/bme280_compensation.h"
#include "hal/i_sensor.h"

#include <stdint.h>

namespace hal {

class Bme280Sensor : public IEnvironmentSensor {
public:
    static constexpr uint8_t kAddress = 0x77;
    static constexpr uint8_t kAddressAlt = 0x76; ///< if SDO is tied to ground
    static constexpr uint8_t kChipIdBme280 = 0x60;
    static constexpr uint8_t kChipIdBmp280 = 0x58;

    bool begin() override;
    SensorSample read() override;
    uint8_t chipId() const override { return chipId_; }

    /// Which of the two addresses answered. 0 if neither did.
    uint8_t address() const { return address_; }

private:
    bool probe(uint8_t address);
    bool readCalibration();
    bool readRegisters(uint8_t reg, uint8_t *out, uint8_t len) const;
    bool writeRegister(uint8_t reg, uint8_t value) const;

    Bme280Calibration cal_;
    uint8_t address_ = 0;
    uint8_t chipId_ = 0;
    bool ready_ = false;
};

} // namespace hal

#endif // MAXL_HAL_SENSOR_BME280_H
