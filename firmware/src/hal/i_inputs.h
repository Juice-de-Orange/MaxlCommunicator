/*
 * The two usable inputs.
 *
 * CLAUDE.md 1.7 is explicit about how many there are and why: a capacitive touch
 * pad and one physical button, because "the second push button is hard-wired to
 * nRESET and is not available to the application". P0.18 does not appear
 * anywhere below this line, and it should not appear anywhere above it either.
 *
 * The same paragraph adds the rule that shapes every screen in ui/: "Button
 * interaction must never be required for correct operation -- all configuration
 * is possible over BLE from the PWA." A device whose touch pad has failed is
 * still a working device.
 */

#ifndef MAXL_HAL_I_INPUTS_H
#define MAXL_HAL_I_INPUTS_H

#include "hal/press_detector.h"

#include <stddef.h>
#include <stdint.h>

namespace hal {

/*
 * The long-press thresholds from CLAUDE.md 3.2.
 *
 * Here rather than in the GPIO driver because two layers need them and only one
 * of them may see a driver: ui/ draws the progress bar towards the threshold and
 * has to know where it is, and firmware/layering.toml lets ui/ include hal/ but
 * not hal/inputs_gpio.h's Arduino-facing world in particular. An interface
 * header is the one place both can look.
 */
inline constexpr uint16_t kTouchLongPressMs = 1000;
inline constexpr uint16_t kButtonLongPressMs = 2000;

/*
 * The double-press window on the push button (D17). A plausible default, not a
 * measured one -- gate 4.2's walkthrough is where it gets judged by a thumb.
 * The touch input keeps 0: no double-press state at all, per the original 3.2
 * reasoning about the pad.
 */
inline constexpr uint16_t kButtonDoubleGapMs = 400;

enum class InputId : uint8_t {
    Touch = 0, ///< TTP223 on P0.11
    Button,    ///< user button on P1.10
    Count,
};

struct InputEvent {
    InputId id = InputId::Touch;
    ButtonEvent event = ButtonEvent::None;
};

class IInputs {
public:
    virtual ~IInputs() = default;

    virtual bool begin() = 0;

    /// Sample both inputs and return the first event, if any. Called from the
    /// main loop; nothing here blocks.
    virtual InputEvent poll(uint32_t nowMs) = 0;

    /// How long the input has been held, for the long-press progress feedback
    /// CLAUDE.md 3.2 requires before the action fires.
    virtual uint32_t heldMs(InputId id, uint32_t nowMs) const = 0;

    /// Completed presses, per input. docs/test-plan.md gates 1.4 and 1.5.
    virtual uint32_t pressCount(InputId id) const = 0;
    virtual uint32_t bounceCount(InputId id) const = 0;
};

} // namespace hal

#endif // MAXL_HAL_I_INPUTS_H
