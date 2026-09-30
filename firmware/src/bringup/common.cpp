#include "common.h"

#include <Arduino.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <Adafruit_TinyUSB.h>
#pragma GCC diagnostic pop

#include "hal/deadman.h"

#include "hal/board.h"
#include "hal/i2c_recover.h"

#ifndef MAXL_SETUP_TRACE
#define MAXL_SETUP_TRACE 0
#endif

namespace bringup {
namespace {

bool g_blinkState = false;

/*
 * Whether USB has come up at least once since boot.
 *
 * The dead man's timer exists for one shape of failure: the image runs, USB
 * never enumerates, and nobody can reach the device. Poking only while USB is up
 * expresses that exactly -- and breaks the moment a sketch is MEANT to run
 * without a host. Sketch 13 does precisely that: it measures the battery and the
 * RTC with the cable OUT, and under the old rule it would have shot itself into
 * DFU partway through and taken the measurement with it.
 *
 * Once USB has been up, the image is proven to enumerate and the timer's job is
 * done. What is NOT given up is the other half: the poke still comes from
 * commonService() in loop(), so a loop that stops coming back stops poking, and
 * the timer still fires. Bring-up sketch 14 is the proof of that and still
 * passes.
 */
bool g_usbEverReady = false;
hal::I2cRecovery g_i2cRecovery{};

} // namespace

void commonSetup(uint32_t deadmanMs)
{
    // Before anything else, so that a hang in a driver still ends in the
    // bootloader rather than in a device nobody can reach until morning.
    hal::deadman::arm(deadmanMs);

    pinMode(PIN_LED1, OUTPUT);
    digitalWrite(PIN_LED1, 1 - LED_STATE_ON);

    Serial.begin(115200);

    /*
     * Free the I2C bus before anything can touch it.
     *
     * A slave still holding SDA low from an interrupted transfer makes
     * Wire.begin() block for ever, and that takes the whole image with it: loop()
     * is never reached, so nothing is ever printed and the LED never blinks. The
     * board still enumerates and still flashes -- TinyUSB runs in its own task,
     * independent of loop() -- which is exactly what makes it look like a serial
     * problem rather than a hung application. Node B on 2026-09-01: bring-up 09
     * printed, bring-up 02 hung, and a power cycle changed nothing because the
     * PCF8563 has backup power and keeps holding the bus.
     *
     * Here rather than in each sketch: every sketch that touches Wire would
     * otherwise have to remember, and the one that forgets is unreachable.
     */
    g_i2cRecovery = hal::recoverI2cBus(hal::board::kPinI2cSda, hal::board::kPinI2cScl);

#if MAXL_SETUP_TRACE
    /*
     * Only under the trace flag, and bounded. traceStep() cannot report a hang
     * the host was not yet listening for, so this waits for the port -- and it
     * is exactly the wait the normal path refuses to do, which is why it is
     * behind a compile flag rather than a runtime one.
     */
    const uint32_t waitUntil = millis() + 4000;
    while (!usbReady() && millis() < waitUntil) {
        delay(10);
    }
    delay(300); // let the host's reader attach after enumeration
    traceStep("commonSetup");
#endif
}

hal::I2cRecovery i2cRecovery()
{
    return g_i2cRecovery;
}

void traceStep(const char *step)
{
#if MAXL_SETUP_TRACE
    Serial.print("TRACE ");
    Serial.println(step);
    Serial.flush();
#else
    (void)step;
#endif
}

bool usbReady()
{
    return TinyUSBDevice.mounted();
}

void commonService()
{
    // mounted() rather than Serial: it is true as soon as the host has
    // configured the device, and does not also require somebody to open the
    // port. A sketch left running unattended must not expire just because no
    // terminal happens to be attached.
    if (usbReady()) {
        g_usbEverReady = true;
    }
    if (g_usbEverReady) {
        hal::deadman::poke();
    }

    g_blinkState = !g_blinkState;
    digitalWrite(PIN_LED1, g_blinkState ? LED_STATE_ON : 1 - LED_STATE_ON);

    hal::deadman::service();
}

} // namespace bringup
