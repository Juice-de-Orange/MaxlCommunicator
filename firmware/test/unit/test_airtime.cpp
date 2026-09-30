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
