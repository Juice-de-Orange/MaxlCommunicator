/*
 * The panel, behind an interface.
 *
 * Screens render into a hal::Canvas and hand it here; this is the only place
 * that knows there is an SSD1681 on the other side. That seam is what makes
 * ui/ host-testable (see canvas.h) and it is the same seam CLAUDE.md 2.6 draws
 * for the radio and decision D3 draws for the block store.
 *
 * The refresh decision does NOT live here. RefreshPolicy makes it, the caller
 * passes the answer in, and the driver does as it is told -- so a test can drive
 * the policy through ten thousand updates without a panel, and the driver stays
 * small enough to be obviously correct.
 *
 * Timing is reported because docs/test-plan.md gate 1.1 is a number: twenty
 * consecutive partial refreshes, each under 400 ms. lastRefreshMs() is that
 * measurement, taken by the only code that knows when the panel actually went
 * busy and when it came back.
 */

#ifndef MAXL_HAL_I_DISPLAY_H
#define MAXL_HAL_I_DISPLAY_H

#include "hal/canvas.h"
#include "hal/refresh_policy.h"

#include <stdint.h>

namespace hal {

class IDisplay {
public:
    virtual ~IDisplay() = default;

    /// Power up the panel and establish a known state. False if it does not
    /// respond -- callers must cope: CLAUDE.md 1.7 requires the device to be
    /// fully configurable over BLE, so a dead panel is not a dead device.
    virtual bool begin() = 0;

    /*
     * Push the canvas. `kind` comes from RefreshPolicy; None is a no-op and is
     * accepted so callers do not have to branch.
     *
     * `region` is the part that actually changed, and it is not an optimisation
     * hint -- it is what makes docs/test-plan.md gate 1.1 reachable. Measured on
     * node A: a partial refresh of the whole panel takes 471 ms against the
     * gate's 400 ms; a small window takes 323 ms. An empty region means "the
     * whole panel", which is what a full refresh always uses.
     */
    virtual void present(const Canvas &canvas, RefreshKind kind, const Rect &region) = 0;

    /// Whole-panel convenience, for callers with nothing better to say.
    void present(const Canvas &canvas, RefreshKind kind)
    {
        present(canvas, kind, Rect{});
    }

    /// Put the panel into deep sleep. E-paper holds its image with no power, so
    /// this is the normal resting state, not an exceptional one (CLAUDE.md 3.1).
    virtual void sleep() = 0;

    /// Milliseconds the last present() took, measured across the busy wait.
    /// Zero before the first push. Gate 1.1 reads this.
    virtual uint32_t lastRefreshMs() const = 0;
};

} // namespace hal

#endif // MAXL_HAL_I_DISPLAY_H
