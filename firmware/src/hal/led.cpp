#include "hal/led.h"

#include <Arduino.h>

namespace hal {

void StatusLed::begin()
{
    pinMode(PIN_LED1, OUTPUT);
    set(false);
}

void StatusLed::set(bool on)
{
    on_ = on;
    // LED_STATE_ON is 0 in variant.h: common anode, the pin sinks.
    digitalWrite(PIN_LED1, on ? LED_STATE_ON : !LED_STATE_ON);
}

void StatusLed::blink(uint16_t ms)
{
    set(true);
    delay(ms);
    set(false);
}

} // namespace hal
