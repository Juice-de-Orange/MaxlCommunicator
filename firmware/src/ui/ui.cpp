#include "ui/ui.h"

namespace ui {

void Ui::begin(hal::IDisplay &display)
{
    display_ = &display;
    policy_.reset();
    screen_ = Screen::Status;
    screenJustChanged_ = true;
}

Action Ui::primaryAction(Screen screen)
{
    switch (screen) {
    case Screen::Status: return Action::ToggleFrontLight;
    case Screen::Messages: return Action::MarkAllRead;
    case Screen::Telemetry: return Action::SampleSensors;
    case Screen::Peers: return Action::SendBeacon;
    case Screen::Position: return Action::RequestGnssFix;
    case Screen::Count: break;
    }
    return Action::None;
}

const char *Ui::primaryActionLabel(Screen screen)
{
    switch (screen) {
    case Screen::Status: return "front light";
    case Screen::Messages: return "mark all read";
    case Screen::Telemetry: return "sample sensors";
    case Screen::Peers: return "send beacon";
    case Screen::Position: return "GNSS fix";
    case Screen::Count: break;
    }
    return "";
}

Action Ui::handle(const hal::InputEvent &event, uint32_t heldMs)
{
    /*
     * The menu is modal, so it takes the inputs before the screens see them.
     *
     * D17 put navigation on the button -- the touch pad does not respond
     * through this housing (touch_not_reachable, 2026-08-31) -- so the button
     * walks the items, a double press picks the one under the cursor, and a
     * long press closes the menu. Touch keeps its old map (walk / long-pick)
     * for a device where the pad works.
     */
    if (menuOpen_) {
        switch (event.event) {
        case hal::ButtonEvent::None:
            return Action::None;

        case hal::ButtonEvent::ShortPress:
            clearHeld();
            menuItem_ = nextMenuItem(menuItem_);
            return Action::None;

        case hal::ButtonEvent::DoublePress:
            if (event.id == hal::InputId::Touch) {
                return Action::None; // the pad never produces this (gap 0)
            }
            clearHeld();
            menuOpen_ = false;
            switch (menuItem_) {
            case PowerMenuItem::Sleep: return Action::EnterSleep;
            case PowerMenuItem::Shutdown: return Action::Shutdown;
            case PowerMenuItem::Cancel:
            case PowerMenuItem::Count: break;
            }
            return Action::None;

        case hal::ButtonEvent::LongPressArmed:
            setHeld(heldMs,
                    event.id == hal::InputId::Touch ? hal::kTouchLongPressMs
                                                    : hal::kButtonLongPressMs,
                    event.id == hal::InputId::Touch ? powerMenuLabel(menuItem_)
                                                    : "close menu");
            return Action::None;

        case hal::ButtonEvent::LongPress:
            clearHeld();
            if (event.id != hal::InputId::Touch) {
                menuOpen_ = false;
                return Action::None;
            }
            menuOpen_ = false;
            switch (menuItem_) {
            case PowerMenuItem::Sleep: return Action::EnterSleep;
            case PowerMenuItem::Shutdown: return Action::Shutdown;
            case PowerMenuItem::Cancel:
            case PowerMenuItem::Count: break;
            }
            return Action::None;
        }
        return Action::None;
    }

    switch (event.event) {
    case hal::ButtonEvent::None:
        return Action::None;

    case hal::ButtonEvent::ShortPress:
        clearHeld();
        /*
         * Navigation, not an action -- the controller owns which screen is
         * showing, so it does this itself. Since D17 BOTH inputs page through
         * the screens: the button because it is the input that works, the touch
         * because a screen change is also the wake-and-refresh the button's
         * short press used to provide, so nothing was lost in the swap.
         */
        screen_ = nextScreen(screen_);
        screenJustChanged_ = true;
        return Action::None;

    case hal::ButtonEvent::DoublePress:
        clearHeld();
        if (event.id == hal::InputId::Touch) {
            return Action::None; // the pad never produces this (gap 0)
        }
        // D17: the button's double press is what the touch long press was --
        // the screen's primary action.
        return primaryAction(screen_);

    case hal::ButtonEvent::LongPressArmed:
        // Feedback only. Nothing happens yet, and that is the point.
        setHeld(heldMs,
                event.id == hal::InputId::Touch ? hal::kTouchLongPressMs
                                                : hal::kButtonLongPressMs,
                event.id == hal::InputId::Touch ? primaryActionLabel(screen_) : "power menu");
        return Action::None;

    case hal::ButtonEvent::LongPress:
        clearHeld();
        if (event.id == hal::InputId::Touch) {
            return primaryAction(screen_);
        }
        // Opening the menu is navigation, not an action, so the controller does
        // it here -- exactly as it does for the screen cycle. Cancel starts
        // under the cursor.
        menuOpen_ = true;
        menuItem_ = PowerMenuItem::Cancel;
        return Action::PowerMenu;
    }
    return Action::None;
}

void Ui::setHeld(uint32_t heldMs, uint32_t thresholdMs, const char *label)
{
    heldMs_ = heldMs;
    heldThresholdMs_ = thresholdMs;
    heldLabel_ = label;
}

void Ui::clearHeld()
{
    heldMs_ = 0;
    heldThresholdMs_ = 0;
    heldLabel_ = nullptr;
}

hal::RefreshKind Ui::render(const ViewModel &model)
{
    renderScreen(screen_, model, next_);
    if (menuOpen_) {
        renderPowerMenu(next_, menuItem_);
    }
    if (heldThresholdMs_ > 0) {
        renderLongPressFeedback(next_, heldMs_, heldThresholdMs_, heldLabel_);
    }

    // One pass over the buffers answers both questions at once: whether anything
    // changed, and where. Asking sameAs() and then diffBounds() would walk 5000
    // bytes twice for the same answer.
    hal::Rect changed;
    const bool contentChanged = next_.diffBounds(shown_, changed);
    const hal::RefreshKind kind = policy_.decide(contentChanged, screenJustChanged_);
    screenJustChanged_ = false;

    if (kind == hal::RefreshKind::None) {
        return kind;
    }

    if (display_ != nullptr) {
        // A full refresh always takes the whole panel; a partial takes only what
        // moved, which is what keeps it inside gate 1.1's 400 ms.
        display_->present(next_, kind, kind == hal::RefreshKind::Full ? hal::Rect{} : changed);
    }
    shown_.copyFrom(next_);
    return kind;
}

} // namespace ui
