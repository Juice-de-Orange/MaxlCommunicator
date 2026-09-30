/*
 * Debounce and press classification.
 *
 * docs/test-plan.md 1.4 wants 500 deliberate presses to produce exactly 500
 * events with "zero doubles, zero misses", and 1.5 wants the touch pad's
 * false-trigger rate recorded. Both are hardware gates and neither can be
 * settled here -- what can be settled is that a clean press produces exactly one
 * event and a bouncing edge produces exactly one too, which is the firmware's
 * half of the bargain.
 *
 * CLAUDE.md 3.2: "Long-press must give visual feedback before the action fires."
 * That is the LongPressArmed / LongPress pair, and the ordering test below is
 * the whole reason the pair exists.
 */

#include "doctest.h"

#include "hal/press_detector.h"

using namespace hal;

namespace {

constexpr uint16_t kTouchLongMs = 1000;  // CLAUDE.md 3.2
constexpr uint16_t kButtonLongMs = 2000;

/// Hold a level for `ms`, sampling every millisecond, and count what came out.
struct Counts {
    uint32_t shortPress = 0;
    uint32_t armed = 0;
    uint32_t longPress = 0;
    uint32_t doublePress = 0;
};

void hold(PressDetector &d, bool level, uint32_t ms, uint32_t &now, Counts &counts)
{
    for (uint32_t i = 0; i < ms; ++i) {
        switch (d.update(level, now)) {
        case ButtonEvent::ShortPress: ++counts.shortPress; break;
        case ButtonEvent::LongPressArmed: ++counts.armed; break;
        case ButtonEvent::LongPress: ++counts.longPress; break;
        case ButtonEvent::DoublePress: ++counts.doublePress; break;
        case ButtonEvent::None: break;
        }
        ++now;
    }
}

} // namespace

TEST_CASE("a clean press produces exactly one event")
{
    PressDetector detector(kTouchLongMs);
    uint32_t now = 0;
    Counts counts;

    hold(detector, false, 100, now, counts);
    hold(detector, true, 200, now, counts);
    hold(detector, false, 100, now, counts);

    CHECK(counts.shortPress == 1);
    CHECK(counts.armed == 0);
    CHECK(counts.longPress == 0);
    CHECK(detector.pressCount() == 1);
}

TEST_CASE("500 presses give 500 events -- gate 1.4's arithmetic")
{
    PressDetector detector(kButtonLongMs);
    uint32_t now = 0;
    Counts counts;
    hold(detector, false, 50, now, counts);

    for (int i = 0; i < 500; ++i) {
        hold(detector, true, 120, now, counts);
        hold(detector, false, 120, now, counts);
    }

    CHECK(counts.shortPress == 500);
    CHECK(counts.longPress == 0);
    CHECK(detector.pressCount() == 500);
}

TEST_CASE("a bouncing contact is one press, and the bounce is counted")
{
    PressDetector detector(kTouchLongMs);
    uint32_t now = 0;
    Counts counts;
    hold(detector, false, 50, now, counts);

    // Six fast transitions, none of them lasting the debounce window, then a
    // press that does.
    for (int i = 0; i < 3; ++i) {
        hold(detector, true, 5, now, counts);
        hold(detector, false, 5, now, counts);
    }
    hold(detector, true, 200, now, counts);
    hold(detector, false, 100, now, counts);

    CHECK(counts.shortPress == 1);
    CHECK(detector.pressCount() == 1);
    CHECK(detector.bounceCount() > 0);
}

TEST_CASE("feedback is armed before the action fires, never after")
{
    PressDetector detector(kTouchLongMs);
    uint32_t now = 0;
    Counts counts;
    hold(detector, false, 50, now, counts);

    // Held past the threshold but not yet released: the screen must already have
    // been told, and nothing must have happened yet.
    hold(detector, true, kTouchLongMs + 100, now, counts);
    CHECK(counts.armed == 1);
    CHECK(counts.longPress == 0);
    CHECK(counts.shortPress == 0);

    hold(detector, false, 100, now, counts);
    CHECK(counts.longPress == 1);
    CHECK(counts.shortPress == 0);
}

TEST_CASE("just under the threshold is still a short press")
{
    PressDetector detector(kTouchLongMs);
    uint32_t now = 0;
    Counts counts;
    hold(detector, false, 50, now, counts);

    hold(detector, true, kTouchLongMs - 60, now, counts);
    hold(detector, false, 100, now, counts);

    CHECK(counts.shortPress == 1);
    CHECK(counts.armed == 0);
    CHECK(counts.longPress == 0);
}

TEST_CASE("the two inputs have different thresholds and do not borrow each other's")
{
    PressDetector touch(kTouchLongMs);
    PressDetector button(kButtonLongMs);
    uint32_t now = 0;
    Counts touchCounts;
    Counts buttonCounts;

    hold(touch, false, 50, now, touchCounts);
    uint32_t buttonNow = 0;
    hold(button, false, 50, buttonNow, buttonCounts);

    // 1.5 s: long for the touch pad, short for the button.
    hold(touch, true, 1500, now, touchCounts);
    hold(touch, false, 50, now, touchCounts);
    hold(button, true, 1500, buttonNow, buttonCounts);
    hold(button, false, 50, buttonNow, buttonCounts);

    CHECK(touchCounts.longPress == 1);
    CHECK(buttonCounts.shortPress == 1);
    CHECK(buttonCounts.armed == 0);
}

TEST_CASE("a finger already on the pad at boot is not a press")
{
    PressDetector detector(kTouchLongMs);
    uint32_t now = 0;
    Counts counts;

    hold(detector, true, 500, now, counts);
    CHECK(counts.shortPress == 0);
    CHECK(detector.pressCount() == 0);

    hold(detector, false, 100, now, counts);
    CHECK(counts.shortPress == 0);
    CHECK(detector.pressCount() == 0);
}

TEST_CASE("the 49-day millis() wraparound passes without a spurious event")
{
    PressDetector detector(kButtonLongMs);
    Counts counts;

    // Start just under the wrap and walk straight through it.
    uint32_t now = 0xFFFFFF00u;
    hold(detector, false, 100, now, counts);
    hold(detector, true, 300, now, counts);
    hold(detector, false, 100, now, counts);

    CHECK(counts.shortPress == 1);
    CHECK(counts.longPress == 0);
    CHECK(detector.pressCount() == 1);
}

TEST_CASE("heldMs reports the press in progress, for the long-press progress bar")
{
    PressDetector detector(kTouchLongMs);
    uint32_t now = 0;
    Counts counts;
    hold(detector, false, 50, now, counts);

    CHECK(detector.heldMs(now) == 0);
    hold(detector, true, 500, now, counts);
    CHECK(detector.heldMs(now) >= 400);
    CHECK(detector.heldMs(now) <= 500);
}

TEST_CASE("D17 -- two quick presses are one DoublePress, and two counted presses")
{
    PressDetector d(kButtonLongMs, 25, 400);
    uint32_t now = 0;
    Counts counts;
    hold(d, false, 100, now, counts); // establish the resting level
    hold(d, true, 100, now, counts);  // press one
    hold(d, false, 200, now, counts); // release, second press begins inside the gap
    hold(d, true, 100, now, counts);  // press two
    hold(d, false, 600, now, counts); // release, then silence

    CHECK(counts.doublePress == 1);
    CHECK(counts.shortPress == 0);
    CHECK(counts.longPress == 0);
    // Gate 1.4 arithmetic: two physical presses are two counts, whatever
    // gesture they combined into.
    CHECK(d.pressCount() == 2);
}

TEST_CASE("D17 -- a lone short press still arrives, one gap late")
{
    PressDetector d(kButtonLongMs, 25, 400);
    uint32_t now = 0;
    Counts counts;
    hold(d, false, 100, now, counts);
    hold(d, true, 100, now, counts);
    hold(d, false, 300, now, counts); // inside the gap: withheld
    CHECK(counts.shortPress == 0);
    hold(d, false, 200, now, counts); // gap expires: delivered
    CHECK(counts.shortPress == 1);
    CHECK(counts.doublePress == 0);
    CHECK(d.pressCount() == 1);
}

TEST_CASE("D17 -- two slow presses are two ShortPresses, not a double")
{
    PressDetector d(kButtonLongMs, 25, 400);
    uint32_t now = 0;
    Counts counts;
    hold(d, false, 100, now, counts);
    hold(d, true, 100, now, counts);
    hold(d, false, 700, now, counts); // well past the gap
    hold(d, true, 100, now, counts);
    hold(d, false, 700, now, counts);

    CHECK(counts.shortPress == 2);
    CHECK(counts.doublePress == 0);
    CHECK(d.pressCount() == 2);
}

TEST_CASE("D17 -- a short press chased by a long hold yields the long pair only")
{
    // The documented loss: the hand was heading for the menu, and a stray
    // screen advance underneath it would be worse than a swallowed short.
    PressDetector d(kButtonLongMs, 25, 400);
    uint32_t now = 0;
    Counts counts;
    hold(d, false, 100, now, counts);
    hold(d, true, 100, now, counts);
    hold(d, false, 200, now, counts); // release inside the gap
    hold(d, true, 2500, now, counts); // second press goes long
    hold(d, false, 600, now, counts);

    CHECK(counts.armed == 1);
    CHECK(counts.longPress == 1);
    CHECK(counts.shortPress == 0);
    CHECK(counts.doublePress == 0);
    CHECK(d.pressCount() == 2);
}

TEST_CASE("D17 -- gap zero means no double-press machinery at all (the touch input)")
{
    PressDetector d(1000); // touch: default debounce, gap 0
    uint32_t now = 0;
    Counts counts;
    hold(d, false, 100, now, counts);
    hold(d, true, 100, now, counts);
    hold(d, false, 100, now, counts); // ShortPress immediately, no withholding
    CHECK(counts.shortPress == 1);
    hold(d, true, 100, now, counts);
    hold(d, false, 100, now, counts);
    CHECK(counts.shortPress == 2);
    CHECK(counts.doublePress == 0);
}
