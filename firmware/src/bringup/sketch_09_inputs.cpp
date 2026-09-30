/*
 * Bring-up 09 -- the two usable inputs, left running for a human.
 *
 * CLAUDE.md 1.7: a capacitive touch pad (TTP223, P0.11) and one physical user
 * button (P1.10, active low with a pull-up). The second push button is wired to
 * nRESET and is not available to the application -- it is not touched here, and
 * P0.18 appears nowhere in this file.
 *
 * Gates 1.4 and 1.5 want 500 and 200 deliberate presses. That is a human with a
 * thumb, and no amount of firmware substitutes for it. What this sketch does is
 * make those gates cheap to run later: it counts edges, measures how long each
 * press lasted, and reports the resting level of both pins.
 *
 * The resting level is worth having on its own. docs/hardware/pinmap.md records
 * the touch pad's polarity as unconfirmed, and a pin that sits at a stable level
 * for minutes with nobody near the device answers that without anyone pressing
 * anything.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 9

#include <Arduino.h>

#include "common.h"
#include "report.h"

namespace {

constexpr uint32_t kDebounceMs = 25;
constexpr uint32_t kReportEveryMs = 5000;

struct Input {
    const char *name;
    uint8_t pin;
    int stable;
    int lastRead;
    uint32_t lastChangeMs;
    uint32_t edges;
    uint32_t pressedSinceMs;
    uint32_t longestPressMs;
    uint32_t activations;
};

Input g_button{"button", PIN_BUTTON1, HIGH, HIGH, 0, 0, 0, 0, 0};
Input g_touch{"touch", PIN_BUTTON_TOUCH, LOW, LOW, 0, 0, 0, 0, 0};

/// The button is documented active low. The touch pad's polarity is not
/// confirmed, so it is treated symmetrically: an "activation" is a transition
/// away from whatever level the pin held at boot.
void poll(Input &input, int restingLevel)
{
    const int now = digitalRead(input.pin);
    if (now != input.lastRead) {
        input.lastRead = now;
        input.lastChangeMs = millis();
        return;
    }
    if (now == input.stable) {
        return;
    }
    if (millis() - input.lastChangeMs < kDebounceMs) {
        return;
    }

    input.stable = now;
    ++input.edges;
    if (now != restingLevel) {
        input.pressedSinceMs = millis();
        ++input.activations;
    } else if (input.pressedSinceMs != 0) {
        const uint32_t held = millis() - input.pressedSinceMs;
        if (held > input.longestPressMs) {
            input.longestPressMs = held;
        }
        input.pressedSinceMs = 0;
    }
}

void describe(const Input &input, int restingLevel)
{
    char key[48];
    snprintf(key, sizeof(key), "%s.level", input.name);
    report::value(key, "%d", input.stable);
    snprintf(key, sizeof(key), "%s.resting_level", input.name);
    report::value(key, "%d", restingLevel);
    snprintf(key, sizeof(key), "%s.edges", input.name);
    report::value(key, "%lu", static_cast<unsigned long>(input.edges));
    snprintf(key, sizeof(key), "%s.activations", input.name);
    report::value(key, "%lu", static_cast<unsigned long>(input.activations));
    snprintf(key, sizeof(key), "%s.longest_press_ms", input.name);
    report::value(key, "%lu", static_cast<unsigned long>(input.longestPressMs));
}

int g_buttonResting = HIGH;
int g_touchResting = LOW;
uint32_t g_lastReport = 0;

int g_touchWithPullUp = -1;
int g_touchWithPullDown = -1;

/*
 * Is anything driving the touch pin at all?
 *
 * On 2026-08-31 the same pin, read the same way -- pinMode(INPUT), no pull --
 * came back HIGH from sketch 11 and LOW from this one, and a five minute sweep
 * of the whole case produced zero edges. A CMOS input with nothing attached does
 * exactly that: it holds whatever stray charge left on it, stably, for minutes,
 * and a different value after the next reset.
 *
 * The pulls settle it. An output that is really connected wins against the
 * internal pull resistor (13 kOhm on nRF52) and holds its own level; a floating
 * pin follows whichever pull is enabled. So:
 *
 *   pull-up HIGH and pull-down LOW  -> nothing is driving it. The TTP223 is not
 *                                      fitted, or its output is not on P0.11.
 *   both the same level             -> something drives it, and that level is
 *                                      the real resting level.
 *
 * Costs two pinMode calls at boot and answers a question that cost an evening.
 */
void probeTouchDrive()
{
    pinMode(PIN_BUTTON_TOUCH, INPUT_PULLUP);
    delay(20);
    g_touchWithPullUp = digitalRead(PIN_BUTTON_TOUCH);

    pinMode(PIN_BUTTON_TOUCH, INPUT_PULLDOWN);
    delay(20);
    g_touchWithPullDown = digitalRead(PIN_BUTTON_TOUCH);

    // Back to the configuration the gates are measured in.
    pinMode(PIN_BUTTON_TOUCH, INPUT);
    delay(20);
}

} // namespace

namespace bringup {

void setup()
{
    commonSetup(0xFFFFFFFF); // left running for whoever walks up to it

    pinMode(PIN_BUTTON1, INPUT_PULLUP);
    probeTouchDrive();
    delay(50);

    g_buttonResting = digitalRead(PIN_BUTTON1);
    g_touchResting = digitalRead(PIN_BUTTON_TOUCH);
    g_button.stable = g_buttonResting;
    g_button.lastRead = g_buttonResting;
    g_touch.stable = g_touchResting;
    g_touch.lastRead = g_touchResting;
}

void loop()
{
    poll(g_button, g_buttonResting);
    poll(g_touch, g_touchResting);

    if (usbReady() && millis() - g_lastReport > kReportEveryMs) {
        g_lastReport = millis();
        report::begin(9);
        report::info("button=P1.10 (INPUT_PULLUP) touch=P0.11 (INPUT); P0.18 is nRESET and is "
                     "not touched");
        report::value("uptime_ms", "%lu", static_cast<unsigned long>(millis()));
        describe(g_button, g_buttonResting);
        describe(g_touch, g_touchResting);
        report::value("touch.with_pullup", "%d", g_touchWithPullUp);
        report::value("touch.with_pulldown", "%d", g_touchWithPullDown);
        report::value("touch.floating", "%d",
                      (g_touchWithPullUp == HIGH && g_touchWithPullDown == LOW) ? 1 : 0);
        report::verdict("inconclusive",
                        "resting levels recorded; gates 1.4 and 1.5 need 500 and 200 "
                        "deliberate presses by a human");
        report::end(9);
    }

    delay(2);
    commonService();

    /*
     * The LED follows the touch pin, and it is the last write in the loop on
     * purpose -- commonService() toggles the same pin for its liveness blink,
     * and at a 2 ms loop that blink is invisible anyway.
     *
     * This exists because hunting for the electrode by reading a counter
     * afterwards is a round trip through another person: sweep, ask, wait, hear
     * "still zero", sweep somewhere else. With the lamp on the pin, whoever is
     * holding the device sees the hit at the moment it happens, and the pad's
     * position stops being a research question.
     *
     * Off is idle. Lit is the pin away from its resting level, whichever
     * direction that turns out to be -- the same definition of "activation" the
     * counters use, so the lamp cannot disagree with the report.
     */
    const bool touchActive = digitalRead(PIN_BUTTON_TOUCH) != g_touchResting;
    digitalWrite(PIN_LED1, touchActive ? LED_STATE_ON : (1 - LED_STATE_ON));
}

} // namespace bringup

#endif // MAXL_BRINGUP == 9
