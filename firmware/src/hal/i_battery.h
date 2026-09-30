/*
 * Battery voltage.
 *
 * CLAUDE.md 2.2 puts `battery uint16 (mV)` in the TELEMETRY payload and 2.1
 * gives frame flag bit 2 to LOW_BATT, so this is not only a number for the
 * STATUS screen -- it is a bit that travels on every frame. Both come from here.
 */

#ifndef MAXL_HAL_I_BATTERY_H
#define MAXL_HAL_I_BATTERY_H

#include <stdint.h>

namespace hal {

class IBattery {
public:
    virtual ~IBattery() = default;

    virtual bool begin() = 0;

    /// Battery terminal voltage in millivolts, 0 if it could not be read.
    virtual uint16_t millivolts() = 0;

    /// Whether the LOW_BATT flag should be set on outgoing frames.
    virtual bool low() = 0;
};

} // namespace hal

#endif // MAXL_HAL_I_BATTERY_H
