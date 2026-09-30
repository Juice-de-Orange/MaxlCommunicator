/*
 * The screen controller: which screen is showing, what a press means, and when
 * the panel is actually pushed.
 *
 * It owns two canvases. One is what the glass is showing, the other is what the
 * model would render to now; the difference between them is the entire refresh
 * decision (CLAUDE.md 1.6, "Redraw on state change only"). Ten kilobytes of RAM
 * for that, out of 248 -- and in exchange nothing anywhere else in the firmware
 * has to reason about whether a value it changed is visible.
 *
 * It does not act. A long press on POSITION returns Action::RequestGnssFix; it
 * does not power the GNSS, because ui/ may not see hal::IGnss's owner and should
 * not want to. app/ reads the action and decides. That keeps the button map --
 * the part that will be argued about -- in one file, and keeps the layer policy
 * intact while it is argued about.
 */

#ifndef MAXL_UI_UI_H
#define MAXL_UI_UI_H

#include "hal/canvas.h"
#include "hal/i_display.h"
#include "hal/i_inputs.h"
#include "hal/refresh_policy.h"
#include "ui/screens.h"
#include "ui/view_model.h"

#include <stdint.h>

namespace ui {

/// What the user asked for. app/ carries it out.
enum class Action : uint8_t {
    None = 0,
    ForceRefresh,     ///< button, short: sample sensors and open an RX window now
    PowerMenu,        ///< button, long
    MarkAllRead,      ///< touch, long, on MESSAGES
    SampleSensors,    ///< touch, long, on TELEMETRY
    RequestGnssFix,   ///< touch, long, on POSITION
    SendBeacon,       ///< touch, long, on PEERS
    ToggleFrontLight, ///< touch, long, on STATUS

    /*
     * Chosen from the power menu.
     *
     * They are returned rather than carried out, like every other action here.
     * What "sleep" means is the CLAUDE.md 3.1 state machine and that is phase 5;
     * what "shut down" means on this board is not settled either, because
     * driving PIN_PWR_ON low does not drop the rail while USB is attached
     * (decisions D2 and D13). The menu is finished; the two things it asks for
     * are not, and pretending otherwise in ui/ would hide that.
     */
    EnterSleep,
    Shutdown,
};

class Ui {
public:
    void begin(hal::IDisplay &display);

    /*
     * Feed one input event. Returns what the user asked for, and changes screen
     * on its own for the one case that is navigation rather than an action.
     *
     * CLAUDE.md 3.2 requires the long-press feedback to appear BEFORE the action
     * fires. That is why LongPressArmed and LongPress are separate events all
     * the way up from hal::PressDetector: Armed puts the progress overlay on
     * screen and returns None, and the action only comes back on release.
     */
    Action handle(const hal::InputEvent &event, uint32_t heldMs);

    /// Render the current screen and push it if it differs. Call every loop --
    /// it is cheap when nothing changed, which is nearly always.
    hal::RefreshKind render(const ViewModel &model);

    Screen screen() const { return screen_; }

    /// Whether the power menu is up. While it is, the inputs mean something
    /// different -- which is what modal means, and why the caller does not have
    /// to know.
    bool menuOpen() const { return menuOpen_; }
    PowerMenuItem menuSelection() const { return menuItem_; }

    /// Progress towards the long press in flight, for the overlay. Fed in by the
    /// caller because only it has a clock.
    void setHeld(uint32_t heldMs, uint32_t thresholdMs, const char *label);
    void clearHeld();

    const hal::RefreshPolicy &policy() const { return policy_; }

    /// What a long touch does on the screen that is showing. Public because the
    /// overlay label and the action have to agree, and there is exactly one
    /// place that decides.
    static Action primaryAction(Screen screen);
    static const char *primaryActionLabel(Screen screen);

private:
    hal::IDisplay *display_ = nullptr;
    hal::RefreshPolicy policy_;
    Screen screen_ = Screen::Status;
    bool screenJustChanged_ = true;

    bool menuOpen_ = false;
    PowerMenuItem menuItem_ = PowerMenuItem::Cancel;

    hal::Canvas shown_;
    hal::Canvas next_;

    uint32_t heldMs_ = 0;
    uint32_t heldThresholdMs_ = 0;
    const char *heldLabel_ = nullptr;
};

} // namespace ui

#endif // MAXL_UI_UI_H
