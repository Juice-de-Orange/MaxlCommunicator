#include "i2c_recover.h"

#include <Arduino.h>

namespace hal {
namespace {

// Slow enough for any device on this board -- roughly 100 kHz. Recovery happens
// once at boot, so there is nothing to gain by hurrying it.
constexpr uint32_t kHalfPeriodUs = 5;

void releaseHigh(uint8_t pin)
{
    // Open drain: never drive high. Both lines are pulled up on the board, and
    // driving against a slave that is still holding low would be a short.
    pinMode(pin, INPUT_PULLUP);
}

void driveLow(uint8_t pin)
{
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
}

} // namespace

I2cRecovery recoverI2cBus(uint8_t sdaPin, uint8_t sclPin)
{
    I2cRecovery out{};

    releaseHigh(sdaPin);
    releaseHigh(sclPin);
    delayMicroseconds(kHalfPeriodUs);

    if (digitalRead(sdaPin) != LOW) {
        // Idle. Nothing to do, and nothing done -- Wire.begin() takes the pins
        // from here.
        out.wasStuck = false;
        out.recovered = true;
        return out;
    }

    out.wasStuck = true;

    /*
     * Up to nine pulses: eight to walk the slave out of the byte it is stuck in,
     * and a ninth for the ACK slot it is waiting to see. It releases SDA as soon
     * as it has counted enough clocks, so stop at the first pulse that frees the
     * line rather than always sending all nine.
     */
    for (uint8_t i = 0; i < 9; ++i) {
        driveLow(sclPin);
        delayMicroseconds(kHalfPeriodUs);
        releaseHigh(sclPin);
        delayMicroseconds(kHalfPeriodUs);
        out.pulses = static_cast<uint8_t>(i + 1);
        if (digitalRead(sdaPin) != LOW) {
            break;
        }
    }

    /*
     * A STOP: SDA low to high while SCL is high. Without it the slave is free of
     * its byte but still believes a transfer is in progress, and the next START
     * would be read as a repeated START.
     */
    driveLow(sdaPin);
    delayMicroseconds(kHalfPeriodUs);
    releaseHigh(sclPin);
    delayMicroseconds(kHalfPeriodUs);
    releaseHigh(sdaPin);
    delayMicroseconds(kHalfPeriodUs);

    out.recovered = digitalRead(sdaPin) != LOW;
    return out;
}

} // namespace hal
