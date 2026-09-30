/*
 * Phase 0 bring-up sketches.
 *
 * CLAUDE.md 5, phase 0: "confirm each peripheral with a minimal blink/read
 * sketch". One sketch per peripheral, selected at build time by MAXL_BRINGUP, so
 * that a failure names one bus and one chip rather than "the firmware".
 *
 *   pio run -e bringup -t upload        (MAXL_BRINGUP from the environment)
 *
 * Each sketch defines these two functions under its own
 * `#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == n` guard; every other sketch
 * file compiles to nothing. src/main.cpp dispatches here when MAXL_BRINGUP is
 * defined and keeps its normal banner behaviour when it is not.
 *
 * Two rules hold in every sketch, and both come from docs/hardware/pinmap.md:
 *
 *   P0.13 is the 3.3 V regulator enable, not a LED. Driven HIGH, never LOW.
 *   P1.01 and P1.03 are not touched at all until the hardware revision is known;
 *   on VERSION_1 boards they are ePaper MISO and LoRa DIO0, not LEDs.
 *
 * The revision-safe status LED is P0.14 (PIN_LED1), active low.
 */

#ifndef MAXL_BRINGUP_H
#define MAXL_BRINGUP_H

namespace bringup {

void setup();
void loop();

} // namespace bringup

#endif // MAXL_BRINGUP_H
