/*
 * The scaffolding every bring-up sketch from 01 onwards shares: dead man's
 * timer, status LED, USB CDC, and a report cycle that repeats so a late reader
 * still sees the whole run.
 *
 * Sketch 00 deliberately does not use this -- it must not touch Serial at all.
 */

#ifndef MAXL_BRINGUP_COMMON_H
#define MAXL_BRINGUP_COMMON_H

#include <stdint.h>

#include "hal/i2c_recover.h"

namespace bringup {

/// Arm the dead man's timer, configure the LED, start Serial. First thing in
/// setup(), before any peripheral is touched.
void commonSetup(uint32_t deadmanMs);

/// Poke the dead man's timer while USB is up, blink, and reboot into the
/// bootloader if USB never came up. Last thing in loop().
void commonService();

/// True once the host has enumerated and configured the device.
bool usbReady();

/**
 * Say how far setup() got, for the case where it does not finish.
 *
 * Every sketch prints from loop() and only once usbReady(), which is right for
 * an unattended run and useless for the failure it cannot survive: a setup()
 * that hangs produces no output at all, ever. Board B did exactly that on
 * 2026-09-01 -- dark LED, silent port, and the dead man's timer dropping it
 * into the bootloader fifteen minutes later with nothing to say about where it
 * stopped.
 *
 * Compiled in only under MAXL_SETUP_TRACE, because it needs commonSetup() to
 * wait for the host before it can print anything, and waiting is precisely what
 * the normal bring-up path must not do.
 *
 *   MAXL_SETUP_TRACE=1 MAXL_BRINGUP=19 pio run -e bringup -t upload
 */
void traceStep(const char *step);

/**
 * What commonSetup() found on the I2C bus, so a sketch can report it.
 *
 * wasStuck is not a warning to be ignored: it means a slave was holding the bus
 * and this image would not have booted at all before the recovery existed.
 */
hal::I2cRecovery i2cRecovery();

} // namespace bringup

#endif // MAXL_BRINGUP_COMMON_H
