/*
 * Integer-only formatting for the screens.
 *
 * Small functions, and exactly the kind that are wrong at the boundaries: the
 * negative zero-point-something, the value that needs a leading zero after the
 * decimal point, INT32_MIN. All three are here.
 */

#include <cstring>

#include "doctest.h"

#include "ui/format.h"

using namespace ui;

TEST_CASE("a scaled integer gets its point in the right place")
{
    char buf[24];
    CHECK(std::strcmp(formatScaled(buf, sizeof(buf), 2134, 2), "21.34") == 0);
    CHECK(std::strcmp(formatScaled(buf, sizeof(buf), 4021, 3), "4.021") == 0);
    CHECK(std::strcmp(formatScaled(buf, sizeof(buf), 9, 1), "0.9") == 0);
    CHECK(std::strcmp(formatScaled(buf, sizeof(buf), 0, 2), "0.00") == 0);
    CHECK(std::strcmp(formatScaled(buf, sizeof(buf), 1234, 0) , "1234") == 0);
}

TEST_CASE("the fraction keeps its leading zeros")
{
    char buf[24];
    // 5 hundredths is "0.05", not "0.5". This is the one that reads plausibly
    // when it is wrong.
    CHECK(std::strcmp(formatScaled(buf, sizeof(buf), 5, 2), "0.05") == 0);
    CHECK(std::strcmp(formatScaled(buf, sizeof(buf), 105, 2), "1.05") == 0);
    CHECK(std::strcmp(formatScaled(buf, sizeof(buf), 1005, 3), "1.005") == 0);
}

TEST_CASE("negative values below one keep their sign")
{
    char buf[24];
    CHECK(std::strcmp(formatScaled(buf, sizeof(buf), -50, 2), "-0.50") == 0);
    CHECK(std::strcmp(formatScaled(buf, sizeof(buf), -1234, 2), "-12.34") == 0);
}

TEST_CASE("INT32_MIN does not overflow on the way to being printed")
{
    char buf[24];
    CHECK(std::strcmp(formatScaled(buf, sizeof(buf), -2147483647 - 1, 0), "-2147483648") == 0);
}

TEST_CASE("a short buffer is truncated, never overrun")
{
    char buf[5] = {'x', 'x', 'x', 'x', 'x'};
    formatScaled(buf, sizeof(buf), 123456, 2);
    CHECK(buf[4] == '\0');

    char one[1] = {'x'};
    formatScaled(one, sizeof(one), 42, 0);
    CHECK(one[0] == '\0');
}

TEST_CASE("an age reads as the largest unit that still says something")
{
    char buf[16];
    CHECK(std::strcmp(formatAge(buf, sizeof(buf), 0), "0s") == 0);
    CHECK(std::strcmp(formatAge(buf, sizeof(buf), 59), "59s") == 0);
    CHECK(std::strcmp(formatAge(buf, sizeof(buf), 60), "1m") == 0);
    CHECK(std::strcmp(formatAge(buf, sizeof(buf), 3599), "59m") == 0);
    CHECK(std::strcmp(formatAge(buf, sizeof(buf), 3600), "1h") == 0);
    CHECK(std::strcmp(formatAge(buf, sizeof(buf), 86399), "23h") == 0);
    CHECK(std::strcmp(formatAge(buf, sizeof(buf), 86400), "1d") == 0);
    CHECK(std::strcmp(formatAge(buf, sizeof(buf), 900000), "10d") == 0);
}

TEST_CASE("a coordinate reads the way it is read aloud")
{
    char buf[24];
    CHECK(std::strcmp(formatCoordinate(buf, sizeof(buf), 481173000, true), "48.11730 N") == 0);
    CHECK(std::strcmp(formatCoordinate(buf, sizeof(buf), -481173000, true), "48.11730 S") == 0);
    CHECK(std::strcmp(formatCoordinate(buf, sizeof(buf), 115166667, false), "11.51666 E") == 0);
    CHECK(std::strcmp(formatCoordinate(buf, sizeof(buf), -115166667, false), "11.51666 W") == 0);
    CHECK(std::strcmp(formatCoordinate(buf, sizeof(buf), 0, true), "0.00000 N") == 0);
}

TEST_CASE("distance switches unit where the extra digits stop helping")
{
    char buf[16];
    CHECK(std::strcmp(formatDistance(buf, sizeof(buf), 0), "0m") == 0);
    CHECK(std::strcmp(formatDistance(buf, sizeof(buf), 847), "847m") == 0);
    CHECK(std::strcmp(formatDistance(buf, sizeof(buf), 9999), "9999m") == 0);
    CHECK(std::strcmp(formatDistance(buf, sizeof(buf), 10000), "10.0km") == 0);
    CHECK(std::strcmp(formatDistance(buf, sizeof(buf), 12340), "12.3km") == 0);
    CHECK(std::strcmp(formatDistance(buf, sizeof(buf), 248380), "248.4km") == 0);
}

TEST_CASE("a coarse age never resolves to seconds -- gate 4.1's failure mode")
{
    char buf[16];
    // Anything under a minute is one string, so a value that ticks every second
    // renders identically and the screen never redraws for it.
    CHECK(std::strcmp(formatAgeCoarse(buf, sizeof(buf), 0), "<1m") == 0);
    CHECK(std::strcmp(formatAgeCoarse(buf, sizeof(buf), 1), "<1m") == 0);
    CHECK(std::strcmp(formatAgeCoarse(buf, sizeof(buf), 59), "<1m") == 0);

    CHECK(std::strcmp(formatAgeCoarse(buf, sizeof(buf), 60), "1m") == 0);
    CHECK(std::strcmp(formatAgeCoarse(buf, sizeof(buf), 3600), "1h") == 0);
    CHECK(std::strcmp(formatAgeCoarse(buf, sizeof(buf), 86400), "1d") == 0);
}
