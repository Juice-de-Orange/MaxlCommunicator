/*
 * Freeing an I2C bus a slave is holding hostage.
 *
 * I2C has no reset. A slave interrupted mid-byte -- by a reset, a brown-out, or
 * a debugger halt -- goes on driving SDA low waiting for the clocks that will
 * finish its transfer, and the master's first transaction then blocks for ever.
 * TWIM on the nRF52 does not time out on its own, so Wire.begin() and the read
 * that follows simply never return.
 *
 * Nothing in the sketch survives that, and the board gives no sign of it. Every
 * bring-up sketch prints from loop() and loop() is never reached, so a board in
 * this state is simply silent -- a FAILURE in setup would have been reported
 * (store.mounted = 0 and so on); only a HANG produces nothing at all. The LED
 * never blinks either, and the dead man's timer fires later with nothing to say.
 *
 * It still enumerates and it still flashes, which is what makes it confusing:
 * the core runs TinyUSB in its own high-priority task, independent of loop(), so
 * USB and the 1200-baud DFU touch keep working on a board whose application is
 * stuck. "It appears on the bus" is not evidence that the firmware is running.
 *
 * And a power cycle does not fix it. The PCF8563 has backup power -- it must, or
 * gate 0.5 could not pass -- so unplugging the USB cable leaves the one device
 * most likely to be holding the bus exactly where it was.
 *
 * Measured on node B, 2026-09-01: bring-up 09 (no I2C) printed normally while
 * bring-up 02 (I2C only) hung, on the same board minutes apart. With this
 * recovery in front of Wire.begin(), bring-up 02 passes and both devices answer
 * -- PCF8563 at 0x51, BME280 at 0x77, chip id 0x60.
 *
 * The standard escape is in the I2C specification and in NXP's AN10216: clock
 * the bus by hand until the slave releases SDA, then issue a STOP so it returns
 * to idle. This is that, and it runs before Wire.begin().
 */

#ifndef MAXL_HAL_I2C_RECOVER_H
#define MAXL_HAL_I2C_RECOVER_H

#include <stdint.h>

namespace hal {

struct I2cRecovery {
    bool wasStuck = false;   ///< SDA was low before we touched anything
    bool recovered = false;  ///< and it is high now
    uint8_t pulses = 0;      ///< clock pulses it took, 0..9
};

/**
 * Free the bus if a slave is holding SDA low. Safe to call always; when the bus
 * is idle it costs two pin reads and reports wasStuck = false.
 *
 * Call BEFORE Wire.begin(), because Wire.begin() is one of the calls that hangs.
 */
I2cRecovery recoverI2cBus(uint8_t sdaPin, uint8_t sclPin);

} // namespace hal

#endif // MAXL_HAL_I2C_RECOVER_H
