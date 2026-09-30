/*
 * Board variant implementation for the LilyGO T-Echo (MaxlCommunicator).
 *
 * Derived in part from the Adafruit nRF52 Arduino core variant template.
 * Copyright (c) 2014-2015 Arduino LLC.  All right reserved.
 * Copyright (c) 2016 Sandeep Mistry All right reserved.
 * Copyright (c) 2018, Adafruit Industries (adafruit.com)
 *
 * Licensed under the GNU Lesser General Public License, version 2.1 or later.
 */

#include "variant.h"

#include "nrf.h"
#include "wiring_constants.h"
#include "wiring_digital.h"

/*
 * Arduino pin number == absolute nRF pin number, so P0.xx maps to xx and P1.yy
 * maps to 32+yy. P0.00 and P0.01 carry the 32.768 kHz crystal and are locked out.
 */
const uint32_t g_ADigitalPinMap[] = {
    // P0.00 .. P0.31
    0xff, 0xff, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,

    // P1.00 .. P1.15
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47};

void initVariant()
{
    /*
     * Bring up the power rails first, and in this order.
     *
     * PIN_REG_EN (P0.13) enables the 3.3 V regulator. A 100k pull-up already
     * holds it high, so the rail is up before any code runs -- we drive it high
     * explicitly so that nothing can later float it, and so that the intent is
     * obvious to the next reader. It must NEVER be driven low: that would divide
     * the enable input down to roughly 0.35 V and switch off the rail this MCU
     * runs from. See docs/hardware/pinmap.md section 2.
     */
    pinMode(PIN_REG_EN, OUTPUT);
    digitalWrite(PIN_REG_EN, HIGH);

    /*
     * PIN_PWR_ON (P0.12) latches battery power and gates the peripheral rail.
     * It has an external 100k pulldown, so without this the node runs only while
     * USB is attached and dies the moment the cable is pulled.
     */
    pinMode(PIN_PWR_ON, OUTPUT);
    digitalWrite(PIN_PWR_ON, HIGH);

    // Status LED off (active low, so HIGH is off).
    pinMode(PIN_LED1, OUTPUT);
    digitalWrite(PIN_LED1, 1 - LED_STATE_ON);
}

/*
 * Park the e-paper control pins as inputs before power-down. Left as outputs
 * they leak current through the panel and, on this board, are enough to restart
 * the device after a shutdown.
 */
void variant_shutdown()
{
    pinMode(PIN_EINK_CS, INPUT);
    pinMode(PIN_EINK_DC, INPUT);
    pinMode(PIN_EINK_RES, INPUT);
    pinMode(PIN_EINK_BUSY, INPUT);
}
