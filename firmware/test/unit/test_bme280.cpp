/*
 * The BME280 compensation arithmetic.
 *
 * What this can settle and what it cannot, stated up front. It cannot settle
 * accuracy -- that is docs/test-plan.md gate 1.3, against a reference instrument,
 * and no host test substitutes for it. What it settles is that the transcription
 * of the datasheet's integer reference implementation behaves like the thing it
 * was transcribed from: the right units on the way out, humidity clamped where
 * the datasheet clamps it, monotonicity in the raw reading, and the t_fine
 * coupling that makes pressure depend on temperature.
 *
 * The calibration below is a plausible set of words from a real part. The
 * absolute numbers it produces are not the subject; the relationships are.
 */

#include "doctest.h"

#include "hal/bme280_compensation.h"

using namespace hal;

namespace {

Bme280Calibration typicalCalibration()
{
    Bme280Calibration cal;
    cal.digT1 = 28244;
    cal.digT2 = 26571;
    cal.digT3 = 50;

    cal.digP1 = 37381;
    cal.digP2 = -10645;
    cal.digP3 = 3024;
    cal.digP4 = 6737;
    cal.digP5 = -134;
    cal.digP6 = -7;
    cal.digP7 = 9900;
    cal.digP8 = -10230;
    cal.digP9 = 4285;

    cal.digH1 = 75;
    cal.digH2 = 359;
    cal.digH3 = 0;
    cal.digH4 = 331;
    cal.digH5 = 0;
    cal.digH6 = 30;
    return cal;
}

/// Mid-scale raw readings -- what the part reports around room conditions.
constexpr int32_t kRawTemp = 519888;
constexpr int32_t kRawPressure = 415148;
constexpr int32_t kRawHumidity = 32768;

} // namespace

TEST_CASE("the three results come back in the units CLAUDE.md 2.2 puts on the wire")
{
    const Bme280Calibration cal = typicalCalibration();
    const Bme280Raw raw{kRawTemp, kRawPressure, kRawHumidity};
    const Bme280Compensated out = bme280Compensate(cal, raw);

    // 0.01 degC. Anything an indoor sensor could plausibly report.
    CHECK(out.tempCentiC > -4000);
    CHECK(out.tempCentiC < 8500);

    // Pa. Sea level is 101325; the part's own range is 300..1100 hPa.
    CHECK(out.pressurePa > 30000u);
    CHECK(out.pressurePa < 110000u);

    // 0.01 %RH, so never above 10000.
    CHECK(out.humidityCentiPct <= 10000u);
}

TEST_CASE("temperature is monotonic in the raw reading")
{
    const Bme280Calibration cal = typicalCalibration();
    int32_t previous = -2000000;
    for (int32_t raw = 300000; raw <= 700000; raw += 20000) {
        const Bme280Compensated out = bme280Compensate(cal, Bme280Raw{raw, kRawPressure, kRawHumidity});
        CAPTURE(raw);
        CHECK(out.tempCentiC > previous);
        previous = out.tempCentiC;
    }
}

TEST_CASE("a higher raw pressure reading means a lower pressure, as the part defines it")
{
    const Bme280Calibration cal = typicalCalibration();
    const uint32_t low = bme280Compensate(cal, Bme280Raw{kRawTemp, 400000, kRawHumidity}).pressurePa;
    const uint32_t high = bme280Compensate(cal, Bme280Raw{kRawTemp, 430000, kRawHumidity}).pressurePa;
    CHECK(low > high);
}

TEST_CASE("pressure depends on temperature -- t_fine is not decoration")
{
    const Bme280Calibration cal = typicalCalibration();
    const Bme280Compensated cold = bme280Compensate(cal, Bme280Raw{400000, kRawPressure, kRawHumidity});
    const Bme280Compensated warm = bme280Compensate(cal, Bme280Raw{600000, kRawPressure, kRawHumidity});

    CHECK(cold.tFine != warm.tFine);
    CHECK(cold.pressurePa != warm.pressurePa);
}

TEST_CASE("humidity is clamped at both ends, exactly where the datasheet clamps it")
{
    const Bme280Calibration cal = typicalCalibration();

    for (int32_t raw = 0; raw <= 65535; raw += 2047) {
        const Bme280Compensated out = bme280Compensate(cal, Bme280Raw{kRawTemp, kRawPressure, raw});
        CAPTURE(raw);
        CHECK(out.humidityCentiPct <= 10000u);
    }

    // The bottom of the range must not wrap round to something enormous.
    const Bme280Compensated dry = bme280Compensate(cal, Bme280Raw{kRawTemp, kRawPressure, 0});
    CHECK(dry.humidityCentiPct == 0u);
}

TEST_CASE("humidity keeps hundredths rather than rounding to whole percent")
{
    const Bme280Calibration cal = typicalCalibration();

    // Across a sweep, at least one reading must land off a multiple of 100 --
    // otherwise the Q22.10 conversion divided before it multiplied and threw the
    // hundredths away.
    bool sawFraction = false;
    for (int32_t raw = 20000; raw <= 45000; raw += 137) {
        const Bme280Compensated out = bme280Compensate(cal, Bme280Raw{kRawTemp, kRawPressure, raw});
        if (out.humidityCentiPct % 100u != 0u) {
            sawFraction = true;
            break;
        }
    }
    CHECK(sawFraction);
}

TEST_CASE("uncalibrated silicon reports no pressure instead of dividing by zero")
{
    Bme280Calibration cal = typicalCalibration();
    cal.digP1 = 0;
    const Bme280Compensated out = bme280Compensate(cal, Bme280Raw{kRawTemp, kRawPressure, kRawHumidity});
    CHECK(out.pressurePa == 0u);
}
