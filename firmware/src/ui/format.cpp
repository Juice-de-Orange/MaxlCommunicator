#include "ui/format.h"

namespace ui {
namespace {

/// Append one character if it fits. Returns the new length.
size_t put(char *out, size_t size, size_t len, char c)
{
    if (len + 1 < size) {
        out[len] = c;
        return len + 1;
    }
    return len;
}

/// Unsigned value, most significant digit first, `pad` digits minimum.
size_t putUnsigned(char *out, size_t size, size_t len, uint32_t value, uint8_t pad)
{
    char digits[10];
    uint8_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    } while (value != 0 && count < sizeof(digits));

    while (count < pad && count < sizeof(digits)) {
        digits[count++] = '0';
    }

    while (count > 0) {
        len = put(out, size, len, digits[--count]);
    }
    return len;
}

uint32_t powerOfTen(uint8_t exponent)
{
    uint32_t value = 1;
    for (uint8_t i = 0; i < exponent; ++i) {
        value *= 10u;
    }
    return value;
}

} // namespace

const char *formatInt(char *out, size_t size, int32_t value)
{
    return formatScaled(out, size, value, 0);
}

const char *formatScaled(char *out, size_t size, int32_t value, uint8_t decimals)
{
    if (size == 0) {
        return out;
    }
    if (decimals > 4) {
        decimals = 4;
    }

    size_t len = 0;
    uint32_t magnitude;
    if (value < 0) {
        len = put(out, size, len, '-');
        // Negated as unsigned so INT32_MIN does not overflow on the way.
        magnitude = static_cast<uint32_t>(-(static_cast<int64_t>(value)));
    } else {
        magnitude = static_cast<uint32_t>(value);
    }

    const uint32_t scale = powerOfTen(decimals);
    len = putUnsigned(out, size, len, magnitude / scale, 1);
    if (decimals > 0) {
        len = put(out, size, len, '.');
        len = putUnsigned(out, size, len, magnitude % scale, decimals);
    }

    out[len < size ? len : size - 1] = '\0';
    return out;
}

const char *formatAge(char *out, size_t size, uint32_t seconds)
{
    if (size == 0) {
        return out;
    }

    uint32_t value = seconds;
    char unit = 's';
    if (seconds >= 86400u) {
        value = seconds / 86400u;
        unit = 'd';
    } else if (seconds >= 3600u) {
        value = seconds / 3600u;
        unit = 'h';
    } else if (seconds >= 60u) {
        value = seconds / 60u;
        unit = 'm';
    }

    size_t len = putUnsigned(out, size, 0, value, 1);
    len = put(out, size, len, unit);
    out[len < size ? len : size - 1] = '\0';
    return out;
}

const char *formatAgeCoarse(char *out, size_t size, uint32_t seconds)
{
    if (size == 0) {
        return out;
    }

    if (seconds < 60u) {
        size_t len = 0;
        const char *text = "<1m";
        for (const char *p = text; *p != '\0'; ++p) {
            len = put(out, size, len, *p);
        }
        out[len < size ? len : size - 1] = '\0';
        return out;
    }

    return formatAge(out, size, seconds);
}

const char *formatCoordinate(char *out, size_t size, int32_t degreesE7, bool isLatitude)
{
    if (size == 0) {
        return out;
    }

    const bool negative = degreesE7 < 0;
    const uint32_t magnitude =
        negative ? static_cast<uint32_t>(-(static_cast<int64_t>(degreesE7)))
                 : static_cast<uint32_t>(degreesE7);

    // Five decimals is about a metre, which is finer than the fix. The wire
    // carries seven; showing all of them would suggest a precision that is not
    // there.
    const uint32_t whole = magnitude / 10000000u;
    const uint32_t fraction = (magnitude % 10000000u) / 100u;

    size_t len = putUnsigned(out, size, 0, whole, 1);
    len = put(out, size, len, '.');
    len = putUnsigned(out, size, len, fraction, 5);
    len = put(out, size, len, ' ');
    if (isLatitude) {
        len = put(out, size, len, negative ? 'S' : 'N');
    } else {
        len = put(out, size, len, negative ? 'W' : 'E');
    }
    out[len < size ? len : size - 1] = '\0';
    return out;
}

const char *formatDistance(char *out, size_t size, uint32_t metres)
{
    if (size == 0) {
        return out;
    }

    size_t len;
    if (metres < 10000u) {
        len = putUnsigned(out, size, 0, metres, 1);
        len = put(out, size, len, 'm');
    } else {
        const uint32_t hundredMetres = (metres + 50u) / 100u;
        len = putUnsigned(out, size, 0, hundredMetres / 10u, 1);
        len = put(out, size, len, '.');
        len = putUnsigned(out, size, len, hundredMetres % 10u, 1);
        len = put(out, size, len, 'k');
        len = put(out, size, len, 'm');
    }
    out[len < size ? len : size - 1] = '\0';
    return out;
}

} // namespace ui
