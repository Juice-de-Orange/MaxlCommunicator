/*
 * IClock on the PCF8563.
 *
 * CLAUDE.md 1.2 hangs the compliance mechanism on this: the duty cycle budget is
 * reconstructed against the RTC on boot, and "if the RTC time is not valid on
 * boot, the device starts fully transmit-blocked". timeValid() is that condition
 * and it is not cosmetic -- it is what decides whether the radio may be used at
 * all.
 *
 * Confirmed on node A, 2026-08-31: the chip answers on 0x51, keeps time across
 * NVIC_SystemReset to within 0.10 s, and reports its VL flag clear. The code
 * below is what bring-up sketch 03 ran, moved rather than rewritten.
 *
 * What is NOT settled: whether the chip has a supply of its own. PIN_PWR_ON low
 * does not drop the rail while USB is attached, because VBUS reaches the same
 * latch node through D5 -- so open decision D2 needs a run on battery alone. If
 * the answer is no, gate 2.8 has to split as D2 proposes.
 */

#ifndef MAXL_HAL_CLOCK_PCF8563_H
#define MAXL_HAL_CLOCK_PCF8563_H

#include "hal/i_clock.h"

#include <stdint.h>

namespace hal {

class Pcf8563Clock : public IClock {
public:
    /// Read the chip and establish whether its time can be trusted. Returns
    /// false if it does not answer at all.
    bool begin();

    uint32_t unixSeconds() const override;
    uint32_t monotonicMs() const override;
    bool timeValid() const override;

    /// SET_TIME, or a GNSS fix. Clears the chip's VL flag as a side effect of
    /// writing the seconds register, which is how the PCF8563 is told its time
    /// is trustworthy again.
    bool setUnixSeconds(uint32_t seconds);

    /// The chip's own voltage-low flag, read at begin(). Set means the supply
    /// dropped far enough that the chip distrusts what it is holding.
    bool voltageLowAtBoot() const { return voltageLowAtBoot_; }

    static constexpr uint8_t kAddress = 0x51;

private:
    bool present_ = false;
    bool voltageLowAtBoot_ = true;
    mutable bool trusted_ = false;
};

} // namespace hal

#endif // MAXL_HAL_CLOCK_PCF8563_H
