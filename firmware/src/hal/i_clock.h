/*
 * Clock abstraction.
 *
 * CLAUDE.md 1.2 hangs the whole compliance mechanism on this interface: the duty
 * cycle budget is a ring of timestamped records reconstructed against the RTC on
 * boot, and "if the RTC time is not valid on boot, the device starts fully
 * transmit-blocked". timeValid() is that condition, and it is the reason this is
 * an interface rather than a call to millis() -- the budget tests need to drive a
 * clock that starts invalid, jumps, and goes backwards.
 *
 * Two clocks, deliberately separate:
 *
 *   unixSeconds()  wall clock from the PCF8563. Can be invalid, can jump when
 *                  SET_TIME or a GNSS fix arrives. Budget records are keyed on it.
 *   monotonicMs()  since boot, never invalid, never jumps. Timers use it.
 *
 * Mixing them is how a clock correction turns into a missed retry, so the ARQ
 * scheduler uses monotonicMs() for its timer and unixSeconds() only where a
 * release time has to survive a reboot.
 */

#ifndef MAXL_HAL_I_CLOCK_H
#define MAXL_HAL_I_CLOCK_H

#include <stdint.h>

namespace hal {

class IClock {
public:
    virtual ~IClock() = default;

    /// Seconds since the Unix epoch. Only meaningful when timeValid() is true.
    virtual uint32_t unixSeconds() const = 0;

    /// Milliseconds since boot. Always meaningful. Wraps after ~49 days, so
    /// callers compare differences, never absolute values.
    virtual uint32_t monotonicMs() const = 0;

    /// Whether the wall clock has been established this power cycle, by RTC
    /// readback, SET_TIME or a GNSS fix. False means transmit-blocked.
    virtual bool timeValid() const = 0;
};

} // namespace hal

#endif // MAXL_HAL_I_CLOCK_H
