/*
 * A clock the test drives by hand.
 *
 * The budget is reconstructed against the RTC on boot (CLAUDE.md 1.2), so the
 * interesting cases are all clock cases: time invalid at boot, time arriving
 * later via SET_TIME, time jumping when a GNSS fix lands, time going backwards
 * because someone set it wrong. None of those are reachable with a real clock in
 * a unit test, which is the whole reason hal::IClock is an interface.
 */

#ifndef MAXL_TEST_FAKE_CLOCK_H
#define MAXL_TEST_FAKE_CLOCK_H

#include "hal/i_clock.h"

namespace fakes {

class FakeClock : public hal::IClock {
public:
    uint32_t unixSeconds() const override { return unix_; }
    uint32_t monotonicMs() const override { return monotonicMs_; }
    bool timeValid() const override { return valid_; }

    /// Establish wall clock time, as SET_TIME or a GNSS fix would.
    void setUnix(uint32_t seconds)
    {
        unix_ = seconds;
        valid_ = true;
    }

    /// Boot with no valid time -- the state CLAUDE.md 1.2 requires to block
    /// transmission entirely.
    void invalidate() { valid_ = false; }

    /// Advance both clocks together, the normal case.
    void advanceSeconds(uint32_t seconds)
    {
        unix_ += seconds;
        monotonicMs_ += seconds * 1000u;
    }

    void advanceMs(uint32_t ms)
    {
        monotonicMs_ += ms;
        unix_ += ms / 1000u;
    }

    /// Advance the monotonic clock only. Models a wall clock that has not been
    /// established yet while the device is otherwise running.
    void advanceMonotonicMs(uint32_t ms) { monotonicMs_ += ms; }

private:
    uint32_t unix_ = 0;
    uint32_t monotonicMs_ = 0;
    bool valid_ = false;
};

} // namespace fakes

#endif // MAXL_TEST_FAKE_CLOCK_H
