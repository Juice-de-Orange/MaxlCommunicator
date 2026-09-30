#include "hal/gnss_l76k.h"

#include <Arduino.h>

namespace hal {
namespace {

/// Bytes drained per poll(). At 9600 baud a second of traffic is 960 bytes, so
/// this keeps up comfortably at any loop rate while bounding the time one call
/// can take -- the main loop also has a radio to service.
constexpr uint16_t kMaxBytesPerPoll = 256;

/// Hold time for the reset pulse. The datasheet asks for at least 100 ms.
constexpr uint16_t kResetHoldMs = 120;

} // namespace

bool GnssL76k::begin()
{
    pinMode(PIN_GPS_RESET, OUTPUT);
    pinMode(PIN_GPS_WAKEUP, OUTPUT);

    // Start powered down. CLAUDE.md 1.5: off is the default, not a state the
    // device settles into once somebody remembers.
    powerOff();
    started_ = true;
    return true;
}

void GnssL76k::requestFix(uint32_t timeoutMs, uint32_t nowMs)
{
    if (!started_) {
        return;
    }

    parser_.reset();
    bytesSeen_ = 0;
    timeToFixMs_ = 0;
    requestedAtMs_ = nowMs;
    timeoutMs_ = timeoutMs;

    digitalWrite(PIN_GPS_WAKEUP, HIGH);
    digitalWrite(PIN_GPS_RESET, LOW);
    delay(kResetHoldMs);
    digitalWrite(PIN_GPS_RESET, HIGH);

    Serial1.begin(kBaud);
    uartOpen_ = true;

    state_ = GnssState::Searching;
}

void GnssL76k::powerOff()
{
    // Only if it was actually opened -- see uartOpen_ in the header. This guard
    // is not defensive tidiness; without it the very first call hangs for ever.
    if (uartOpen_) {
        Serial1.end();
        uartOpen_ = false;
    }
    digitalWrite(PIN_GPS_WAKEUP, LOW);
    digitalWrite(PIN_GPS_RESET, LOW);
    state_ = GnssState::Off;
}

GnssState GnssL76k::poll(uint32_t nowMs)
{
    if (state_ != GnssState::Searching && state_ != GnssState::Fixed) {
        return state_;
    }

    uint16_t budget = kMaxBytesPerPoll;
    while (budget-- > 0 && Serial1.available() > 0) {
        const int byte = Serial1.read();
        if (byte < 0) {
            break;
        }
        ++bytesSeen_;
        if (parser_.consume(static_cast<char>(byte)) && parser_.fix().hasPosition) {
            if (timeToFixMs_ == 0) {
                timeToFixMs_ = static_cast<uint32_t>(nowMs - requestedAtMs_);
                // A fix at exactly the same millisecond as the request would
                // read as "no fix yet" for ever. One millisecond is a lie small
                // enough to be harmless and large enough to be a value.
                if (timeToFixMs_ == 0) {
                    timeToFixMs_ = 1;
                }
            }
            state_ = GnssState::Fixed;
        }
    }

    if (state_ != GnssState::Fixed &&
        static_cast<uint32_t>(nowMs - requestedAtMs_) >= timeoutMs_) {
        // The hard timeout from CLAUDE.md 1.5. Powering down is the point of it,
        // so it happens here rather than being left to the caller.
        powerOff();
        state_ = GnssState::TimedOut;
    }

    return state_;
}

} // namespace hal
