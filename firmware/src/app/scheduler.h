/*
 * When the periodic things happen.
 *
 * Gate 3.4: "Sensor scheduling -- telemetry interval honoured +/-10 % over 6 h."
 * Six hours at a 10-minute interval is 36 firings, and a scheduler that drifts a
 * few seconds per firing is well outside 10 % by the end. So each task keeps its
 * *phase* rather than restarting its interval from the moment it happened to
 * run: due-time advances by the interval, not by "now plus the interval".
 *
 * Everything here runs on the monotonic clock, never on the wall clock
 * (hal/i_clock.h): SET_TIME or a GNSS fix can move the wall clock by hours, and
 * a telemetry sample that fires because somebody corrected the date is a bug
 * that would look like a radio problem in the power measurements.
 *
 * Intervals are configurable over BLE (docs/bridge-protocol.md, Config TLVs
 * 0x07 telemetryIntervalS and 0x08 beaconIntervalS), so the setters accept a
 * change at any time and re-phase from the change rather than from boot.
 */

#ifndef MAXL_APP_SCHEDULER_H
#define MAXL_APP_SCHEDULER_H

#include <stddef.h>
#include <stdint.h>

namespace app {

enum class Task : uint8_t {
    Telemetry = 0,
    Beacon,
    TaskCount,
};

inline constexpr size_t kTaskCount = static_cast<size_t>(Task::TaskCount);

class Scheduler {
public:
    /// `nowMs` is the monotonic clock at construction, so the first firing is a
    /// full interval away rather than immediately at boot.
    void start(uint32_t nowMs);

    /// 0 disables the task. TLV 0x07 documents 0 as "off" for telemetry.
    void setInterval(Task task, uint32_t intervalS, uint32_t nowMs);
    uint32_t interval(Task task) const { return tasks_[static_cast<size_t>(task)].intervalS; }

    /// Whether the task is due, and if so consume the firing. Call from the main
    /// loop; it is edge-triggered and never fires twice for one due-time.
    bool due(Task task, uint32_t nowMs);

    /// Milliseconds until the next firing of any enabled task, capped at `cap`.
    /// This is what the sleep decision is built on -- CLAUDE.md 3.1 wants System-ON
    /// sleep with an RTC wakeup, not a poll loop.
    uint32_t untilNext(uint32_t nowMs, uint32_t cap) const;

    /// How many times the task has fired. The gate counts these.
    uint32_t firings(Task task) const { return tasks_[static_cast<size_t>(task)].firings; }

private:
    struct Entry {
        uint32_t intervalS = 0;
        uint32_t nextAtMs = 0;
        uint32_t firings = 0;
        bool enabled = false;
    };
    Entry tasks_[kTaskCount];

    /*
     * Unsigned wraparound, on purpose.
     *
     * monotonicMs() wraps after about 49 days, and a node is meant to run for a
     * fortnight on a charge -- so the wrap is not hypothetical, it is what
     * happens to a device left on a shelf. Comparing (now - nextAt) as a signed
     * difference keeps working across it; comparing now >= nextAt does not, and
     * fails by never firing again.
     */
    static bool reached(uint32_t nowMs, uint32_t whenMs)
    {
        return static_cast<int32_t>(nowMs - whenMs) >= 0;
    }
};

} // namespace app

#endif // MAXL_APP_SCHEDULER_H
