/*
 * Opening the external flash, including the case where it is asleep.
 *
 * The ZD25WQ16B has no reset line. A deep power-down therefore survives every
 * MCU reset there is: the chip ignores every command except 0xAB, the bus reads
 * back 0xFFFFFF, and Adafruit_SPIFlash::begin() concludes there is no chip.
 *
 * That is not hypothetical. On 2026-08-31 node A read JEDEC 0xBA6015 in the
 * morning and 0xFFFFFF in the evening, with nothing in between but resets. The
 * consequence is worse than a missing filesystem: without the store there is no
 * persistent frame counter, and CLAUDE.md 2.1 requires a node that cannot
 * persist its counter to refuse to transmit. So the node goes quiet, for ever,
 * behaving exactly as specified, and nothing in the report says why.
 *
 * Sending 0xAB unconditionally before opening costs microseconds and is a no-op
 * on a chip that is already awake. Every caller does it through here so there is
 * one place to be right, and so the candidate list is not copied a seventh time.
 */

#ifndef MAXL_HAL_EXTERNAL_FLASH_H
#define MAXL_HAL_EXTERNAL_FLASH_H

#include <stdint.h>

class Adafruit_SPIFlash;
class Adafruit_FlashTransport_QSPI;

namespace hal {

/*
 * Wake the chip, then identify and open it.
 *
 * `jedecBeforeWake`, when given, receives the id read BEFORE the release
 * command. 0xFFFFFF there followed by a real id afterwards is the signature of a
 * chip that was in deep power-down, and it is the only evidence that
 * distinguishes that from a chip that is simply absent -- worth reporting rather
 * than swallowing.
 */
bool openExternalFlash(Adafruit_FlashTransport_QSPI &transport, Adafruit_SPIFlash &flash,
                       uint32_t *jedecBeforeWake = nullptr);

} // namespace hal

#endif // MAXL_HAL_EXTERNAL_FLASH_H
