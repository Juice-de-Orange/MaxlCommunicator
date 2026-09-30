/*
 * Integer-only formatting for the screens.
 *
 * Every value in the view model is a scaled integer -- centidegrees, millivolts,
 * degrees x 1e7 -- and printing them with %f would drag the floating-point
 * formatter into the image for no benefit. newlib's is tens of kilobytes and
 * this device has one job for it: putting a decimal point in the right place.
 *
 * Every function writes a NUL and never writes past `size`. They return the
 * buffer so calls can be nested inside a canvas.text().
 */

#ifndef MAXL_UI_FORMAT_H
#define MAXL_UI_FORMAT_H

#include <stddef.h>
#include <stdint.h>

namespace ui {

/// A scaled integer with a decimal point inserted: (-1234, 2) -> "-12.34".
/// `decimals` above 4 is clamped, because nothing here needs more.
const char *formatScaled(char *out, size_t size, int32_t value, uint8_t decimals);

/// Whole number.
const char *formatInt(char *out, size_t size, int32_t value);

/// A duration as the largest unit that still reads sensibly: "12s", "5m",
/// "3h", "9d". Ages on screen are glanced at, not measured.
const char *formatAge(char *out, size_t size, uint32_t seconds);

/*
 * A duration that never resolves to seconds: "<1m", "5m", "3h", "9d".
 *
 * For values that are on screen permanently and change on their own. An uptime
 * rendered in seconds changes every second, and a screen that redraws every
 * second is exactly what docs/test-plan.md gate 4.1 exists to catch -- "a redraw
 * triggered by a value that technically changed". Measured on node A before this
 * existed: 7 partial refreshes in the first 10 seconds of a run, every one of
 * them the uptime ticking.
 *
 * Nobody reads an uptime to the second. Use formatAge where the seconds are the
 * point, such as the age of a GNSS fix on a screen the user is looking at.
 */
const char *formatAgeCoarse(char *out, size_t size, uint32_t seconds);

/// Degrees x 1e7 with five decimals and a hemisphere letter, which is how a
/// coordinate is read aloud: "48.11730 N".
const char *formatCoordinate(char *out, size_t size, int32_t degreesE7, bool isLatitude);

/// Metres, switching to kilometres with one decimal above 10 km: "847m", "12.3km".
const char *formatDistance(char *out, size_t size, uint32_t metres);

} // namespace ui

#endif // MAXL_UI_FORMAT_H
