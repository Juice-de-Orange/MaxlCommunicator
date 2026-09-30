/*
 * Bearing and distance between two points, for the POSITION screen.
 *
 * CLAUDE.md 3.3: "POSITION shows own coordinates, fix age, and bearing plus
 * distance to each peer as a numeric readout and an arrow. This is what you
 * actually read in the field, and it costs nothing."
 *
 * It costs nothing on the air, which is the sense that mattered there. It does
 * cost trigonometry, and the two functions below are why this is a separate
 * translation unit: they are pure, they build on the host, and a bearing that is
 * ninety degrees out is not something anybody notices by looking at an arrow.
 *
 * Coordinates are degrees x 1e7, the same integer form the wire uses
 * (CLAUDE.md 2.2), so nothing is converted on the way in.
 *
 * Haversine on a spherical earth. The error against the real ellipsoid is about
 * 0.3 %, which at the range this radio reaches -- single-digit kilometres -- is
 * a few metres. That is far inside the error of the fix itself, and an ellipsoid
 * formula would cost flash for precision the GNSS cannot supply.
 */

#ifndef MAXL_UI_GEO_H
#define MAXL_UI_GEO_H

#include <stdint.h>

namespace ui {

/// Great-circle distance in metres. Saturates at UINT32_MAX rather than
/// wrapping, which matters only for antipodal nonsense but costs nothing.
uint32_t distanceMetres(int32_t fromLatE7, int32_t fromLonE7, int32_t toLatE7, int32_t toLonE7);

/// Initial bearing in whole degrees, 0..359, measured clockwise from true north.
uint16_t bearingDegrees(int32_t fromLatE7, int32_t fromLonE7, int32_t toLatE7, int32_t toLonE7);

/// The eight-point compass name for a bearing: "N", "NE", "E", ... Returned as a
/// pointer to a static string, so it is safe to hold and must not be written to.
const char *compassPoint(uint16_t bearing);

} // namespace ui

#endif // MAXL_UI_GEO_H
