/*
 * The screen controller: what a press means, and when the panel is pushed.
 *
 * CLAUDE.md 3.2 is four lines of table and every one of them is a decision that
 * can be got backwards. It also carries the one rule that is about ordering
 * rather than about mapping -- "long-press must give visual feedback before the
 * action fires" -- and that is checked here as an ordering, because on e-paper
 * a 300 ms refresh after the fact is feedback about something that already
 * happened.
 */

#include "doctest.h"

#include "fakes/fake_display.h"
#include "ui/ui.h"

using namespace ui;

namespace {

hal::InputEvent touch(hal::ButtonEvent event)
{
    return hal::InputEvent{hal::InputId::Touch, event};
}

hal::InputEvent button(hal::ButtonEvent event)
{
    return hal::InputEvent{hal::InputId::Button, event};
}

struct Harness {
    fakes::FakeDisplay display;
    Ui ui;
    ViewModel model;

    Harness()
    {
        ui.begin(display);
        model.timeValid = true;
        model.budgetTotalS = 360;
        model.budgetRemainingS = 360;
        model.batteryMv = 4000;
    }
};

} // namespace

TEST_CASE("a short touch walks the screen cycle and asks for nothing")
{
    Harness h;
    CHECK(h.ui.screen() == Screen::Status);

    CHECK(h.ui.handle(touch(hal::ButtonEvent::ShortPress), 0) == Action::None);
    CHECK(h.ui.screen() == Screen::Messages);

    for (int i = 0; i < 4; ++i) {
        h.ui.handle(touch(hal::ButtonEvent::ShortPress), 0);
    }
    CHECK(h.ui.screen() == Screen::Status);
}

TEST_CASE("D17 -- the button navigates: short pages, double is the screen's action")
{
    /*
     * Decision D17 (2026-08-31): the touch pad does not respond through this
     * housing, so the button takes over navigation. Its old wake/refresh role
     * is not lost -- a screen change redraws, which is the refresh.
     */
    Harness h;
    CHECK(h.ui.handle(button(hal::ButtonEvent::ShortPress), 0) == Action::None);
    CHECK(h.ui.screen() == Screen::Messages);

    CHECK(h.ui.handle(button(hal::ButtonEvent::DoublePress), 0) == Action::MarkAllRead);
    CHECK(h.ui.screen() == Screen::Messages);

    for (int i = 0; i < 4; ++i) {
        h.ui.handle(button(hal::ButtonEvent::ShortPress), 0);
    }
    CHECK(h.ui.screen() == Screen::Status);
}

TEST_CASE("a long touch does what the screen it is on does")
{
    Harness h;
    CHECK(h.ui.handle(touch(hal::ButtonEvent::LongPress), 1200) == Action::ToggleFrontLight);

    h.ui.handle(touch(hal::ButtonEvent::ShortPress), 0);
    CHECK(h.ui.screen() == Screen::Messages);
    CHECK(h.ui.handle(touch(hal::ButtonEvent::LongPress), 1200) == Action::MarkAllRead);

    h.ui.handle(touch(hal::ButtonEvent::ShortPress), 0);
    CHECK(h.ui.handle(touch(hal::ButtonEvent::LongPress), 1200) == Action::SampleSensors);

    h.ui.handle(touch(hal::ButtonEvent::ShortPress), 0);
    CHECK(h.ui.handle(touch(hal::ButtonEvent::LongPress), 1200) == Action::SendBeacon);

    h.ui.handle(touch(hal::ButtonEvent::ShortPress), 0);
    CHECK(h.ui.screen() == Screen::Position);
    CHECK(h.ui.handle(touch(hal::ButtonEvent::LongPress), 1200) == Action::RequestGnssFix);
}

TEST_CASE("feedback is drawn before the action fires, and nothing happens on the way")
{
    Harness h;

    // Armed: the screen must already show it, and nothing may have happened.
    CHECK(h.ui.handle(touch(hal::ButtonEvent::LongPressArmed), 1000) == Action::None);
    h.ui.render(h.model);
    const size_t pushesWithFeedback = h.display.pushes.size();
    CHECK(pushesWithFeedback > 0);

    // Only on release does the action come back.
    CHECK(h.ui.handle(touch(hal::ButtonEvent::LongPress), 1200) == Action::ToggleFrontLight);
}

TEST_CASE("a long button press opens the power menu with cancel under the cursor")
{
    Harness h;
    CHECK(h.ui.handle(button(hal::ButtonEvent::LongPress), 2200) == Action::PowerMenu);
    CHECK(h.ui.menuOpen());
    // Reached by holding a button for two seconds, which happens by accident.
    CHECK(h.ui.menuSelection() == PowerMenuItem::Cancel);
}

TEST_CASE("the menu is modal: touch walks it, and the screen behind does not move")
{
    Harness h;
    h.ui.handle(button(hal::ButtonEvent::LongPress), 2200);
    const Screen behind = h.ui.screen();

    CHECK(h.ui.handle(touch(hal::ButtonEvent::ShortPress), 0) == Action::None);
    CHECK(h.ui.menuSelection() == PowerMenuItem::Sleep);
    CHECK(h.ui.screen() == behind);

    h.ui.handle(touch(hal::ButtonEvent::ShortPress), 0);
    CHECK(h.ui.menuSelection() == PowerMenuItem::Shutdown);

    h.ui.handle(touch(hal::ButtonEvent::ShortPress), 0);
    CHECK(h.ui.menuSelection() == PowerMenuItem::Cancel);
}

TEST_CASE("picking cancel closes the menu and asks for nothing")
{
    Harness h;
    h.ui.handle(button(hal::ButtonEvent::LongPress), 2200);
    REQUIRE(h.ui.menuSelection() == PowerMenuItem::Cancel);

    CHECK(h.ui.handle(touch(hal::ButtonEvent::LongPress), 1200) == Action::None);
    CHECK_FALSE(h.ui.menuOpen());
}

TEST_CASE("picking sleep or shutdown returns it once and closes the menu")
{
    Harness h;
    h.ui.handle(button(hal::ButtonEvent::LongPress), 2200);
    h.ui.handle(touch(hal::ButtonEvent::ShortPress), 0);
    CHECK(h.ui.handle(touch(hal::ButtonEvent::LongPress), 1200) == Action::EnterSleep);
    CHECK_FALSE(h.ui.menuOpen());

    h.ui.handle(button(hal::ButtonEvent::LongPress), 2200);
    h.ui.handle(touch(hal::ButtonEvent::ShortPress), 0);
    h.ui.handle(touch(hal::ButtonEvent::ShortPress), 0);
    CHECK(h.ui.handle(touch(hal::ButtonEvent::LongPress), 1200) == Action::Shutdown);
    CHECK_FALSE(h.ui.menuOpen());
}

TEST_CASE("a long button press closes the menu, which is the way out that needs no aim")
{
    // D17 moved walking onto the button's short press, so the blind exit moved
    // to the long press -- the same gesture that opened it.
    Harness h;
    h.ui.handle(button(hal::ButtonEvent::LongPress), 2200);
    h.ui.handle(button(hal::ButtonEvent::ShortPress), 0);
    REQUIRE(h.ui.menuSelection() == PowerMenuItem::Sleep);

    CHECK(h.ui.handle(button(hal::ButtonEvent::LongPress), 2200) == Action::None);
    CHECK_FALSE(h.ui.menuOpen());
}

TEST_CASE("D17 -- the button drives the whole menu: walk, double-press to pick")
{
    Harness h;
    h.ui.handle(button(hal::ButtonEvent::LongPress), 2200);
    REQUIRE(h.ui.menuOpen());
    REQUIRE(h.ui.menuSelection() == PowerMenuItem::Cancel);

    h.ui.handle(button(hal::ButtonEvent::ShortPress), 0);
    REQUIRE(h.ui.menuSelection() == PowerMenuItem::Sleep);
    CHECK(h.ui.handle(button(hal::ButtonEvent::DoublePress), 0) == Action::EnterSleep);
    CHECK_FALSE(h.ui.menuOpen());
}

TEST_CASE("the menu is visible on the panel while it is open")
{
    Harness h;
    h.ui.render(h.model);
    const size_t before = h.display.pushes.size();

    h.ui.handle(button(hal::ButtonEvent::LongPress), 2200);
    h.ui.render(h.model);
    CHECK(h.display.pushes.size() > before);

    // And it goes away again.
    h.ui.handle(button(hal::ButtonEvent::ShortPress), 0);
    const size_t withMenu = h.display.pushes.size();
    h.ui.render(h.model);
    CHECK(h.display.pushes.size() > withMenu);
}

TEST_CASE("the first push is full and takes the whole panel")
{
    Harness h;
    h.ui.render(h.model);
    REQUIRE(h.display.pushes.size() == 1);
    CHECK(h.display.pushes[0].kind == hal::RefreshKind::Full);
    CHECK(h.display.pushes[0].region.empty());
}

TEST_CASE("a partial takes only what moved -- which is what gate 1.1 turns on")
{
    Harness h;
    h.ui.render(h.model);

    // One value, on one line.
    h.model.budgetRemainingS = 300;
    h.ui.render(h.model);

    REQUIRE(h.display.pushes.size() == 2);
    CHECK(h.display.pushes[1].kind == hal::RefreshKind::Partial);
    CHECK_FALSE(h.display.pushes[1].region.empty());
    // The whole panel would be 471 ms against the gate's 400. A single line is
    // nothing like the whole panel.
    CHECK(h.display.pushes[1].region.h < 40);
    CHECK(h.display.pushes[1].region.w < hal::Canvas::kWidth);
}

TEST_CASE("nothing changed means nothing is pushed at all")
{
    Harness h;
    h.ui.render(h.model);
    const size_t after = h.display.pushes.size();

    for (int i = 0; i < 1000; ++i) {
        CHECK(h.ui.render(h.model) == hal::RefreshKind::None);
    }
    CHECK(h.display.pushes.size() == after);
}

TEST_CASE("a screen change is always full, and always the whole panel")
{
    Harness h;
    h.ui.render(h.model);

    h.ui.handle(touch(hal::ButtonEvent::ShortPress), 0);
    CHECK(h.ui.render(h.model) == hal::RefreshKind::Full);

    REQUIRE(h.display.pushes.size() == 2);
    CHECK(h.display.pushes[1].region.empty());
}

TEST_CASE("the ghosting counter still applies through the controller")
{
    Harness h;
    h.ui.render(h.model);

    for (uint16_t i = 0; i < hal::RefreshPolicy::kPartialsBeforeFull; ++i) {
        h.model.budgetRemainingS = static_cast<uint16_t>(300 - i);
        CAPTURE(i);
        REQUIRE(h.ui.render(h.model) == hal::RefreshKind::Partial);
    }

    h.model.budgetRemainingS = 100;
    CHECK(h.ui.render(h.model) == hal::RefreshKind::Full);
}
