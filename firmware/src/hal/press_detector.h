/*
 * Debounce and press classification -- the portable half of the input layer.
 *
 * CLAUDE.md 3.2 (as amended by decision D17) gives two inputs and these actions:
 *
 *   Button, short          next screen
 *   Button, double         screen-specific primary action
 *   Button, long (> 2 s)   power menu
 *   Touch, short / long    next screen / primary action -- kept wired, unusable
 *                          on this device (touch_not_reachable, 2026-08-31)
 *
 * Two rules shape this class. The original "no chords, no double-taps" was
 * argued from the TOUCH input's reliability, and stands for it: a detector with
 * doubleGapMs = 0 has no multi-press state at all. The push button measured
 * clean -- 734 edges for 367 presses, exactly two per press -- and D17 moves
 * navigation onto it, double press included. And "long-press must give visual
 * feedback before the action fires", which is why a long press produces TWO
 * events: LongPressArmed the moment the threshold is crossed, while the finger
 * is still down, and LongPress on release. The screen draws its feedback on the
 * first and acts on the second, so the feedback cannot arrive after the thing
 * it was supposed to warn about. On e-paper, where a partial refresh costs
 * 0.3 s, that ordering is not a detail.
 *
 * The price of a double press is latency on the single one: a ShortPress is
 * held back for doubleGapMs to see whether a second press begins, so every
 * screen advance arrives that much later. Against a 0.3 s partial refresh the
 * default gap is not noticeable. One deliberate loss: a short press chased
 * within the gap by a press that turns LONG yields only the long-press pair --
 * the user's hand was clearly heading for the menu, and a stray screen advance
 * under the menu would be worse than a swallowed one.
 *
 * docs/test-plan.md gates 1.4 and 1.5 are counts -- 500 deliberate presses with
 * "zero doubles, zero misses", and 200 touches with the false-trigger rate
 * recorded. pressCount() and bounceCount() are those two numbers, which is why
 * this counts at all rather than only reporting edges.
 *
 * Timestamps are milliseconds from a monotonic source and are compared as
 * differences throughout, so the 49-day wraparound passes without notice -- the
 * same discipline app::Scheduler uses, for the same reason.
 */

#ifndef MAXL_HAL_PRESS_DETECTOR_H
#define MAXL_HAL_PRESS_DETECTOR_H

#include <stdint.h>

namespace hal {

enum class ButtonEvent : uint8_t {
    None = 0,
    ShortPress,     ///< released before the long-press threshold
    LongPressArmed, ///< threshold crossed, still held -- draw the feedback now
    LongPress,      ///< released after the threshold -- act now
    DoublePress,    ///< two short presses within doubleGapMs (D17; 0 disables)
};

class PressDetector {
public:
    /// 25 ms settles both a mechanical contact and the TTP223's output. Lower
    /// values let a bounce through; higher ones start to feel unresponsive.
    static constexpr uint16_t kDefaultDebounceMs = 25;

    /// `doubleGapMs` is how long after a short release a second press may begin
    /// and still count as a double press. Zero disables the gesture entirely --
    /// the touch input stays at zero (D17: the measurement that allows doubles
    /// was made on the push button, not on the pad).
    PressDetector(uint16_t longPressMs, uint16_t debounceMs = kDefaultDebounceMs,
                  uint16_t doubleGapMs = 0)
        : longPressMs_(longPressMs), debounceMs_(debounceMs), doubleGapMs_(doubleGapMs)
    {
    }

    /// Feed one sample of the debounced-in level. `rawPressed` is already
    /// polarity-corrected by the caller -- true means the user is touching it.
    ButtonEvent update(bool rawPressed, uint32_t nowMs);

    bool isPressed() const { return stable_; }

    /// How long the current press has lasted, 0 when nothing is held. Screens
    /// use this to draw a progress bar towards the long-press threshold.
    uint32_t heldMs(uint32_t nowMs) const;

    /// Completed presses of either kind. Gate 1.4's number.
    uint32_t pressCount() const { return pressCount_; }

    /// Transitions rejected as too short to be real. Gate 1.5 wants this: the
    /// touch pad's false-trigger rate "will not be zero".
    uint32_t bounceCount() const { return bounceCount_; }

    void resetCounters();

private:
    uint16_t longPressMs_;
    uint16_t debounceMs_;
    uint16_t doubleGapMs_;

    bool stable_ = false;       ///< the level we believe
    bool candidate_ = false;    ///< the level we are waiting to believe
    bool haveCandidate_ = false;
    bool armed_ = false;        ///< LongPressArmed already emitted for this press
    bool seenAny_ = false;      ///< first sample seen, so timestamps are meaningful

    /*
     * Whether the press now in progress was actually observed beginning.
     *
     * False for a level that was already down when the first sample arrived, and
     * that case is not hypothetical: docs/hardware/pinmap.md records the TTP223's
     * polarity on P0.11 as unconfirmed, with the vendor header and Meshtastic
     * disagreeing. Get it backwards and the pad reads as permanently pressed --
     * without this flag the first real touch would then release into a phantom
     * event, and the device would change screen on its own the moment anybody
     * came near it.
     */
    bool pressBegan_ = false;

    uint32_t candidateSinceMs_ = 0;
    uint32_t pressedSinceMs_ = 0;

    /// A short release waiting out doubleGapMs before it becomes a ShortPress.
    bool pendingShort_ = false;
    uint32_t pendingShortSinceMs_ = 0;
    /// The press now in progress began inside the gap -- its short release is
    /// the second half of a double press.
    bool doubleSecond_ = false;

    uint32_t pressCount_ = 0;
    uint32_t bounceCount_ = 0;
};

} // namespace hal

#endif // MAXL_HAL_PRESS_DETECTOR_H
