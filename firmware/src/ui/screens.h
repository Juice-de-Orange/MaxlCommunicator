/*
 * The five screens, as pure functions from a view model to a canvas.
 *
 * CLAUDE.md 3.3 fixes the set and the order:
 *
 *   STATUS -> MESSAGES -> TELEMETRY -> PEERS -> POSITION
 *
 * and the touch button walks the cycle (3.2). Each function below clears the
 * canvas and draws the whole screen; there is no incremental drawing anywhere in
 * ui/. That sounds wasteful and is not: the panel is pushed by comparing the
 * finished canvas against the last one presented, so redrawing everything into
 * RAM costs microseconds and is what makes "did anything change?" a question
 * with an exact answer. Incremental drawing would make it a question about
 * whoever wrote the last screen.
 *
 * Nothing here reads a clock, a sensor or a radio. Everything comes from the
 * ViewModel, which is the property docs/test-plan.md gate 4.1 rests on.
 */

#ifndef MAXL_UI_SCREENS_H
#define MAXL_UI_SCREENS_H

#include "hal/canvas.h"
#include "ui/view_model.h"

#include <stdint.h>

namespace ui {

enum class Screen : uint8_t {
    Status = 0,
    Messages,
    Telemetry,
    Peers,
    Position,
    Count,
};

/// Next screen in the cycle. CLAUDE.md 3.2: a short touch moves on, and there is
/// no way back -- five screens is short enough that going round is faster than
/// remembering which button reverses.
Screen nextScreen(Screen current);

const char *screenTitle(Screen screen);

/// Draw `screen` into `canvas`, clearing it first.
void renderScreen(Screen screen, const ViewModel &model, hal::Canvas &canvas);

/*
 * Overlay the long-press progress indicator.
 *
 * CLAUDE.md 3.2: "Long-press must give visual feedback before the action fires."
 * Drawn on top of whatever screen is showing, so the feedback does not depend on
 * the screen having remembered to make room for it. `heldMs` and `thresholdMs`
 * come from hal::PressDetector; at heldMs >= thresholdMs the bar is full and the
 * action is about to happen on release.
 */
void renderLongPressFeedback(hal::Canvas &canvas, uint32_t heldMs, uint32_t thresholdMs,
                             const char *label);

/// What the power menu offers. CLAUDE.md 3.2: "Button, long (> 2 s) -> Power
/// menu (sleep / shutdown)". Cancel is first, and deliberately: a menu reached
/// by holding a button for two seconds is reached by accident often enough that
/// the harmless option should be the one already under the cursor.
enum class PowerMenuItem : uint8_t {
    Cancel = 0,
    Sleep,
    Shutdown,
    Count,
};

PowerMenuItem nextMenuItem(PowerMenuItem current);
const char *powerMenuLabel(PowerMenuItem item);

/*
 * Draw the power menu over whatever screen is showing.
 *
 * An overlay rather than a sixth screen: it is modal, it is reached from
 * anywhere, and it must not disturb where the user was. CLAUDE.md 3.3 fixes the
 * screen set at five and this is not a sixth one.
 */
void renderPowerMenu(hal::Canvas &canvas, PowerMenuItem selected);

} // namespace ui

#endif // MAXL_UI_SCREENS_H
