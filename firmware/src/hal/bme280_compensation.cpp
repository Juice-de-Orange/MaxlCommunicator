#include "hal/bme280_compensation.h"

namespace hal {
namespace {

/// Datasheet BME280_compensate_T_int32. Result in 0.01 degC, t_fine out.
int32_t compensateTemperature(const Bme280Calibration &cal, int32_t adc, int32_t &tFine)
{
    const int32_t var1 =
        (((adc >> 3) - (static_cast<int32_t>(cal.digT1) << 1)) * static_cast<int32_t>(cal.digT2)) >> 11;
    const int32_t d = (adc >> 4) - static_cast<int32_t>(cal.digT1);
    const int32_t var2 = (((d * d) >> 12) * static_cast<int32_t>(cal.digT3)) >> 14;
    tFine = var1 + var2;
    return (tFine * 5 + 128) >> 8;
}

/// Datasheet BME280_compensate_P_int64. Result in Q24.8 Pa.
uint32_t compensatePressure(const Bme280Calibration &cal, int32_t adc, int32_t tFine)
{
    int64_t var1 = static_cast<int64_t>(tFine) - 128000;
    int64_t var2 = var1 * var1 * static_cast<int64_t>(cal.digP6);
    var2 = var2 + ((var1 * static_cast<int64_t>(cal.digP5)) << 17);
    var2 = var2 + (static_cast<int64_t>(cal.digP4) << 35);
    var1 = ((var1 * var1 * static_cast<int64_t>(cal.digP3)) >> 8) +
           ((var1 * static_cast<int64_t>(cal.digP2)) << 12);
    var1 = (((static_cast<int64_t>(1) << 47) + var1) * static_cast<int64_t>(cal.digP1)) >> 33;

    // The datasheet's own guard. digP1 of zero means uncalibrated silicon, and
    // the division below would be by zero.
    if (var1 == 0) {
        return 0;
    }

    int64_t p = 1048576 - adc;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (static_cast<int64_t>(cal.digP9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (static_cast<int64_t>(cal.digP8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (static_cast<int64_t>(cal.digP7) << 4);

    if (p < 0) {
        return 0;
    }
    return static_cast<uint32_t>(p);
}

/// Datasheet BME280_compensate_H_int32. Result in Q22.10 %RH.
uint32_t compensateHumidity(const Bme280Calibration &cal, int32_t adc, int32_t tFine)
{
    int32_t v = tFine - 76800;
    v = (((((adc << 14) - (static_cast<int32_t>(cal.digH4) << 20) -
            (static_cast<int32_t>(cal.digH5) * v)) +
           16384) >>
          15) *
         (((((((v * static_cast<int32_t>(cal.digH6)) >> 10) *
              (((v * static_cast<int32_t>(cal.digH3)) >> 11) + 32768)) >>
             10) +
            2097152) *
               static_cast<int32_t>(cal.digH2) +
           8192) >>
          14));
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * static_cast<int32_t>(cal.digH1)) >> 4);

    // 419430400 is 100 %RH in Q22.10 shifted by 12 -- the datasheet's clamp, and
    // the reason a humidity reading can never come out above 100 %.
    if (v < 0) {
        v = 0;
    }
    if (v > 419430400) {
        v = 419430400;
    }
    return static_cast<uint32_t>(v >> 12);
}

} // namespace

Bme280Compensated bme280Compensate(const Bme280Calibration &cal, const Bme280Raw &raw)
{
    Bme280Compensated out;
    out.tempCentiC = compensateTemperature(cal, raw.temperature, out.tFine);

    // Q24.8 Pa -> Pa. CLAUDE.md 2.2 puts whole pascals on the wire; a quarter of
    // a pascal is well under the part's own noise floor.
    out.pressurePa = compensatePressure(cal, raw.pressure, out.tFine) / 256u;

    // Q22.10 %RH -> 0.01 %RH. Multiplying before dividing keeps the hundredths;
    // the other order throws them away and reports whole percent.
    const uint32_t humidityQ22_10 = compensateHumidity(cal, raw.humidity, out.tFine);
    out.humidityCentiPct = (humidityQ22_10 * 100u) / 1024u;

    return out;
}

} // namespace hal
