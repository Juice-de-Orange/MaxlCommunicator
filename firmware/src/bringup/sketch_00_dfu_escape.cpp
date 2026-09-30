/*
 * Bring-up 00 -- prove the way back into the bootloader, and nothing else.
 *
 * This is the first image flashed in an unattended session, and every later one
 * depends on what it establishes. It touches no bus, opens no Serial, and links
 * against nothing but the core. All it does is blink P0.14 and, after a fixed
 * time, call enterUf2Dfu().
 *
 * It answers two questions at once:
 *
 *   1. Does the dead man's timer actually return the device to TECHOBOOT without
 *      a human pressing anything? Everything else tonight is flashed on the
 *      strength of a yes.
 *
 *   2. Does application code run at all? This is the cross-check the USB-CDC
 *      investigation asked for in its step 3 -- a blink-only image
 *      with no Serial access whatsoever. The timing separates the two cases and
 *      needs no eyes on the LED:
 *
 *        TECHOBOOT reappears after ~45 s  -> the application ran. The fault is
 *                                            confined to USB.
 *        TECHOBOOT is back within seconds -> the application never started. The
 *                                            fault is earlier: image layout,
 *                                            initVariant, the SoftDevice.
 *
 * No Serial on purpose. Opening it here would be the very subsystem under
 * suspicion, and a hang inside TinyUSB would then look like a dead application.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 0

#include <Arduino.h>

#include "hal/deadman.h"

namespace {

/// Short enough to iterate on, long enough that the reappearing mass storage
/// device cannot be confused with the bootloader simply refusing to start the
/// application.
constexpr uint32_t kEscapeAfterMs = 45000;

} // namespace

namespace bringup {

void setup()
{
    // First statement, before anything else can hang.
    hal::deadman::arm(kEscapeAfterMs);

    // initVariant() has already driven PIN_REG_EN high and configured the LED;
    // this only makes the intent local and visible.
    pinMode(PIN_LED1, OUTPUT);
    digitalWrite(PIN_LED1, 1 - LED_STATE_ON);
}

void loop()
{
    // A heartbeat that is obviously deliberate rather than a stuck pin: one
    // short pulse per second.
    digitalWrite(PIN_LED1, LED_STATE_ON);
    delay(80);
    digitalWrite(PIN_LED1, 1 - LED_STATE_ON);
    delay(920);

    // Never poked -- this image is meant to expire.
    hal::deadman::service();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 0
