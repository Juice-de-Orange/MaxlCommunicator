/*
 * GNSS, behind an interface -- and the interface is shaped by the power budget.
 *
 * CLAUDE.md 1.5 is the whole design: "The L76K draws tens of mA when active
 * versus microamps for the sleeping MCU. GNSS is powered down by default and
 * only enabled for an explicit fix request or while the position screen is
 * open, with a hard timeout. Never leave it running 'just in case'."
 *
 * So there is no "read the position" call here. There is a request, a poll and
 * a timeout, because the module being off is the normal state and getting a fix
 * is an event with a beginning, an end and a cost. An interface that let a
 * caller ask for a position whenever it felt like one would make 1.5 impossible
 * to honour without every caller remembering to.
 *
 * docs/test-plan.md 1.7's second half is the one that matters: "power-down
 * confirmed to actually cut current". That needs a meter and is not something
 * this code can claim about itself.
 */

#ifndef MAXL_HAL_I_GNSS_H
#define MAXL_HAL_I_GNSS_H

#include "hal/nmea.h"

#include <stdint.h>

namespace hal {

enum class GnssState : uint8_t {
    Off = 0,      ///< powered down, the resting state
    Searching,    ///< powered, no usable fix yet
    Fixed,        ///< a fix is available
    TimedOut,     ///< the request expired without one; the module is off again
};

class IGnss {
public:
    virtual ~IGnss() = default;

    virtual bool begin() = 0;

    /// Power the module and start looking. `timeoutMs` is a hard bound: when it
    /// expires the module is powered down whether or not a fix arrived.
    virtual void requestFix(uint32_t timeoutMs, uint32_t nowMs) = 0;

    /// Power down now, fix or no fix.
    virtual void powerOff() = 0;

    /// Pump the UART and the timeout. Call from the main loop while not Off.
    virtual GnssState poll(uint32_t nowMs) = 0;

    virtual GnssState state() const = 0;

    /// The last fix seen. Its `hasPosition` is false until one arrives, and
    /// goes false again if the module reports that it has lost the fix.
    virtual const GnssFix &fix() const = 0;

    /// Milliseconds from requestFix() to the first usable fix. 0 if none came.
    /// docs/test-plan.md 1.7's first half.
    virtual uint32_t timeToFixMs() const = 0;
};

} // namespace hal

#endif // MAXL_HAL_I_GNSS_H
