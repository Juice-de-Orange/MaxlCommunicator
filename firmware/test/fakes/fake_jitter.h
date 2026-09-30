/*
 * A jitter source a test can pin.
 *
 * CLAUDE.md 2.4 asks for randomised backoff. Randomness that cannot be pinned
 * makes the retry schedule untestable, so link::IJitterSource is an interface and
 * the tests supply this: either a fixed point in the range (mid, low, high) or a
 * small deterministic sequence.
 */

#ifndef MAXL_TEST_FAKE_JITTER_H
#define MAXL_TEST_FAKE_JITTER_H

#include "link/arq.h"

namespace fakes {

class FixedJitter : public link::IJitterSource {
public:
    /// `position` in [0, 1000] picks where in the range next() lands: 0 is the
    /// bottom, 500 the middle, 1000 the top.
    explicit FixedJitter(uint32_t position = 500) : position_(position) {}

    uint32_t next(uint32_t bound) override
    {
        if (bound == 0) {
            return 0;
        }
        const uint64_t scaled = (static_cast<uint64_t>(bound - 1u) * position_) / 1000u;
        return static_cast<uint32_t>(scaled);
    }

private:
    uint32_t position_;
};

} // namespace fakes

#endif // MAXL_TEST_FAKE_JITTER_H
