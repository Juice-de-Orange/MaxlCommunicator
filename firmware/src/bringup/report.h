/*
 * Machine-readable output for the bring-up sketches.
 *
 * docs/test-plan.md: "A phase is complete when its gate passes, on real
 * hardware, with the numbers written down." Numbers that have to be read off a
 * terminal by eye and retyped are numbers that get retyped wrong, so every
 * sketch emits a fixed grammar instead and tools/bringup_run.py collects it:
 *
 *     MAXL-BRINGUP 02 begin
 *     INFO   sda=26 scl=27
 *     RESULT i2c.addr.0x51 = present
 *     RESULT i2c.count = 2
 *     VERDICT pass
 *     MAXL-BRINGUP 02 end
 *
 * Everything is repeated on a loop, because output produced before the host
 * opens the port is lost -- there is no way to hold the device back until a
 * reader attaches without also hanging it when nobody ever does.
 */

#ifndef MAXL_BRINGUP_REPORT_H
#define MAXL_BRINGUP_REPORT_H

#include <stdint.h>

namespace report {

void begin(uint8_t sketch);
void end(uint8_t sketch);

/// Context that is not itself a result: pin numbers, chip IDs, conditions.
void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/// One measured or observed value. `key` is dotted and stable across runs.
void value(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/// pass / fail / inconclusive. "inconclusive" is for what needs a human or an
/// instrument -- it is not a failure and must not be recorded as one.
void verdict(const char *state, const char *why);

} // namespace report

#endif // MAXL_BRINGUP_REPORT_H
