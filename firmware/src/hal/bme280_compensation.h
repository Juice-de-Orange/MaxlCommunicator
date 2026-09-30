/*
 * The BME280's compensation arithmetic, on its own and away from the I2C.
 *
 * Bosch does not publish a formula for these sensors -- it publishes an integer
 * reference implementation, and the calibration words are meaningless without
 * it. What follows is that reference arithmetic (datasheet BST-BME280-DS002,
 * section 4.2.3: BME280_compensate_T_int32, _P_int64, _H_int32), transcribed
 * rather than reinvented, with the results converted into the units CLAUDE.md
 * 2.2 puts on the wire.
 *
 * It sits in its own translation unit for one reason: it is pure integer
 * arithmetic over a struct, so the host build compiles it and the tests can
 * drive it. The I2C register access in sensor_bme280.cpp cannot be tested
 * without the part; this can, and this is where the shifts that are easy to get
 * wrong actually live.
 *
 * What the tests can and cannot settle, stated plainly: they check the
 * properties -- monotonicity in the raw reading, humidity clamped to 0..100 %,
 * the t_fine coupling that makes pressure depend on temperature, the unit
 * conversions. They cannot check absolute accuracy. That is docs/test-plan.md
 * gate 1.3, it needs a reference instrument, and no amount of host testing
 * substitutes for it.
 */

#ifndef MAXL_HAL_BME280_COMPENSATION_H
#define MAXL_HAL_BME280_COMPENSATION_H

#include <stdint.h>

namespace hal {

/// The calibration words, exactly as they are laid out in the part's registers
/// (0x88..0xA1 and 0xE1..0xE7). Named as the datasheet names them, because a
/// rename here is a bug waiting for the next person who compares the two.
struct Bme280Calibration {
    uint16_t digT1 = 0;
    int16_t digT2 = 0;
    int16_t digT3 = 0;

    uint16_t digP1 = 0;
    int16_t digP2 = 0;
    int16_t digP3 = 0;
    int16_t digP4 = 0;
    int16_t digP5 = 0;
    int16_t digP6 = 0;
    int16_t digP7 = 0;
    int16_t digP8 = 0;
    int16_t digP9 = 0;

    uint8_t digH1 = 0;
    int16_t digH2 = 0;
    uint8_t digH3 = 0;
    int16_t digH4 = 0;
    int16_t digH5 = 0;
    int8_t digH6 = 0;
};

/// Raw 20-bit (T, P) and 16-bit (H) readings straight out of registers 0xF7..0xFE.
struct Bme280Raw {
    int32_t temperature = 0;
    int32_t pressure = 0;
    int32_t humidity = 0;
};

struct Bme280Compensated {
    int32_t tempCentiC = 0;      ///< 0.01 degC
    uint32_t pressurePa = 0;     ///< Pa
    uint32_t humidityCentiPct = 0; ///< 0.01 %RH
    int32_t tFine = 0;           ///< the datasheet's carrier between the three
};

/*
 * All three compensations in one call, because they are not independent: t_fine
 * is computed by the temperature step and consumed by the other two. Exposing
 * them separately would let a caller compensate pressure against a stale
 * temperature, which is a bug that reads perfectly well.
 */
Bme280Compensated bme280Compensate(const Bme280Calibration &cal, const Bme280Raw &raw);

} // namespace hal

#endif // MAXL_HAL_BME280_COMPENSATION_H
