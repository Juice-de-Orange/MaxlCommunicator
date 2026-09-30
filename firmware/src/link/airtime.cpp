#include "airtime.h"

#include "frame.h"
#include "payloads.h"

#include "band.h"

namespace link {
namespace {

uint8_t clampSf(uint8_t sf)
{
    if (sf < kMinSf) {
        return kMinSf;
    }
    if (sf > kMaxSf) {
        return kMaxSf;
    }
    return sf;
}

/*
 * Low data rate optimisation. Mandatory at BW125 for SF11 and SF12, where the
 * symbol time exceeds 16 ms and crystal drift would otherwise break demodulation.
 * It costs two bits per symbol, which is why the SF12 airtimes in CLAUDE.md 1.4
 * are worse than a naive doubling from SF11 would suggest.
 */
constexpr uint8_t kLowDataRateOptimiseFromSf = 11;

} // namespace

uint32_t symbolDurationUs(uint8_t sf)
{
    // BW125: Tsym = 2^sf / 125000 s = 2^sf * 8 us. Exact for every sf in range.
    return (1u << clampSf(sf)) * 8u;
}

uint32_t timeOnAirUs(uint8_t sf, uint8_t payloadBytes, uint16_t preambleSymbols)
{
    const uint8_t effectiveSf = clampSf(sf);
    const uint32_t tSym = symbolDurationUs(effectiveSf);

    /*
     * Preamble time is (nPreamble + 4.25) * Tsym -- the 4.25 covers the sync word
     * and the start-of-frame delimiter. Tsym is a multiple of 1024 us here, so
     * 17 * Tsym / 4 is exact and no fraction is lost.
     */
    const uint32_t preambleUs =
        static_cast<uint32_t>(preambleSymbols) * tSym + (17u * tSym) / 4u;

    /*
     * Semtech's payload symbol count, with CR4/5, explicit header and CRC on:
     *
     *   nPayload = 8 + max(ceil((8*PL - 4*SF + 28 + 16*CRC - 20*IH)
     *                          / (4*(SF - 2*DE))) * (CR + 4), 0)
     *
     * with CRC = 1, IH = 0 (explicit header), CR = 1 (4/5).
     */
    const uint8_t de = (effectiveSf >= kLowDataRateOptimiseFromSf) ? 1u : 0u;
    const int32_t numerator =
        8 * static_cast<int32_t>(payloadBytes) - 4 * static_cast<int32_t>(effectiveSf) + 28 + 16;
    const int32_t denominator = 4 * (static_cast<int32_t>(effectiveSf) - 2 * static_cast<int32_t>(de));

    // Ceiling division. The numerator is positive for every payload we can send,
    // but a short payload at a high SF could in principle make it negative, and
    // the max(..., 0) in the formula is there for exactly that.
    int32_t blocks = (numerator + denominator - 1) / denominator;
    if (blocks < 0) {
        blocks = 0;
    }
    const uint32_t payloadSymbols = 8u + static_cast<uint32_t>(blocks) * 5u;

    return preambleUs + payloadSymbols * tSym;
}

uint32_t timeOnAirMs(uint8_t sf, uint8_t payloadBytes, uint16_t preambleSymbols)
{
    // Round to nearest: this is how the CLAUDE.md 1.4 table is written, and the
    // tests check against it entry by entry.
    return (timeOnAirUs(sf, payloadBytes, preambleSymbols) + 500u) / 1000u;
}

uint16_t preambleSymbolsForInterval(uint8_t sf, uint32_t sniffIntervalMs)
{
    const uint32_t tSym = symbolDurationUs(sf);
    const uint32_t intervalUs = sniffIntervalMs * 1000u;
    const uint32_t symbols = (intervalUs + tSym - 1u) / tSym;  // ceil

    // The SX1262's preamble length register is 16 bits. At the 10 s maximum sniff
    // interval and SF7 this is 9766 symbols, so the cap is not reachable through
    // the configuration path -- it guards against a caller passing nonsense.
    if (symbols > UINT16_MAX) {
        return UINT16_MAX;
    }
    if (symbols < kStandardPreambleSymbols) {
        return kStandardPreambleSymbols;
    }
    return static_cast<uint16_t>(symbols);
}

uint8_t minSymbolsForSf(uint8_t sf)
{
    return (clampSf(sf) > 10u) ? 12u : 8u;
}

uint32_t ackWindowMs(uint8_t sf)
{
    /*
     * The ACK's own flight, twice, plus the ARQ's slack -- the same shape as
     * Arq::retryTimerMs, because the window exists to cover exactly the interval
     * in which the sender is waiting for this ACK rather than retrying.
     */
    const size_t ackFrameBytes = kFrameOverheadBytes + kAckPayloadBytes;
    return 2u * timeOnAirMs(sf, static_cast<uint8_t>(ackFrameBytes), kStandardPreambleSymbols)
           + kAckWindowSlackMs;
}

} // namespace link
