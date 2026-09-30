/*
 * IInputs on the two GPIOs, with PressDetector doing the thinking.
 *
 * The button is settled: P1.10, active low with a pull-up, per the vendor header
 * and the schematic and every other source.
 *
 * The touch pad is not. docs/hardware/pinmap.md records the disagreement in
 * full -- LilyGO's header says active high, Meshtastic sets ACTIVE_LOW true
 * while its own comment beside it says active high. Nobody has measured it on
 * this board.
 *
 * So the polarity is a setting with a documented default rather than a constant,
 * begin() records the level the pad rests at with nobody near it, and
 * PressDetector refuses to report a release it never saw begin. Between the
 * three, getting it backwards produces a pad that does nothing -- which is
 * visible and harmless -- instead of a device that changes screen whenever a
 * hand comes near it. Bring-up sketch 09 reports the resting level and settles
 * it; until it has, the vendor header's reading is what ships.
 */

#ifndef MAXL_HAL_INPUTS_GPIO_H
#define MAXL_HAL_INPUTS_GPIO_H

#include "hal/i_inputs.h"

#include <stddef.h>
#include <stdint.h>

namespace hal {

class GpioInputs : public IInputs {
public:
    GpioInputs()
        : detectors_{PressDetector(kTouchLongPressMs),
                     PressDetector(kButtonLongPressMs, PressDetector::kDefaultDebounceMs,
                                   kButtonDoubleGapMs)}
    {
    }

    bool begin() override;
    InputEvent poll(uint32_t nowMs) override;
    uint32_t heldMs(InputId id, uint32_t nowMs) const override;
    uint32_t pressCount(InputId id) const override;
    uint32_t bounceCount(InputId id) const override;

    /// Default false: the vendor header's reading. Call before begin().
    void setTouchActiveLow(bool activeLow) { touchActiveLow_ = activeLow; }

    /// The raw level each pin rested at during begin(), with nobody touching
    /// anything. This is the measurement that decides the setting above.
    bool touchRestingLevel() const { return touchRestingLevel_; }
    bool buttonRestingLevel() const { return buttonRestingLevel_; }

private:
    PressDetector detectors_[static_cast<size_t>(InputId::Count)];
    bool touchActiveLow_ = false;
    bool touchRestingLevel_ = false;
    bool buttonRestingLevel_ = true;
    uint8_t next_ = 0; ///< round-robin, so neither input can starve the other
};

} // namespace hal

#endif // MAXL_HAL_INPUTS_GPIO_H
