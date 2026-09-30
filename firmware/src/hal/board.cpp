/*
 * board.h against variant.h, checked at compile time.
 *
 * There are two pin maps in this project and there have to be. variants/t-echo/
 * variant.h is what the Arduino core reads -- it is preprocessor macros, it is
 * included by everything, and its numbers are the ones the drivers actually pass
 * to pinMode(). hal/board.h is what phase 0 confirmed on the bench: the same
 * numbers, each with the sketch and the date that proved it.
 *
 * Two copies of the same numbers drift. The drift would be silent, and it would
 * show up as a driver talking to the wrong pin while a document said the right
 * one -- which is worse than having no document, because the document would be
 * believed.
 *
 * So this translation unit exists to hold them together. It contains no code and
 * emits no symbols; it is a wall of static_assert. If somebody edits one map and
 * not the other, the build stops and names the pin.
 *
 * A pin that is deliberately absent from board.h stays absent -- P0.13 is the
 * 3.3 V regulator enable and nothing in the application has any business naming
 * it, and P0.18 is nRESET. Neither is asserted here, because asserting them
 * would mean writing them down.
 */

#include "hal/board.h"

#include <Arduino.h>

namespace hal {
namespace board {
namespace {

// --- I2C, confirmed by sketch 02 -------------------------------------------
static_assert(kPinI2cSda == PIN_WIRE_SDA, "board.h and variant.h disagree on I2C SDA");
static_assert(kPinI2cScl == PIN_WIRE_SCL, "board.h and variant.h disagree on I2C SCL");

// --- External flash, confirmed by sketches 04 and 10 -----------------------
static_assert(kPinQspiSck == PIN_QSPI_SCK, "board.h and variant.h disagree on QSPI SCK");
static_assert(kPinQspiCs == PIN_QSPI_CS, "board.h and variant.h disagree on QSPI CS");
static_assert(kPinQspiIo0 == PIN_QSPI_IO0, "board.h and variant.h disagree on QSPI IO0");
static_assert(kPinQspiIo1 == PIN_QSPI_IO1, "board.h and variant.h disagree on QSPI IO1");
static_assert(kPinQspiIo2 == PIN_QSPI_IO2, "board.h and variant.h disagree on QSPI IO2");
static_assert(kPinQspiIo3 == PIN_QSPI_IO3, "board.h and variant.h disagree on QSPI IO3");

// --- SX1262, confirmed read-only by sketch 07 ------------------------------
static_assert(kPinRadioCs == SX126X_CS, "board.h and variant.h disagree on radio CS");
static_assert(kPinRadioReset == SX126X_RESET, "board.h and variant.h disagree on radio reset");
static_assert(kPinRadioBusy == SX126X_BUSY, "board.h and variant.h disagree on radio BUSY");
static_assert(kPinRadioDio1 == SX126X_DIO1, "board.h and variant.h disagree on radio DIO1");
static_assert(kPinRadioDio3Tcxo == SX126X_DIO3, "board.h and variant.h disagree on radio DIO3");
static_assert(kPinRadioSck == PIN_SPI_SCK, "board.h and variant.h disagree on radio SCK");
static_assert(kPinRadioMosi == PIN_SPI_MOSI, "board.h and variant.h disagree on radio MOSI");
static_assert(kPinRadioMiso == PIN_SPI_MISO, "board.h and variant.h disagree on radio MISO");

// --- GNSS, confirmed by sketches 06 and 11 ---------------------------------
// The direction of the UART pair is the one entry in docs/hardware/pinmap.md
// with a warning attached, so it is the one most worth pinning down here.
static_assert(kPinGnssRx == PIN_SERIAL1_RX, "board.h and variant.h disagree on GNSS RX");
static_assert(kPinGnssTx == PIN_SERIAL1_TX, "board.h and variant.h disagree on GNSS TX");
static_assert(kPinGnssWakeup == PIN_GPS_WAKEUP, "board.h and variant.h disagree on GNSS wakeup");
static_assert(kPinGnssReset == PIN_GPS_RESET, "board.h and variant.h disagree on GNSS reset");
static_assert(kPinGnssPps == PIN_GPS_PPS, "board.h and variant.h disagree on GNSS PPS");

// --- E-paper, confirmed by sketches 08 and 11 ------------------------------
static_assert(kPinEpaperCs == PIN_EINK_CS, "board.h and variant.h disagree on e-paper CS");
static_assert(kPinEpaperDc == PIN_EINK_DC, "board.h and variant.h disagree on e-paper DC");
static_assert(kPinEpaperReset == PIN_EINK_RES, "board.h and variant.h disagree on e-paper reset");
static_assert(kPinEpaperBusy == PIN_EINK_BUSY, "board.h and variant.h disagree on e-paper BUSY");
static_assert(kPinEpaperSck == PIN_SPI1_SCK, "board.h and variant.h disagree on e-paper SCK");
static_assert(kPinEpaperMosi == PIN_SPI1_MOSI, "board.h and variant.h disagree on e-paper MOSI");
static_assert(kPinEpaperFrontLight == PIN_EINK_BL,
              "board.h and variant.h disagree on the e-paper front light");
static_assert(kPinEpaperMisoUnconfirmed == PIN_SPI1_MISO,
              "board.h and variant.h disagree on e-paper MISO");

// --- Inputs, LED, battery, power -------------------------------------------
static_assert(kPinStatusLed == PIN_LED1, "board.h and variant.h disagree on the status LED");
static_assert(kLedOnLevel == LED_STATE_ON, "board.h and variant.h disagree on LED polarity");
static_assert(kPinButton == PIN_BUTTON1, "board.h and variant.h disagree on the user button");
static_assert(kPinTouch == PIN_BUTTON_TOUCH, "board.h and variant.h disagree on the touch pad");
static_assert(kPinBatteryAdc == PIN_A0, "board.h and variant.h disagree on the battery ADC");
static_assert(kPinPeripheralPower == PIN_PWR_ON,
              "board.h and variant.h disagree on the peripheral rail");

/*
 * P0.13 is REG_EN and P0.18 is nRESET. Both are absent from board.h on purpose
 * and are therefore not asserted -- asserting them would mean naming them, and
 * naming them is the first step towards somebody driving them.
 *
 * What IS asserted is that they are not any of the pins above, because a typo
 * that turned the status LED into the regulator enable would take the board down
 * with it and there would be nothing left to report the fault with.
 */
static_assert(kPinStatusLed != PIN_REG_EN, "the status LED must never be the regulator enable");
static_assert(kPinButton != PIN_REG_EN, "the user button must never be the regulator enable");
static_assert(kPinTouch != PIN_REG_EN, "the touch pad must never be the regulator enable");
static_assert(kPinPeripheralPower != PIN_REG_EN,
              "the peripheral rail must never be the regulator enable");

/*
 * P1.01 and P1.03 are LEDs on one hardware revision and ePaper MISO and LoRa
 * DIO0 on the other (docs/hardware/pinmap.md section 1). Until the revision of
 * this board is settled, nothing may drive either of them.
 */
constexpr uint8_t kRevisionAmbiguousA = 32 + 1;  // P1.01
constexpr uint8_t kRevisionAmbiguousB = 32 + 3;  // P1.03
static_assert(kPinStatusLed != kRevisionAmbiguousA && kPinStatusLed != kRevisionAmbiguousB,
              "the status LED must be P0.14, the only pin that is an LED on both revisions");

} // namespace
} // namespace board
} // namespace hal
