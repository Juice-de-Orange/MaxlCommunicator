/*
 * An IDisplay that keeps what it was told instead of a panel.
 *
 * Records every push: the kind, the region and the canvas. That is what lets a
 * test assert the two things the real driver's behaviour turns on -- that a full
 * refresh takes the whole panel and a partial takes only what moved, which is
 * the difference between 471 ms and 324 ms on the real one (gate 1.1).
 */

#ifndef MAXL_TEST_FAKE_DISPLAY_H
#define MAXL_TEST_FAKE_DISPLAY_H

#include <vector>

#include "hal/i_display.h"

namespace fakes {

class FakeDisplay : public hal::IDisplay {
public:
    struct Push {
        hal::RefreshKind kind;
        hal::Rect region;
    };

    bool begin() override
    {
        started = true;
        return !failBegin;
    }

    void present(const hal::Canvas &canvas, hal::RefreshKind kind, const hal::Rect &region) override
    {
        if (kind == hal::RefreshKind::None) {
            return;
        }
        pushes.push_back(Push{kind, region});
        last.copyFrom(canvas);
    }

    using hal::IDisplay::present;

    void sleep() override { asleep = true; }
    uint32_t lastRefreshMs() const override { return 0; }

    size_t countOf(hal::RefreshKind kind) const
    {
        size_t total = 0;
        for (const Push &push : pushes) {
            if (push.kind == kind) {
                ++total;
            }
        }
        return total;
    }

    std::vector<Push> pushes;
    hal::Canvas last;
    bool started = false;
    bool asleep = false;
    bool failBegin = false;
};

} // namespace fakes

#endif // MAXL_TEST_FAKE_DISPLAY_H
