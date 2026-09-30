/*
 * When to push the panel, and how.
 *
 * CLAUDE.md 1.6 states the rule in one sentence and every part of it matters:
 * "Use partial refresh for anything that changes often. Redraw on state change
 * only. Ghosting mitigation is counted in *partial updates performed* -- full
 * refresh after 16 partials or on screen change, never on a wall-clock timer."
 *
 * Three decisions, and this class makes all three so that no screen has to:
 *
 *   nothing changed          -> None. Not a cheap refresh; no refresh at all.
 *   content changed          -> Partial (~0.3 s)
 *   16 partials, or a new
 *   screen                   -> Full (~2 s), and the count restarts
 *
 * `None` is the one that carries a gate. docs/test-plan.md 4.1 wants zero
 * partial refreshes across an hour with no state change, and 4.1's note names
 * the failure it exists to catch: "a redraw triggered by a value that
 * technically changed" -- an uptime counter, a millis() reading, a battery
 * voltage wobbling in the last millivolt. Screens render into a Canvas and the
 * Canvas is compared byte for byte; a value that changed but is not on screen
 * cannot reach the panel, because the comparison happens after rendering rather
 * than before it.
 *
 * Deliberately portable: no Arduino, no GxEPD2, no clock. It counts what it is
 * told about and nothing else, which is what lets a host test drive a thousand
 * updates through it in a millisecond instead of an hour.
 */

#ifndef MAXL_HAL_REFRESH_POLICY_H
#define MAXL_HAL_REFRESH_POLICY_H

#include <stdint.h>

namespace hal {

enum class RefreshKind : uint8_t {
    None = 0, ///< nothing to do -- the panel already shows this
    Partial,  ///< ~0.3 s, leaves ghosting behind
    Full,     ///< ~2 s, clears it
};

class RefreshPolicy {
public:
    /// CLAUDE.md 1.6. Counted in partials performed, never in seconds elapsed.
    static constexpr uint8_t kPartialsBeforeFull = 16;

    /// Decide and record. `contentChanged` is the byte-for-byte verdict on the
    /// rendered Canvas; `screenChanged` means the user moved to another screen,
    /// which always earns a full refresh regardless of how similar the two look.
    RefreshKind decide(bool contentChanged, bool screenChanged);

    /// Partials performed since the last full refresh. Reaches
    /// kPartialsBeforeFull and then goes back to zero.
    uint8_t partialsSinceFull() const { return partialsSinceFull_; }

    /// Lifetime totals, for the STATUS screen and for gate 4.1's evidence.
    uint32_t partialCount() const { return partialCount_; }
    uint32_t fullCount() const { return fullCount_; }
    uint32_t skippedCount() const { return skippedCount_; }

    /// After a panel reset, or on first power-up: the next push must be full,
    /// because nothing is known about what the glass is holding.
    void reset();

private:
    uint8_t partialsSinceFull_ = 0;
    bool needsFull_ = true;
    uint32_t partialCount_ = 0;
    uint32_t fullCount_ = 0;
    uint32_t skippedCount_ = 0;
};

} // namespace hal

#endif // MAXL_HAL_REFRESH_POLICY_H
