#include "hal/press_detector.h"

namespace hal {
namespace {

/// Unsigned subtraction, so the 49-day millis() wraparound is a non-event.
inline uint32_t elapsed(uint32_t now, uint32_t since)
{
    return static_cast<uint32_t>(now - since);
}

} // namespace

ButtonEvent PressDetector::update(bool rawPressed, uint32_t nowMs)
{
    if (!seenAny_) {
        /*
         * The first sample establishes the resting level rather than counting as
         * an edge. A device that boots with a finger already on the pad -- or
         * with the pad's idle level not what pinmap.md guessed -- must not
         * report a press it never saw begin.
         */
        seenAny_ = true;
        stable_ = rawPressed;
        pressBegan_ = false;
        pressedSinceMs_ = nowMs;
        return ButtonEvent::None;
    }

    if (rawPressed != stable_) {
        if (!haveCandidate_ || candidate_ != rawPressed) {
            haveCandidate_ = true;
            candidate_ = rawPressed;
            candidateSinceMs_ = nowMs;
        } else if (elapsed(nowMs, candidateSinceMs_) >= debounceMs_) {
            haveCandidate_ = false;
            stable_ = candidate_;

            if (stable_) {
                pressedSinceMs_ = nowMs;
                armed_ = false;
                pressBegan_ = true;
                if (pendingShort_) {
                    if (elapsed(nowMs, pendingShortSinceMs_) <= doubleGapMs_) {
                        // The second half of a double press has begun.
                        pendingShort_ = false;
                        doubleSecond_ = true;
                        return ButtonEvent::None;
                    }
                    /*
                     * The gap had already expired; this is a fresh first press.
                     * The withheld ShortPress goes out now rather than being
                     * forgotten -- late is a latency, lost is a miss, and gate
                     * 1.4 counts misses.
                     */
                    pendingShort_ = false;
                    return ButtonEvent::ShortPress;
                }
                return ButtonEvent::None;
            }

            // Release. A release we never saw begin is not a press -- see
            // pressBegan_ in the header.
            const bool wasLong = armed_;
            const bool real = pressBegan_;
            const bool wasSecond = doubleSecond_;
            armed_ = false;
            pressBegan_ = false;
            doubleSecond_ = false;
            if (!real) {
                return ButtonEvent::None;
            }
            ++pressCount_;
            if (wasLong) {
                // A long release ends any double-press sequence without a
                // ShortPress -- the deliberate loss the header documents.
                return ButtonEvent::LongPress;
            }
            if (doubleGapMs_ == 0) {
                return ButtonEvent::ShortPress;
            }
            if (wasSecond) {
                return ButtonEvent::DoublePress;
            }
            // Withhold the ShortPress until the gap decides what it was.
            pendingShort_ = true;
            pendingShortSinceMs_ = nowMs;
            return ButtonEvent::None;
        }
        return ButtonEvent::None;
    }

    /*
     * The level agrees with what we believe. If a candidate for the other level
     * was pending, it never lasted long enough -- that is a bounce, and gate 1.5
     * asks for the count.
     */
    if (haveCandidate_) {
        haveCandidate_ = false;
        ++bounceCount_;
    }

    if (stable_ && pressBegan_ && !armed_ && elapsed(nowMs, pressedSinceMs_) >= longPressMs_) {
        armed_ = true;
        return ButtonEvent::LongPressArmed;
    }

    // The gap ran out with nothing following: the withheld release was a
    // plain short press after all.
    if (!stable_ && pendingShort_ && elapsed(nowMs, pendingShortSinceMs_) > doubleGapMs_) {
        pendingShort_ = false;
        return ButtonEvent::ShortPress;
    }

    return ButtonEvent::None;
}

uint32_t PressDetector::heldMs(uint32_t nowMs) const
{
    if (!stable_) {
        return 0;
    }
    return elapsed(nowMs, pressedSinceMs_);
}

void PressDetector::resetCounters()
{
    pressCount_ = 0;
    bounceCount_ = 0;
}

} // namespace hal
