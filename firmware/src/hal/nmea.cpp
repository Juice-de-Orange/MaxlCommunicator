#include "hal/nmea.h"

namespace hal {
namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

int hexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/*
 * Locate comma-separated field `index` of a sentence body.
 *
 * Returns a pointer into the body and its length; an empty field is a valid
 * result with length 0, which matters -- NMEA marks "I do not know" by sending
 * nothing between two commas, and treating that as an error would reject every
 * sentence a module emits before it has a fix.
 */
bool field(const char *body, size_t len, size_t index, const char **out, size_t *outLen)
{
    size_t current = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        const bool end = (i == len) || (body[i] == ',') || (body[i] == '*');
        if (!end) {
            continue;
        }
        if (current == index) {
            *out = body + start;
            *outLen = i - start;
            return true;
        }
        ++current;
        start = i + 1;
        if (i < len && body[i] == '*') {
            break;
        }
    }
    return false;
}

/// Unsigned decimal, no sign, no exponent. False on any other character.
bool parseUint(const char *s, size_t len, uint32_t *out)
{
    if (len == 0) {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        if (!isDigit(s[i])) {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(s[i] - '0');
    }
    *out = value;
    return true;
}

/*
 * A decimal like "545.4" or "-12" scaled by 10^`scale`, rounded to nearest.
 *
 * Used for altitude (metres) and HDOP (tenths). Digits past the requested scale
 * are consumed for rounding and then dropped, so "0.96" at scale 1 gives 10 and
 * not 9 -- an HDOP that rounds the wrong way turns a marginal fix into a good
 * one on the screen.
 */
bool parseScaled(const char *s, size_t len, uint8_t scale, int32_t *out)
{
    if (len == 0) {
        return false;
    }

    size_t i = 0;
    bool negative = false;
    if (s[0] == '-') {
        negative = true;
        i = 1;
    } else if (s[0] == '+') {
        i = 1;
    }
    if (i >= len) {
        return false;
    }

    int32_t value = 0;
    bool sawDigit = false;
    for (; i < len && s[i] != '.'; ++i) {
        if (!isDigit(s[i])) {
            return false;
        }
        value = value * 10 + (s[i] - '0');
        sawDigit = true;
    }
    if (!sawDigit) {
        return false;
    }

    uint8_t taken = 0;
    if (i < len && s[i] == '.') {
        ++i;
        for (; i < len && taken < scale; ++i, ++taken) {
            if (!isDigit(s[i])) {
                return false;
            }
            value = value * 10 + (s[i] - '0');
        }
    }
    for (; taken < scale; ++taken) {
        value *= 10;
    }

    // One more digit, purely to round.
    if (i < len && isDigit(s[i]) && s[i] >= '5') {
        value += 1;
    }

    *out = negative ? -value : value;
    return true;
}

} // namespace

bool nmeaChecksumValid(const char *body, size_t len)
{
    // "a*hh" is the shortest thing that can carry one.
    if (len < 4) {
        return false;
    }
    size_t star = len;
    for (size_t i = 0; i < len; ++i) {
        if (body[i] == '*') {
            star = i;
            break;
        }
    }
    if (star + 3 != len) {
        return false;
    }

    const int high = hexValue(body[star + 1]);
    const int low = hexValue(body[star + 2]);
    if (high < 0 || low < 0) {
        return false;
    }

    uint8_t sum = 0;
    for (size_t i = 0; i < star; ++i) {
        sum = static_cast<uint8_t>(sum ^ static_cast<uint8_t>(body[i]));
    }
    return sum == static_cast<uint8_t>((high << 4) | low);
}

bool nmeaCoordinateToE7(const char *s, size_t len, char hemisphere, int32_t *out)
{
    if (len < 3) {
        return false;
    }

    size_t dot = len;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] == '.') {
            dot = i;
            break;
        }
    }

    // ddmm or dddmm before the point -- two minute digits and at least one
    // degree digit. Anything else is not a coordinate.
    if (dot < 3) {
        return false;
    }

    uint32_t whole = 0;
    if (!parseUint(s, dot, &whole)) {
        return false;
    }
    const uint32_t degrees = whole / 100u;
    const uint32_t minutesWhole = whole % 100u;
    if (degrees > 180u || minutesWhole >= 60u) {
        return false;
    }

    // Minutes as hundred-thousandths, which is more resolution than any consumer
    // module emits and keeps the later multiply inside int32.
    uint32_t minutesE5 = minutesWhole * 100000u;
    if (dot < len) {
        uint32_t scale = 10000u;
        for (size_t i = dot + 1; i < len && scale > 0; ++i) {
            if (!isDigit(s[i])) {
                return false;
            }
            minutesE5 += static_cast<uint32_t>(s[i] - '0') * scale;
            scale /= 10u;
        }
    }

    /*
     * degrees x 1e7 = degrees x 1e7 + minutesE5 / 60 x 1e7 / 1e5
     *               = degrees x 1e7 + minutesE5 x 5 / 3, rounded.
     * minutesE5 tops out at 5,999,999, so x10 stays well inside int32.
     */
    const uint32_t fromMinutes = ((minutesE5 * 10u / 3u) + 1u) / 2u;
    int32_t value = static_cast<int32_t>(degrees * 10000000u + fromMinutes);

    if (hemisphere == 'S' || hemisphere == 'W') {
        value = -value;
    } else if (hemisphere != 'N' && hemisphere != 'E') {
        return false;
    }

    *out = value;
    return true;
}

uint32_t daysFromCivil(uint16_t year, uint8_t month, uint8_t day)
{
    // Howard Hinnant's civil-from-days, the shifted-epoch form. Integer only and
    // correct across century leap rules, which the naive version is not.
    int32_t y = static_cast<int32_t>(year);
    y -= (month <= 2) ? 1 : 0;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
    const uint32_t doy =
        (153u * (month + (month > 2 ? -3u : 9u)) + 2u) / 5u + day - 1u;
    const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const int32_t days = era * 146097 + static_cast<int32_t>(doe) - 719468;
    return days < 0 ? 0u : static_cast<uint32_t>(days);
}

bool NmeaParser::consume(char c)
{
    if (c == '$') {
        inSentence_ = true;
        overrun_ = false;
        len_ = 0;
        return false;
    }
    if (!inSentence_) {
        return false;
    }
    if (c == '\r' || c == '\n') {
        inSentence_ = false;
        if (overrun_ || len_ == 0) {
            ++rejected_;
            return false;
        }
        buf_[len_] = '\0';
        return parseSentence(buf_, len_);
    }
    if (len_ >= kMaxSentence) {
        // Drop the whole sentence rather than truncating it. See the header.
        overrun_ = true;
        return false;
    }
    buf_[len_++] = c;
    return false;
}

bool NmeaParser::parseSentence(const char *body, size_t len)
{
    if (!nmeaChecksumValid(body, len)) {
        ++rejected_;
        return false;
    }

    const char *type = nullptr;
    size_t typeLen = 0;
    if (!field(body, len, 0, &type, &typeLen) || typeLen < 5) {
        ++rejected_;
        return false;
    }

    // Talker ID is GP, GN, GL, GA ... -- the last three characters are what
    // identifies the sentence, and a multi-constellation module uses all of them.
    const char *kind = type + (typeLen - 3);
    bool handled = false;
    if (kind[0] == 'G' && kind[1] == 'G' && kind[2] == 'A') {
        handled = parseGga(body, len);
    } else if (kind[0] == 'R' && kind[1] == 'M' && kind[2] == 'C') {
        handled = parseRmc(body, len);
    } else {
        // A sentence we do not parse is not a rejected sentence. The module
        // emits GSV and GSA constantly and counting those as failures would make
        // rejectedCount() useless as a link-quality number.
        ++accepted_;
        return false;
    }

    if (handled) {
        ++accepted_;
    } else {
        ++rejected_;
    }
    return handled;
}

bool NmeaParser::parseGga(const char *body, size_t len)
{
    const char *lat = nullptr;
    const char *latHem = nullptr;
    const char *lon = nullptr;
    const char *lonHem = nullptr;
    const char *qual = nullptr;
    const char *sats = nullptr;
    const char *hdop = nullptr;
    const char *alt = nullptr;
    size_t latLen = 0, latHemLen = 0, lonLen = 0, lonHemLen = 0;
    size_t qualLen = 0, satsLen = 0, hdopLen = 0, altLen = 0;

    if (!field(body, len, 2, &lat, &latLen) || !field(body, len, 3, &latHem, &latHemLen) ||
        !field(body, len, 4, &lon, &lonLen) || !field(body, len, 5, &lonHem, &lonHemLen) ||
        !field(body, len, 6, &qual, &qualLen) || !field(body, len, 7, &sats, &satsLen) ||
        !field(body, len, 8, &hdop, &hdopLen) || !field(body, len, 9, &alt, &altLen)) {
        return false;
    }

    uint32_t quality = 0;
    if (!parseUint(qual, qualLen, &quality)) {
        return false;
    }

    if (quality == 0) {
        // A valid sentence saying "no fix". Not an error, and the previous
        // position must go -- a stale coordinate on the POSITION screen is worse
        // than an empty one, because it looks like an answer.
        fix_.hasPosition = false;
        fix_.quality = FixQuality::None;
        fix_.satellites = 0;
        return true;
    }

    if (latHemLen != 1 || lonHemLen != 1) {
        return false;
    }

    int32_t latE7 = 0;
    int32_t lonE7 = 0;
    if (!nmeaCoordinateToE7(lat, latLen, latHem[0], &latE7) ||
        !nmeaCoordinateToE7(lon, lonLen, lonHem[0], &lonE7)) {
        return false;
    }

    fix_.latitudeE7 = latE7;
    fix_.longitudeE7 = lonE7;
    fix_.quality = (quality >= 2u) ? FixQuality::Dgps : FixQuality::Gps;
    fix_.hasPosition = true;

    uint32_t satellites = 0;
    fix_.satellites = parseUint(sats, satsLen, &satellites) && satellites < 256u
                          ? static_cast<uint8_t>(satellites)
                          : 0;

    int32_t hdopTenths = 0;
    if (parseScaled(hdop, hdopLen, 1, &hdopTenths) && hdopTenths >= 0) {
        fix_.hdopTenths = hdopTenths > 255 ? 255 : static_cast<uint8_t>(hdopTenths);
    } else {
        // Unknown, and 255 is the "as bad as it gets" value rather than 0, which
        // would read as a perfect fix.
        fix_.hdopTenths = 255;
    }

    int32_t altitude = 0;
    if (parseScaled(alt, altLen, 0, &altitude)) {
        if (altitude > 32767) {
            altitude = 32767;
        }
        if (altitude < -32768) {
            altitude = -32768;
        }
        fix_.altitudeM = static_cast<int16_t>(altitude);
    }

    return true;
}

bool NmeaParser::parseRmc(const char *body, size_t len)
{
    const char *time = nullptr;
    const char *status = nullptr;
    const char *date = nullptr;
    size_t timeLen = 0, statusLen = 0, dateLen = 0;

    if (!field(body, len, 1, &time, &timeLen) || !field(body, len, 2, &status, &statusLen) ||
        !field(body, len, 9, &date, &dateLen)) {
        return false;
    }

    if (statusLen != 1 || status[0] != 'A') {
        // 'V' -- navigation receiver warning. The time may still be running off
        // the module's own oscillator, but it is not disciplined and CLAUDE.md
        // 1.2 hangs the duty cycle budget on this clock. Not good enough.
        fix_.hasTime = false;
        return true;
    }

    if (dateLen == 6) {
        uint32_t ddmmyy = 0;
        if (parseUint(date, dateLen, &ddmmyy)) {
            const uint8_t day = static_cast<uint8_t>(ddmmyy / 10000u);
            const uint8_t month = static_cast<uint8_t>((ddmmyy / 100u) % 100u);
            const uint16_t year = static_cast<uint16_t>(2000u + (ddmmyy % 100u));
            if (day >= 1 && day <= 31 && month >= 1 && month <= 12) {
                lastDateDays_ = daysFromCivil(year, month, day);
                haveDate_ = true;
            }
        }
    }

    if (!haveDate_ || timeLen < 6) {
        return true;
    }

    uint32_t hhmmss = 0;
    if (!parseUint(time, 6, &hhmmss)) {
        return false;
    }
    const uint32_t hours = hhmmss / 10000u;
    const uint32_t minutes = (hhmmss / 100u) % 100u;
    const uint32_t seconds = hhmmss % 100u;
    if (hours > 23u || minutes > 59u || seconds > 59u) {
        return false;
    }

    fix_.unixSeconds = lastDateDays_ * 86400u + hours * 3600u + minutes * 60u + seconds;
    fix_.hasTime = true;
    return true;
}

void NmeaParser::reset()
{
    fix_ = GnssFix{};
    len_ = 0;
    inSentence_ = false;
    overrun_ = false;
    accepted_ = 0;
    rejected_ = 0;
    lastDateDays_ = 0;
    haveDate_ = false;
}

} // namespace hal
