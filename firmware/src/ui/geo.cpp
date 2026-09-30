#include "ui/geo.h"

#include <math.h>

namespace ui {
namespace {

/// IUGG mean earth radius. The sphere the haversine below assumes.
constexpr double kEarthRadiusM = 6371008.8;

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

constexpr double kE7 = 1.0e7;

double toRadians(int32_t degreesE7)
{
    return (static_cast<double>(degreesE7) / kE7) * kDegToRad;
}

} // namespace

uint32_t distanceMetres(int32_t fromLatE7, int32_t fromLonE7, int32_t toLatE7, int32_t toLonE7)
{
    const double lat1 = toRadians(fromLatE7);
    const double lat2 = toRadians(toLatE7);
    const double dLat = lat2 - lat1;
    const double dLon = toRadians(toLonE7) - toRadians(fromLonE7);

    const double sinLat = sin(dLat / 2.0);
    const double sinLon = sin(dLon / 2.0);
    const double a = sinLat * sinLat + cos(lat1) * cos(lat2) * sinLon * sinLon;

    // atan2 rather than asin: asin loses precision for nearly antipodal points,
    // and more usefully it goes wrong quietly when `a` drifts a hair above 1
    // through rounding.
    const double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    const double metres = kEarthRadiusM * c;

    if (metres <= 0.0) {
        return 0;
    }
    if (metres >= 4294967295.0) {
        return 4294967295u;
    }
    return static_cast<uint32_t>(metres + 0.5);
}

uint16_t bearingDegrees(int32_t fromLatE7, int32_t fromLonE7, int32_t toLatE7, int32_t toLonE7)
{
    const double lat1 = toRadians(fromLatE7);
    const double lat2 = toRadians(toLatE7);
    const double dLon = toRadians(toLonE7) - toRadians(fromLonE7);

    const double y = sin(dLon) * cos(lat2);
    const double x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dLon);

    double degrees = atan2(y, x) * kRadToDeg;
    degrees = degrees - 360.0 * floor(degrees / 360.0);

    const long rounded = lround(degrees);
    return static_cast<uint16_t>(((rounded % 360) + 360) % 360);
}

const char *compassPoint(uint16_t bearing)
{
    static const char *const kPoints[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    // Each point owns 45 degrees centred on itself, so the boundaries sit at
    // 22.5, 67.5 and so on -- hence the +22 before the divide.
    const uint16_t index = static_cast<uint16_t>(((bearing % 360u) + 22u) / 45u) % 8u;
    return kPoints[index];
}

} // namespace ui
