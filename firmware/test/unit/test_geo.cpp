/*
 * Bearing and distance for the POSITION screen.
 *
 * The expectations are computed independently, in Python, against the same
 * spherical model -- not read off this implementation. A haversine that agrees
 * with itself proves nothing; one that agrees with a separately written one to
 * within a metre over 250 km is doing the arithmetic right.
 *
 * The one that would go unnoticed in the field is bearing. A distance that is
 * wrong looks wrong; an arrow that points ninety degrees off looks exactly like
 * an arrow.
 */

#include "doctest.h"

#include "ui/geo.h"

using namespace ui;

namespace {

// A degree of latitude on the model sphere: 6371008.8 * pi / 180.
constexpr uint32_t kDegreeOfLatitudeM = 111195;

} // namespace

TEST_CASE("distance over a long leg matches an independently computed haversine")
{
    // Munich, Marienplatz to Vienna, Stephansplatz: 355844.7 m by the same
    // spherical model.
    const uint32_t metres = distanceMetres(481374000, 115755000, 482082000, 163738000);
    CHECK(metres >= 355844);
    CHECK(metres <= 355846);
}

TEST_CASE("a degree of latitude is a degree of latitude wherever you measure it")
{
    CHECK(distanceMetres(480000000, 130000000, 490000000, 130000000) == kDegreeOfLatitudeM);
    CHECK(distanceMetres(0, 0, 10000000, 0) == kDegreeOfLatitudeM);
    CHECK(distanceMetres(600000000, -1200000000, 610000000, -1200000000) == kDegreeOfLatitudeM);
}

TEST_CASE("a degree of longitude at the equator is the same length")
{
    CHECK(distanceMetres(0, 0, 0, 10000000) == kDegreeOfLatitudeM);
}

TEST_CASE("a hundred metres reads as a hundred metres, which is the range that matters")
{
    // 100 m north of 48.0 N: 100 / 111195 degrees, in units of 1e-7.
    const uint32_t metres = distanceMetres(480000000, 130000000, 480008993, 130000000);
    CHECK(metres >= 99);
    CHECK(metres <= 101);
}

TEST_CASE("the same point is zero away from itself, not a rounding artefact")
{
    CHECK(distanceMetres(480000000, 130000000, 480000000, 130000000) == 0);
    CHECK(distanceMetres(0, 0, 0, 0) == 0);
}

TEST_CASE("bearing points where the compass says it should")
{
    CHECK(bearingDegrees(480000000, 130000000, 490000000, 130000000) == 0);   // north
    CHECK(bearingDegrees(0, 0, 0, 10000000) == 90);                          // east
    CHECK(bearingDegrees(490000000, 130000000, 480000000, 130000000) == 180); // south
    CHECK(bearingDegrees(0, 100000000, 0, 90000000) == 270);                 // west
}

TEST_CASE("bearing over a long leg matches the independent computation")
{
    // Munich, Marienplatz to Vienna, Stephansplatz: 86.95 degrees.
    const uint16_t bearing = bearingDegrees(481374000, 115755000, 482082000, 163738000);
    CHECK(bearing >= 86);
    CHECK(bearing <= 87);
}

TEST_CASE("bearing is always in range, including for degenerate input")
{
    CHECK(bearingDegrees(480000000, 130000000, 480000000, 130000000) < 360);
    CHECK(bearingDegrees(900000000, 0, -900000000, 0) < 360);
    CHECK(bearingDegrees(0, -1800000000, 0, 1800000000) < 360);
}

TEST_CASE("compass points name the sector they are the middle of")
{
    CHECK(compassPoint(0)[0] == 'N');
    CHECK(compassPoint(359)[0] == 'N');
    CHECK(compassPoint(22)[0] == 'N');

    CHECK(compassPoint(45)[0] == 'N');
    CHECK(compassPoint(45)[1] == 'E');

    CHECK(compassPoint(90)[0] == 'E');
    CHECK(compassPoint(180)[0] == 'S');
    CHECK(compassPoint(270)[0] == 'W');

    // The boundary at 22.5 rounds to the nearer point, not down.
    CHECK(compassPoint(23)[0] == 'N');
    CHECK(compassPoint(23)[1] == 'E');
}
