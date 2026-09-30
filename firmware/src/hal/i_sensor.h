/*
 * Environmental sensor, behind an interface.
 *
 * The units are not this layer's choice. CLAUDE.md 2.2 fixes the TELEMETRY
 * payload encoding and these fields are copied into it byte for byte:
 *
 *   tempC     int16   0.01 degC
 *   humidity  uint16  0.01 %RH
 *   pressure  uint32  Pa
 *
 * Converting at the sensor rather than at the frame keeps the rounding in one
 * place, and it means link/ never has to know what a BME280 is.
 *
 * `humidityValid` exists because of what bring-up sketch 02 found. The chip ID
 * on node A read 0x60, which is a BME280; 0x58 would have been a BMP280, same
 * package, same address, no humidity sensor. Under a silent driver that field
 * would simply have gone out as zero for ever. It is checked, and if it is a
 * BMP280 the flag says so instead of the reading lying.
 */

#ifndef MAXL_HAL_I_SENSOR_H
#define MAXL_HAL_I_SENSOR_H

#include <stdint.h>

namespace hal {

struct SensorSample {
    int16_t tempCentiC = 0;      ///< 0.01 degC
    uint16_t humidityCentiPct = 0; ///< 0.01 %RH
    uint32_t pressurePa = 0;     ///< Pa
    bool valid = false;          ///< the read completed
    bool humidityValid = false;  ///< the part actually has a humidity sensor
};

class IEnvironmentSensor {
public:
    virtual ~IEnvironmentSensor() = default;

    /// Probe, read calibration, configure. False if the part does not answer.
    virtual bool begin() = 0;

    /// One forced conversion, then back to sleep. CLAUDE.md 1.5's power
    /// discipline applies to every peripheral, not only to the GNSS: a sensor in
    /// normal mode converts for ever whether anybody is reading it or not.
    virtual SensorSample read() = 0;

    /// The chip ID actually found. 0x60 = BME280, 0x58 = BMP280, 0 = no answer.
    virtual uint8_t chipId() const = 0;
};

} // namespace hal

#endif // MAXL_HAL_I_SENSOR_H
