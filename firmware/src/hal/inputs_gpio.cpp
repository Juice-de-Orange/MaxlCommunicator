#include "hal/inputs_gpio.h"

#include <Arduino.h>

namespace hal {
namespace {

/// Samples averaged at begin() to establish the resting level. The pad's output
/// settles in well under a millisecond; this is generous on purpose.
constexpr uint8_t kRestingSamples = 16;

bool majorityLevel(uint8_t pin)
{
    uint8_t high = 0;
    for (uint8_t i = 0; i < kRestingSamples; ++i) {
        if (digitalRead(pin) == HIGH) {
            ++high;
        }
        delay(1);
    }
    return high > (kRestingSamples / 2);
}

} // namespace

bool GpioInputs::begin()
{
    pinMode(PIN_BUTTON1, INPUT_PULLUP);

    // No pull on the touch pad: the TTP223 drives its output actively, and a
    // pull-up would fight it in whichever direction turns out to be the active
    // one.
    pinMode(PIN_BUTTON_TOUCH, INPUT);

    buttonRestingLevel_ = majorityLevel(PIN_BUTTON1);
    touchRestingLevel_ = majorityLevel(PIN_BUTTON_TOUCH);

    return true;
}

InputEvent GpioInputs::poll(uint32_t nowMs)
{
    /*
     * One input per call, alternating. Both are sampled often enough at any
     * plausible loop rate, and returning at most one event per call keeps the
     * caller's dispatch trivial -- CLAUDE.md 3.2 has no chords, so there is
     * never a reason for two events to be meaningful together.
     */
    for (uint8_t attempt = 0; attempt < static_cast<uint8_t>(InputId::Count); ++attempt) {
        const uint8_t index = next_;
        next_ = static_cast<uint8_t>((next_ + 1) % static_cast<uint8_t>(InputId::Count));

        bool pressed = false;
        if (index == static_cast<uint8_t>(InputId::Touch)) {
            const bool level = digitalRead(PIN_BUTTON_TOUCH) == HIGH;
            pressed = touchActiveLow_ ? !level : level;
        } else {
            // Active low with a pull-up: pressed pulls the pin down.
            pressed = digitalRead(PIN_BUTTON1) == LOW;
        }

        const ButtonEvent event = detectors_[index].update(pressed, nowMs);
        if (event != ButtonEvent::None) {
            return InputEvent{static_cast<InputId>(index), event};
        }
    }
    return InputEvent{};
}

uint32_t GpioInputs::heldMs(InputId id, uint32_t nowMs) const
{
    return detectors_[static_cast<size_t>(id)].heldMs(nowMs);
}

uint32_t GpioInputs::pressCount(InputId id) const
{
    return detectors_[static_cast<size_t>(id)].pressCount();
}

uint32_t GpioInputs::bounceCount(InputId id) const
{
    return detectors_[static_cast<size_t>(id)].bounceCount();
}

} // namespace hal
