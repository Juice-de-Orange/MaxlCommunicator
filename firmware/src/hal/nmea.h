/*
 * NMEA 0183 parsing for the L76K -- the portable half of the GNSS driver.
 *
 * Two sentences, and only two. GGA carries position, altitude, fix quality and
 * HDOP; RMC carries date, time and a validity flag. Everything else the module
 * says is discarded without being looked at, because parsing it would cost
 * flash and buy nothing: CLAUDE.md 2.2's POSITION payload has five fields and
 * they all come from GGA.
 *
 * RMC is here for a reason that is not position at all. CLAUDE.md 1.2: "If the
 * RTC time is not valid on boot, the device starts fully transmit-blocked until
 * time is re-established over BLE or GNSS." This parser is the GNSS half of
 * that sentence -- without a date and time out of RMC, a device that lost its
 * clock and has no phone in range can never transmit again.
 *
 * Fixed buffers, no allocation, no <string>, no sscanf. A sentence is capped at
 * 82 characters by the standard and anything longer is dropped rather than
 * truncated: a truncated sentence with a coincidentally valid checksum is not
 * possible, but a truncated sentence that overruns a buffer is a different class
 * of problem entirely.
 *
 * Every sentence is checksum-verified before a single field is used. A GNSS
 * module shares a UART with nothing here, but it is powered up and down
 * constantly (CLAUDE.md 1.5), and the first bytes after a power-up are reliably
 * garbage.
 *
 * Coordinates convert to degrees x 1e7 in integer arithmetic throughout. The
 * obvious float route loses precision exactly where it matters -- a float holds
 * about seven significant digits and a latitude in 1e-7 degrees needs nine.
 */

#ifndef MAXL_HAL_NMEA_H
#define MAXL_HAL_NMEA_H

#include <stddef.h>
#include <stdint.h>

namespace hal {

/// GGA field 6. 0 means no fix, and the driver treats anything above 0 as usable.
enum class FixQuality : uint8_t {
    None = 0,
    Gps = 1,
    Dgps = 2,
};

struct GnssFix {
    int32_t latitudeE7 = 0;
    int32_t longitudeE7 = 0;
    int16_t altitudeM = 0;

    /*
     * HDOP in TENTHS, clamped at 255 (= 25.5 and worse).
     *
     * CLAUDE.md 2.2 says "hdop uint8" and stops there, so the scale was never
     * written down; link/payloads.h carried the same bare byte. Tenths is the
     * choice this project makes: whole units would report every usable fix as
     * "1" and every bad one as "2", which is no information at all, and HDOP
     * above 25 is unusable by any standard. See docs/decisions D11.
     */
    uint8_t hdopTenths = 0;

    uint8_t satellites = 0;
    FixQuality quality = FixQuality::None;

    /// Unix seconds from RMC. 0 until a valid RMC with a date has been seen --
    /// the module reports a time before it reports a date, and a time without a
    /// date is worse than nothing for setting a clock.
    uint32_t unixSeconds = 0;

    bool hasPosition = false;
    bool hasTime = false;
};

class NmeaParser {
public:
    /// The standard's limit, plus the leading '$' and the trailing checksum.
    static constexpr size_t kMaxSentence = 82;

    /// Feed one byte. Returns true when a complete, checksum-valid sentence has
    /// just been parsed and fix() may have changed.
    bool consume(char c);

    /// Parse one complete sentence body, '$' and line ending excluded. Public
    /// because it is what the tests drive; consume() is a byte-wise wrapper.
    bool parseSentence(const char *body, size_t len);

    const GnssFix &fix() const { return fix_; }

    /// Sentences accepted and rejected since the last reset. The rejection count
    /// is the honest measure of how a GNSS link is doing -- it is never zero
    /// right after a power-up.
    uint32_t acceptedCount() const { return accepted_; }
    uint32_t rejectedCount() const { return rejected_; }

    /// Forget the fix and the counters. Called on every power-up of the module,
    /// so a stale position cannot survive into the next fix attempt.
    void reset();

private:
    bool parseGga(const char *body, size_t len);
    bool parseRmc(const char *body, size_t len);

    GnssFix fix_;
    char buf_[kMaxSentence + 1] = {0};
    size_t len_ = 0;
    bool inSentence_ = false;
    bool overrun_ = false;
    uint32_t accepted_ = 0;
    uint32_t rejected_ = 0;

    /// RMC gives the date, GGA does not. Held so a later GGA can still be
    /// timestamped without waiting for the next RMC.
    uint32_t lastDateDays_ = 0;
    bool haveDate_ = false;
};

/*
 * Helpers, exposed because they are what the tests actually pin down.
 */

/// "4807.038" plus hemisphere -> degrees x 1e7. False if the field is malformed.
bool nmeaCoordinateToE7(const char *field, size_t len, char hemisphere, int32_t *out);

/// XOR of everything between '$' and '*', compared against the two hex digits.
bool nmeaChecksumValid(const char *body, size_t len);

/// Days since the Unix epoch for a proleptic Gregorian date. Valid from 1970 on.
uint32_t daysFromCivil(uint16_t year, uint8_t month, uint8_t day);

} // namespace hal

#endif // MAXL_HAL_NMEA_H
