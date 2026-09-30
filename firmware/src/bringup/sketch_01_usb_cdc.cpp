/*
 * Bring-up 01 -- does USB CDC enumerate?
 *
 * Sketch 00 established that application code runs and that the fault is
 * confined to USB. This one tests the fix for it: -Wl,-u,TinyUSB_Device_Init in
 * platformio.ini, which forces the linker to extract the archive member that
 * defines the USB initialisation. Without it the symbol is absent from the image
 * entirely and the core's call goes to address 0.
 *
 * The dead man's timer is poked from TinyUSBDevice.mounted() rather than from
 * Serial: mounted() is true as soon as the host has enumerated and configured
 * the device, which is exactly the thing under test, and it does not also depend
 * on somebody opening the port. So:
 *
 *   USB enumerates  -> mounted() true -> poked -> the image stays up and talks
 *   USB stays dead  -> never poked    -> back to TECHOBOOT after 90 s
 *
 * Either way the device is reachable afterwards, which is the whole point.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 1

#include <Arduino.h>

/*
 * Adafruit_TinyUSB.h drags in the core's SPI.h, whose SPISettings constructor
 * shadows its own members. That is a vendor header, not our code, and -Wshadow
 * is worth keeping everywhere else -- so it is silenced here and nowhere else.
 * platformio.ini already notes the real fix: pass the core include paths as
 * -isystem. That is a change to every translation unit and does not belong in
 * the middle of a bring-up.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <Adafruit_TinyUSB.h>
#pragma GCC diagnostic pop

#include "hal/deadman.h"

namespace {

constexpr uint32_t kEscapeAfterMs = 90000;

uint32_t g_iteration = 0;
bool g_wasMounted = false;

/// Blink pattern doubles as a diagnosis for the case where CDC is still dead and
/// nobody can read anything: one pulse per second means not mounted, a rapid
/// double pulse means mounted.
void heartbeat(bool mounted)
{
    digitalWrite(PIN_LED1, LED_STATE_ON);
    delay(60);
    digitalWrite(PIN_LED1, 1 - LED_STATE_ON);
    if (mounted) {
        delay(120);
        digitalWrite(PIN_LED1, LED_STATE_ON);
        delay(60);
        digitalWrite(PIN_LED1, 1 - LED_STATE_ON);
    }
}

} // namespace

namespace bringup {

void setup()
{
    hal::deadman::arm(kEscapeAfterMs);

    pinMode(PIN_LED1, OUTPUT);
    digitalWrite(PIN_LED1, 1 - LED_STATE_ON);

    Serial.begin(115200);
}

void loop()
{
    const bool mounted = TinyUSBDevice.mounted();
    if (mounted) {
        hal::deadman::poke();
        if (!g_wasMounted) {
            g_wasMounted = true;
        }
    }

    // Printed every second regardless of whether anyone is listening: output
    // produced before the host opens the port is lost, so repetition is the only
    // way a late reader sees anything at all.
    Serial.printf("bringup01 n=%lu mounted=%d dtr=%d deadman=%lums\n",
                  static_cast<unsigned long>(g_iteration++),
                  mounted ? 1 : 0,
                  Serial ? 1 : 0,
                  static_cast<unsigned long>(hal::deadman::remainingMs()));

    heartbeat(mounted);
    delay(mounted ? 760 : 940);

    hal::deadman::service();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 1
