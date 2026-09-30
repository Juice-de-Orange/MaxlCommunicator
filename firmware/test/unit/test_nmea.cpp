/*
 * The NMEA parser.
 *
 * The sentences below carry real checksums, computed over the real bodies. That
 * matters more than it looks: a test that feeds a sentence with a wrong checksum
 * and expects a fix would pass happily against a parser that never checks one,
 * and the first thing a freshly powered L76K emits is garbage.
 *
 * The coordinate expectations are worked out by hand rather than taken from the
 * implementation. 4807.038 N is 48 degrees and 7.038 minutes; 7.038 / 60 is
 * exactly 0.1173, so the answer is 481173000 and nothing about the arithmetic
 * gets a vote.
 */

#include <cstring>

#include "doctest.h"

#include "hal/nmea.h"

using namespace hal;

namespace {

const char kGgaFix[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
const char kGgaNoFix[] = "$GNGGA,081530.00,,,,,0,00,99.99,,,,,,*77\r\n";
const char kRmcValid[] =
    "$GPRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,310826,003.1,W*45\r\n";
const char kRmcWarning[] = "$GPRMC,123519.00,V,,,,,,,310826,,*1C\r\n";
const char kGsv[] =
    "$GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00*74\r\n";
const char kGgaSouthWest[] =
    "$GPGGA,123519,4807.038,S,01131.000,W,2,10,1.26,545.4,M,46.9,M,,*7E\r\n";

void feed(NmeaParser &parser, const char *sentence)
{
    for (const char *p = sentence; *p != '\0'; ++p) {
        parser.consume(*p);
    }
}

} // namespace

TEST_CASE("coordinate conversion is exact where the arithmetic allows it")
{
    int32_t value = 0;

    // 48 deg + 7.038 min; 7.038/60 = 0.1173 exactly.
    REQUIRE(nmeaCoordinateToE7("4807.038", 8, 'N', &value));
    CHECK(value == 481173000);

    // 11 deg + 31 min; 31/60 = 0.51666... and rounds up in the last digit.
    REQUIRE(nmeaCoordinateToE7("01131.000", 9, 'E', &value));
    CHECK(value == 115166667);

    REQUIRE(nmeaCoordinateToE7("4807.038", 8, 'S', &value));
    CHECK(value == -481173000);

    REQUIRE(nmeaCoordinateToE7("01131.000", 9, 'W', &value));
    CHECK(value == -115166667);
}

TEST_CASE("malformed coordinates are refused rather than guessed at")
{
    int32_t value = 0;
    CHECK_FALSE(nmeaCoordinateToE7("", 0, 'N', &value));
    CHECK_FALSE(nmeaCoordinateToE7("12", 2, 'N', &value));
    CHECK_FALSE(nmeaCoordinateToE7("4807.038", 8, 'X', &value));   // no such hemisphere
    CHECK_FALSE(nmeaCoordinateToE7("4877.038", 8, 'N', &value));   // 77 minutes
    CHECK_FALSE(nmeaCoordinateToE7("48o7.038", 8, 'N', &value));   // not a digit
}

TEST_CASE("checksums decide whether a sentence is looked at at all")
{
    CHECK(nmeaChecksumValid("GPGGA,1,2*55", 12));

    NmeaParser parser;
    feed(parser, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48\r\n");
    CHECK_FALSE(parser.fix().hasPosition);
    CHECK(parser.rejectedCount() == 1);
}

TEST_CASE("a GGA with a fix yields every field the POSITION payload needs")
{
    NmeaParser parser;
    feed(parser, kGgaFix);

    const GnssFix &fix = parser.fix();
    CHECK(fix.hasPosition);
    CHECK(fix.latitudeE7 == 481173000);
    CHECK(fix.longitudeE7 == 115166667);
    CHECK(fix.altitudeM == 545);          // 545.4 rounds down
    CHECK(fix.hdopTenths == 9);           // 0.9
    CHECK(fix.satellites == 8);
    CHECK(fix.quality == FixQuality::Gps);
}

TEST_CASE("HDOP rounds to nearest, because the wrong way turns marginal into good")
{
    NmeaParser parser;
    feed(parser, kGgaSouthWest);
    CHECK(parser.fix().hdopTenths == 13);  // 1.26 -> 12.6 tenths -> 13
    CHECK(parser.fix().quality == FixQuality::Dgps);
    CHECK(parser.fix().latitudeE7 == -481173000);
    CHECK(parser.fix().longitudeE7 == -115166667);
}

TEST_CASE("a GGA saying 'no fix' clears the position instead of leaving a stale one")
{
    NmeaParser parser;
    feed(parser, kGgaFix);
    REQUIRE(parser.fix().hasPosition);

    feed(parser, kGgaNoFix);
    CHECK_FALSE(parser.fix().hasPosition);
    CHECK(parser.fix().quality == FixQuality::None);
    CHECK(parser.fix().satellites == 0);

    // Still a well-formed sentence -- it is not a parse failure.
    CHECK(parser.rejectedCount() == 0);
}

TEST_CASE("RMC establishes the clock, which is what unblocks transmitting")
{
    NmeaParser parser;
    feed(parser, kRmcValid);

    CHECK(parser.fix().hasTime);
    // 2026-08-31 12:35:19 UTC
    CHECK(parser.fix().unixSeconds == 1788179719u);
}

TEST_CASE("an RMC in warning state does not set the clock")
{
    NmeaParser parser;
    feed(parser, kRmcValid);
    REQUIRE(parser.fix().hasTime);

    feed(parser, kRmcWarning);
    CHECK_FALSE(parser.fix().hasTime);
}

TEST_CASE("sentences we do not parse are not counted as failures")
{
    NmeaParser parser;
    feed(parser, kGsv);
    CHECK(parser.rejectedCount() == 0);
    CHECK(parser.acceptedCount() == 1);
    CHECK_FALSE(parser.fix().hasPosition);
}

TEST_CASE("an over-long sentence is dropped whole, and the next one still parses")
{
    NmeaParser parser;

    parser.consume('$');
    for (size_t i = 0; i < NmeaParser::kMaxSentence + 40; ++i) {
        parser.consume('A');
    }
    parser.consume('\r');
    parser.consume('\n');
    CHECK(parser.rejectedCount() == 1);

    feed(parser, kGgaFix);
    CHECK(parser.fix().hasPosition);
}

TEST_CASE("garbage before the first '$' is ignored, as it is after every power-up")
{
    NmeaParser parser;
    feed(parser, "\xFF\x00garbage,,,*ZZ");
    feed(parser, kGgaFix);
    CHECK(parser.fix().hasPosition);
}

TEST_CASE("reset forgets the fix so a power cycle cannot leave a stale position")
{
    NmeaParser parser;
    feed(parser, kGgaFix);
    feed(parser, kRmcValid);
    REQUIRE(parser.fix().hasPosition);
    REQUIRE(parser.fix().hasTime);

    parser.reset();
    CHECK_FALSE(parser.fix().hasPosition);
    CHECK_FALSE(parser.fix().hasTime);
    CHECK(parser.fix().latitudeE7 == 0);
    CHECK(parser.acceptedCount() == 0);
}

TEST_CASE("civil dates convert across the leap rules the naive version gets wrong")
{
    CHECK(daysFromCivil(1970, 1, 1) == 0u);
    CHECK(daysFromCivil(2000, 3, 1) == 11017u);   // 2000 is a leap year
    CHECK(daysFromCivil(2100, 3, 1) == 47541u);   // 2100 is not
    CHECK(daysFromCivil(2026, 8, 31) == 20696u);
}
