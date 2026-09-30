/*
 * Dead man's timer -- the way back into the bootloader without a human thumb.
 *
 * The T-Echo has exactly one recovery path for an application that does not come
 * up on USB: press the upper left button (P0.18, wired to nRESET) twice. That
 * works, and it was used to recover this device on 2026-08-30. It also requires
 * somebody to be in the room, which rules out unattended work -- and the image
 * currently on the flash is precisely the failure it protects against: a valid
 * application that never enumerates.
 *
 * So every bring-up image carries this. It arms a timer at boot; if nothing pokes
 * it before the timer runs out, the firmware calls enterUf2Dfu() from the core,
 * which writes GPREGRET = 0x57 and resets. The bootloader reads that register
 * before any application code runs, so TECHOBOOT reappears on its own and the
 * next image can be flashed.
 *
 * What it covers: the application runs but USB does not. That is the diagnosed
 * fault, and it is the one that would otherwise cost a night.
 *
 * IT RUNS IN ITS OWN FreeRTOS TASK, and that is the whole point of the design.
 *
 * The first version checked the deadline from service(), called out of loop().
 * That covers a device whose USB never comes up, and nothing else -- a loop()
 * that never comes back never checks anything. Bring-up sketch 11 hit exactly
 * that on 2026-08-31: hal::GnssL76k::begin() called Serial1.end() on a UART that
 * had never been begun, the core's end() spun on an event a disabled UARTE never
 * raises, and its yield() kept FreeRTOS scheduling. So USB stayed up, the host
 * saw a healthy device, and nothing printed for as long as anyone cared to wait.
 * The timer was armed the entire time and never got a chance to look at itself.
 *
 * A separate task at a higher priority than the loop covers that case, because
 * it does not need loop() to come back -- only the scheduler to still be
 * running, which in the failure above it was. The task blocks on vTaskDelay
 * between checks, so it costs a kilobyte of stack and nothing else.
 *
 * What it still does NOT cover:
 *
 *   - A crash before setup() reaches arm(). Nothing in the application can cover
 *     that; it is what the reset button is for.
 *   - A hang with interrupts masked, or one that takes the scheduler down with
 *     it. Then no task runs, this one included.
 *
 * If the task cannot be created, arm() says so and service() falls back to
 * checking the deadline itself -- the old behaviour, which is worse than the new
 * one and much better than none.
 *
 * Arm early. The first statement of setup(), before any peripheral is touched,
 * so that a hang inside a driver still ends in the bootloader rather than in a
 * device that has to wait for morning.
 */

#ifndef MAXL_HAL_DEADMAN_H
#define MAXL_HAL_DEADMAN_H

#include <stdint.h>

namespace hal {
namespace deadman {

/*
 * What happens when it fires, and it is not the same answer in both worlds.
 *
 * Bootloader is right for bring-up: a host is standing by and the next image
 * wants flashing, so leaving the device in DFU is exactly where it should be.
 *
 * Reset is right for a device in a field. A node that answered a hang by
 * sitting in the bootloader would be waiting for somebody with a laptop, in a
 * place chosen for having nobody with a laptop. Restarting the application is
 * the only useful thing it can do for itself.
 *
 * The default follows the build: debug images -- every bring-up sketch is one --
 * go to the bootloader, release images reset. That is the same seam the rest of
 * the project already uses for "is a person watching this".
 */
enum class Recovery : uint8_t {
    Bootloader = 0,
    Reset,
};

/// The default for this build. Debug -> Bootloader, release -> Reset.
Recovery defaultRecovery();

/// Start the timer. `timeoutMs` counts from now, and from every later poke().
void arm(uint32_t timeoutMs, Recovery recovery = defaultRecovery());

/// Restart the countdown. Call this whenever the host has proven it is there.
void poke();

/// Stop the timer for good. Only for an image already proven to enumerate --
/// after this call there is no way back except the reset button.
void disarm();

/*
 * Kept, and still worth calling from loop().
 *
 * Normally a no-op: the task does the firing. It matters when the task could not
 * be created -- see taskRunning() -- in which case this is the only thing that
 * will ever check the deadline.
 */
void service();

/// Whether the watchdog task actually exists. False means the timer degraded to
/// the old loop-driven behaviour, and a hang inside loop() is no longer covered.
bool taskRunning();

/// Milliseconds left, or 0 when disarmed or already expired. For logging.
uint32_t remainingMs();

} // namespace deadman
} // namespace hal

#endif // MAXL_HAL_DEADMAN_H
