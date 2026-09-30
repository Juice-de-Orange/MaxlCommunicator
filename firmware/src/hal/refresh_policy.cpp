#include "hal/refresh_policy.h"

namespace hal {

RefreshKind RefreshPolicy::decide(bool contentChanged, bool screenChanged)
{
    /*
     * A screen change is a content change by definition -- but not the other way
     * round, and the order of these tests is what enforces that. Checking
     * contentChanged first would let a screen change that happens to render
     * identically (two empty peer lists, say) go out as a partial and keep the
     * ghost of the previous screen underneath it.
     */
    if (screenChanged) {
        needsFull_ = true;
    }

    if (!contentChanged && !needsFull_) {
        ++skippedCount_;
        return RefreshKind::None;
    }

    if (needsFull_ || partialsSinceFull_ >= kPartialsBeforeFull) {
        needsFull_ = false;
        partialsSinceFull_ = 0;
        ++fullCount_;
        return RefreshKind::Full;
    }

    ++partialsSinceFull_;
    ++partialCount_;
    return RefreshKind::Partial;
}

void RefreshPolicy::reset()
{
    partialsSinceFull_ = 0;
    needsFull_ = true;
}

} // namespace hal
