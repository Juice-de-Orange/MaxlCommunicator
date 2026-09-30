#include "hal/battery_adc.h"

#include <Arduino.h>

namespace hal {

bool BatteryAdc::begin()
{
    pinMode(PIN_A0, INPUT);

    // Both settings are what adcCountsToBatteryMv() assumes, and neither is the
    // core's default. AR_INTERNAL_3_0 is the 0.6 V band gap at gain 1/5, which
    // puts full scale at 3.0 V at the pin and 6.0 V behind the 2:1 divider.
    analogReference(AR_INTERNAL_3_0);
    analogReadResolution(12);

    // The first conversion after a reference change is not trustworthy -- the
    // SAADC's reference needs a moment to settle and the first sample is taken
    // against whatever it was before.
    (void)analogRead(PIN_A0);
    delay(2);

    ready_ = true;
    return true;
}

uint16_t BatteryAdc::millivolts()
{
    if (!ready_) {
        return 0;
    }

    uint32_t sum = 0;
    for (uint8_t i = 0; i < kOversample; ++i) {
        const int reading = analogRead(PIN_A0);
        if (reading < 0) {
            return 0;
        }
        sum += static_cast<uint32_t>(reading);
    }
    lastCounts_ = static_cast<uint16_t>((sum + kOversample / 2) / kOversample);
    return adcCountsToBatteryMv(lastCounts_);
}

bool BatteryAdc::low()
{
    const uint16_t mv = millivolts();
    // A failed read is not a low battery. Reporting LOW_BATT on every frame
    // because the ADC did not answer would be worse than saying nothing.
    return mv != 0 && mv < kLowBatteryMv;
}

} // namespace hal
