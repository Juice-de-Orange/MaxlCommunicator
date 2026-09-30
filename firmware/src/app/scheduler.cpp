#include "scheduler.h"

namespace app {

void Scheduler::start(uint32_t nowMs)
{
    for (Entry &entry : tasks_) {
        if (entry.enabled) {
            entry.nextAtMs = nowMs + entry.intervalS * 1000u;
        }
    }
}

void Scheduler::setInterval(Task task, uint32_t intervalS, uint32_t nowMs)
{
    Entry &entry = tasks_[static_cast<size_t>(task)];
    entry.intervalS = intervalS;
    entry.enabled = intervalS > 0;
    entry.nextAtMs = nowMs + intervalS * 1000u;
}

bool Scheduler::due(Task task, uint32_t nowMs)
{
    Entry &entry = tasks_[static_cast<size_t>(task)];
    if (!entry.enabled || !reached(nowMs, entry.nextAtMs)) {
        return false;
    }

    /*
     * Advance the phase, do not restart the interval.
     *
     * nextAt += interval keeps the schedule anchored to where it started, so a
     * firing that is late by 300 ms does not push every later firing 300 ms
     * further out. Over the 36 firings gate 3.4 measures, restarting from `now`
     * accumulates every delay in the loop; this does not accumulate anything.
     */
    const uint32_t step = entry.intervalS * 1000u;
    entry.nextAtMs += step;

    /*
     * Unless it has fallen a whole interval behind -- after a long sleep, or a
     * debugger, or an e-paper full refresh that took two seconds while the
     * interval was one. Catching up by firing repeatedly would spend the duty
     * cycle budget on backlog, which is exactly what CLAUDE.md 1.2 exists to
     * prevent. One firing, then re-phase from now.
     */
    if (reached(nowMs, entry.nextAtMs)) {
        entry.nextAtMs = nowMs + step;
    }

    ++entry.firings;
    return true;
}

uint32_t Scheduler::untilNext(uint32_t nowMs, uint32_t cap) const
{
    uint32_t soonest = cap;
    for (const Entry &entry : tasks_) {
        if (!entry.enabled) {
            continue;
        }
        if (reached(nowMs, entry.nextAtMs)) {
            return 0;
        }
        const uint32_t remaining = entry.nextAtMs - nowMs;
        if (remaining < soonest) {
            soonest = remaining;
        }
    }
    return soonest;
}

} // namespace app
