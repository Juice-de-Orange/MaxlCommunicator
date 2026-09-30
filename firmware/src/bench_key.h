/*
 * The network key and node id a bench image starts with.
 *
 * CLAUDE.md 2.1 permits a development key behind a compile flag for bench work
 * in phases 2-3, and section 6 requires that flag to fail a release build.
 * scripts/release_guard.py and build_guard.h enforce the second half. This is
 * the first half, which until 2026-08-31 did not exist: MAXL_DEV_KEY was passed
 * to the compiler and no source file ever read it, so the documented bench path
 * had no end. README.md described it anyway.
 *
 * Two boards need the SAME key and DIFFERENT addresses. Bring-up sketches 17 and
 * 18 each derived a key from NRF_FICR->DEVICEID -- unique per chip, reproducible
 * on it, and exactly wrong for a pair: every frame would fail its MIC on the
 * other side, which is indistinguishable from a broken radio.
 *
 * So the rule here is: a compiled-in key wins, and the per-chip derivation stays
 * as the fallback. A single node transmitting into an empty room keeps working
 * with no environment set, and a pair works by setting one variable on both.
 *
 *     MAXL_DEV_KEY=<32 hex> MAXL_NODE_ID=1 pio run -e bringup -t upload ...
 *     MAXL_DEV_KEY=<same>   MAXL_NODE_ID=2 pio run -e bringup -t upload ...
 *
 * The key exists in an environment variable and in the object code of a debug
 * build, and nowhere else. There has never been one in the tree for `git grep`
 * to find.
 */

#ifndef MAXL_BENCH_KEY_H
#define MAXL_BENCH_KEY_H

#include <nrf.h>
#include <stdint.h>

namespace bench {

/// The net id every bench image agrees on. Arbitrary, and shared so two boards
/// do not disagree about which network they are on before the key even matters.
inline constexpr uint8_t kNetId = 0x2A;

namespace detail {

inline int hexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Per chip, from the factory DEVICEID. Unique to a board, reproducible on it,
 * and in no file anywhere -- which is why it is the fallback rather than a
 * constant. Useless for a pair, on purpose: two boards derive two keys, and the
 * failure is loud (every MIC fails) rather than silent.
 */
inline void deriveFromDeviceId(uint8_t out[16])
{
    const uint32_t idLow = NRF_FICR->DEVICEID[0];
    const uint32_t idHigh = NRF_FICR->DEVICEID[1];
    for (uint8_t i = 0; i < 16; ++i) {
        const uint32_t word = (i < 8) ? idLow : idHigh;
        out[i] = static_cast<uint8_t>(((word >> ((i % 4) * 8)) & 0xFFu) ^ (0xA5u + i));
    }
}

} // namespace detail

/*
 * Fill `out` with the key this image should provision.
 *
 * Returns true when it came from MAXL_DEV_KEY -- i.e. when another board built
 * with the same variable will agree with it. False means the per-chip fallback,
 * which no other board can match.
 */
inline bool networkKey(uint8_t out[16])
{
#ifdef MAXL_DEV_KEY
    const char *hex = MAXL_DEV_KEY;
    for (uint8_t i = 0; i < 16; ++i) {
        const int hi = detail::hexDigit(hex[i * 2]);
        const int lo = detail::hexDigit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            // release_guard.py already refused anything but 32 hex characters.
            // Reaching here means the two disagree, and a half-parsed key is
            // worse than none: fall back rather than encrypt with rubbish.
            detail::deriveFromDeviceId(out);
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
#else
    detail::deriveFromDeviceId(out);
    return false;
#endif
}

/// The address this image answers to. MAXL_NODE_ID, defaulting to 1.
inline constexpr uint16_t nodeId()
{
#ifdef MAXL_NODE_ID
    return static_cast<uint16_t>(MAXL_NODE_ID);
#else
    return 1;
#endif
}

/// The other node, under the two-board convention: 1 talks to 2, 2 talks to 1.
inline constexpr uint16_t peerId()
{
    return static_cast<uint16_t>(3 - nodeId());
}

} // namespace bench

#endif // MAXL_BENCH_KEY_H
