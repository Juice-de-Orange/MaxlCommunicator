/*
 * IBattery on the nRF52840's SAADC, through the board's 2:1 divider.
 *
 * docs/hardware/pinmap.md: the battery sits on P0.04 (AIN2, Arduino A0) behind
 * an external 2:1 divider. Two settings decide what a count means and both are
 * set explicitly in begin() rather than inherited from whatever the core last
 * left behind: 12-bit resolution and the internal 0.6 V reference at gain 1/6,
 * which puts full scale at 3.6 V at the pin and therefore 7.2 V at the battery.
 * A LiPo never reaches that, so the top of the range is wasted -- and that is
 * the right trade, because the alternative reference options put 4.2 V outside
 * the range entirely and clip a full battery to "full enough".
 *
 * The conversion is a free function on purpose. It is the part that can be wrong
 * in a way nobody notices -- a factor of two reads perfectly plausibly -- and
 * keeping it out of the class means the host tests can drive it across the whole
 * range without a SAADC. What they cannot do is tell you the divider is really
 * 2:1 or that the reference is really 0.6 V; that is docs/test-plan.md gate 1.6,
 * against a multimeter at three points.
 */

#ifndef MAXL_HAL_BATTERY_ADC_H
#define MAXL_HAL_BATTERY_ADC_H

#include "hal/i_battery.h"

#include <stdint.h>

namespace hal {

/*
 * Full-scale input at the pin, in millivolts: the 0.6 V reference at gain 1/5,
 * which the core calls AR_INTERNAL_3_0.
 *
 * AR_INTERNAL (gain 1/6, 3.6 V) would also cover the range and leaves more
 * headroom. This is the one bring-up sketch 05 ran on node A, and both give the
 * same answer from the same pin voltage -- so the choice is made on evidence
 * rather than headroom. Even the worst case stays inside it: with USB at 5.25 V
 * the divider puts 2.6 V on the pin (decision D13), against 3.0 V of range.
 */
constexpr uint32_t kAdcFullScaleMv = 3000;

/// 12-bit conversion.
constexpr uint32_t kAdcCounts = 4096;

/// The board's external divider between battery and pin.
constexpr uint32_t kBatteryDividerNumerator = 2;

/// CLAUDE.md 2.1 flag bit 2. 3.4 V is where a LiPo's discharge curve turns
/// steep; below it the remaining capacity is small and falls away quickly.
constexpr uint16_t kLowBatteryMv = 3400;

/// One 12-bit count to battery millivolts. Rounds to nearest rather than
/// truncating -- over a 4096-count range truncation is a systematic 0.9 mV low,
/// and gate 1.6 allows only 50 mV in total.
constexpr uint16_t adcCountsToBatteryMv(uint16_t counts)
{
    const uint32_t numerator =
        static_cast<uint32_t>(counts) * kAdcFullScaleMv * kBatteryDividerNumerator;
    return static_cast<uint16_t>((numerator + kAdcCounts / 2) / kAdcCounts);
}

class BatteryAdc : public IBattery {
public:
    bool begin() override;
    uint16_t millivolts() override;
    bool low() override;

    /// The averaged raw count behind the last millivolts() call. Gate 1.6 wants
    /// the reading next to a multimeter, and the count is what makes a
    /// disagreement diagnosable rather than merely visible.
    uint16_t lastCounts() const { return lastCounts_; }

    /// Conversions averaged per reading. The SAADC's noise is a couple of counts
    /// and eight samples cost microseconds.
    static constexpr uint8_t kOversample = 8;

private:
    bool ready_ = false;
    uint16_t lastCounts_ = 0;
};

} // namespace hal

#endif // MAXL_HAL_BATTERY_ADC_H
