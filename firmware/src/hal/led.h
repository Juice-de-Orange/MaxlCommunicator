/*
 * The status LED. One pin, and choosing it took a schematic argument.
 *
 * docs/hardware/pinmap.md section 4: the T-Echo shipped in two revisions and
 * P1.01 and P1.03 are LEDs on one of them and ePaper-MISO and LoRa-DIO0 on the
 * other. Until the revision of this board is settled, driving either could put
 * an output onto a line something else is driving. P0.14 is the one pin that is
 * an LED on both, so it is the only one this project uses.
 *
 * Common anode to VDD_POWR: the pin sinks, so LOW is lit. variant.h says the
 * same thing as LED_STATE_ON.
 *
 * Bluefruit is separately prevented from claiming this pin -- it drives
 * LED_CONN on its own unless told not to, and LED_CONN and LED_BLUE both point
 * at P0.14 in variant.h. hal/ble_transport_bluefruit calls autoConnLed(false)
 * for that reason.
 */

#ifndef MAXL_HAL_LED_H
#define MAXL_HAL_LED_H

#include <stdint.h>

namespace hal {

class StatusLed {
public:
    void begin();
    void set(bool on);
    bool isOn() const { return on_; }

    /// One blink of `ms`, blocking. Only for bring-up and for the power-menu
    /// acknowledgement -- anything in the main loop uses set().
    void blink(uint16_t ms);

private:
    bool on_ = false;
};

} // namespace hal

#endif // MAXL_HAL_LED_H
