/*
 * Airtime against the published tables.
 *
 * These are not plausibility checks. CLAUDE.md 1.4 and 2.3 print exact
 * millisecond figures, the duty cycle budget charges what this module computes,
 * and the budget is a legal obligation (1.2). So the test is the spec table,
 * entry for entry. If someone edits the formula, or edits the table, this fails
 * and the two are reconciled deliberately rather than drifting apart.
 *
 * Frame sizes are payload + 16 bytes of overhead (12-byte header + 4-byte MIC),
 * per decision D7.
 */

#include "doctest.h"

#include "link/airtime.h"
#include "link/band.h"

using namespace link;

namespace {

// CLAUDE.md 1.4, with the standard 8-symbol preamble.
struct AirtimeRow {
    const char *name;
    uint8_t frameBytes;
    uint32_t sf7;
    uint32_t sf9;
    uint32_t sf10;
    uint32_t sf12;
};

constexpr AirtimeRow kSpecTable[] = {
    {"ACK", 20, 57, 185, 371, 1319},
    {"POSITION", 28, 67, 226, 412, 1647},
    {"TELEMETRY", 30, 72, 226, 453, 1647},
    {"TEXT max", 64, 118, 390, 698, 2793},
};

} // namespace

TEST_CASE("airtime reproduces the CLAUDE.md 1.4 table exactly")
{
    for (const AirtimeRow &row : kSpecTable) {
        CAPTURE(row.name);
        CHECK(timeOnAirMs(7, row.frameBytes, kStandardPreambleSymbols) == row.sf7);
        CHECK(timeOnAirMs(9, row.frameBytes, kStandardPreambleSymbols) == row.sf9);
        CHECK(timeOnAirMs(10, row.frameBytes, kStandardPreambleSymbols) == row.sf10);
        CHECK(timeOnAirMs(12, row.frameBytes, kStandardPreambleSymbols) == row.sf12);
    }
}

TEST_CASE("symbol duration at BW125 is exact")
{
    // Tsym = 2^sf / 125000 s. Integer microseconds, no rounding anywhere.
    CHECK(symbolDurationUs(7) == 1024u);
    CHECK(symbolDurationUs(8) == 2048u);
    CHECK(symbolDurationUs(9) == 4096u);
    CHECK(symbolDurationUs(10) == 8192u);
    CHECK(symbolDurationUs(11) == 16384u);
    CHECK(symbolDurationUs(12) == 32768u);
}

TEST_CASE("preamble sizing reproduces the CLAUDE.md 2.3 table")
{
    // SF, sniff interval ms, expected preamble symbols, expected TX airtime ms
    // for a 28-byte POSITION frame.
    struct Row { uint8_t sf; uint32_t intervalMs; uint16_t symbols; uint32_t airtimeMs; };
    constexpr Row kRows[] = {
        {9, 500, 123, 697},
        {9, 2000, 489, 2196},
        {9, 5000, 1221, 5195},
        {12, 1000, 31, 2400},
        {12, 5000, 153, 6398},
    };

    for (const Row &row : kRows) {
        CAPTURE(row.sf);
        CAPTURE(row.intervalMs);
        const uint16_t symbols = preambleSymbolsForInterval(row.sf, row.intervalMs);
        CHECK(symbols == row.symbols);
        CHECK(timeOnAirMs(row.sf, 28, symbols) == row.airtimeMs);
    }
}

TEST_CASE("the preamble always outlasts the sniff interval it has to bridge")
{
    /*
     * The property behind the table: a preamble shorter than the interval means
     * the receiver can sleep through the whole thing. REVIEW.md C5 -- this is the
     * mistake that costs 10-25 % of packets and a day of debugging.
     */
    for (uint8_t sf = kMinSf; sf <= kMaxSf; ++sf) {
        for (uint32_t interval = kMinSniffIntervalMs; interval <= kMaxSniffIntervalMs;
             interval += 250) {
            const uint16_t symbols = preambleSymbolsForInterval(sf, interval);
            const uint64_t preambleUs =
                static_cast<uint64_t>(symbols) * symbolDurationUs(sf);
            CAPTURE(sf);
            CAPTURE(interval);
            CHECK(preambleUs >= static_cast<uint64_t>(interval) * 1000u);
        }
    }
}

TEST_CASE("lockout after one POSITION frame matches CLAUDE.md 1.4")
{
    // Lockout = airtime * (100/duty - 1). Checked in whole tenths of a second so
    // the test reads like the table it is checking.
    struct Row { uint8_t sf; uint32_t g3Tenths; uint32_t g1Tenths; };
    constexpr Row kRows[] = {
        {7, 6, 66},
        {9, 20, 224},
        {10, 37, 408},
        {12, 148, 1630},
    };

    for (const Row &row : kRows) {
        CAPTURE(row.sf);
        const uint64_t airtimeUs = timeOnAirUs(row.sf, 28, kStandardPreambleSymbols);
        // g3 is 10 %: nine parts silence for one part transmission.
        CHECK((airtimeUs * 9u + 50000u) / 100000u == row.g3Tenths);
        // g1 is 1 %: ninety-nine parts silence.
        CHECK((airtimeUs * 99u + 50000u) / 100000u == row.g1Tenths);
    }
}

TEST_CASE("minimum sniff symbols follow Semtech's guidance")
{
    // REVIEW.md C5 and CLAUDE.md 2.3: 8 symbols up to SF10, 12 above.
    CHECK(minSymbolsForSf(7) == 8);
    CHECK(minSymbolsForSf(9) == 8);
    CHECK(minSymbolsForSf(10) == 8);
    CHECK(minSymbolsForSf(11) == 12);
    CHECK(minSymbolsForSf(12) == 12);
}

TEST_CASE("airtime matches an independent implementation of the SX126x formula")
{
    /*
     * The tables above are this project's own; this is someone else's. The
     * figures below come from RadioLib 7.8.1, SX126x::calculateTimeOnAir -- the
     * driver this firmware links -- which implements the time-on-air formula of
     * the SX1261/2 datasheet, section 6.1.4, in integer microseconds:
     *
     *   Tsym = 2^SF / BW
     *   ToA  = (Npreamble + 4.25 + 8 + ceil(max(8*PL + 16*CRC - 4*SF + 8 + 20*(1-IH), 0)
     *                                       / (4*(SF - 2*LDRO))) * (CR + 4)) * Tsym
     *
     * at the parameters CLAUDE.md 1.4 fixes and this module does not take: BW125,
     * CR4/5, explicit header, CRC on, low data rate optimisation from SF11 (where
     * Tsym reaches 16 ms, and where RadioLib switches it on by itself). They were
     * produced by running RadioLib's arithmetic over 288 combinations of SF7-12,
     * eight payload lengths and six preamble lengths; all 288 agreed with
     * timeOnAirUs to the microsecond, and these rows are a sample of them that
     * covers every SF, both sides of the LDRO switch and the sniff preambles.
     *
     * BW125 makes Tsym an exact multiple of 1024 us, so neither implementation
     * rounds anything here: what the budget charges is the exact airtime, and
     * there is no rounding direction to get wrong.
     */
    struct Row { uint8_t sf; uint8_t frameBytes; uint16_t preamble; uint32_t us; };
    constexpr Row kReference[] = {
        {7, 13, 8, 46336},      {8, 13, 8, 82432},       {9, 13, 8, 164864},
        {10, 13, 8, 288768},    {11, 13, 8, 577536},     {12, 13, 8, 1155072},
        {7, 51, 8, 102656},     {8, 51, 8, 184832},      {9, 51, 8, 328704},
        {10, 51, 8, 616448},    {11, 51, 8, 1314816},    {12, 51, 8, 2465792},
        {7, 64, 8, 118016},     {12, 64, 8, 2793472},    {9, 28, 489, 2196480},
        {9, 21, 489, 2155520},  {11, 28, 8, 905216},     {11, 28, 62, 1789952},
        {12, 20, 31, 2072576},  {7, 28, 1954, 2059520},
    };
    for (const Row &row : kReference) {
        CAPTURE(row.sf);
        CAPTURE(row.frameBytes);
        CAPTURE(row.preamble);
        CHECK(timeOnAirUs(row.sf, row.frameBytes, row.preamble) == row.us);
    }
}
