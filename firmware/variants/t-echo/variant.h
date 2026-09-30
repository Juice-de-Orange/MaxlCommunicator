/*
 * Board variant for the LilyGO T-Echo (nRF52840 + SX1262), MaxlCommunicator.
 *
 * Pin assignments follow docs/hardware/pinmap.md. The authoritative sources are
 * LilyGO's own examples/Factory/utilities.h, cfr34k/t-echo-lora-aprs config/pinout.h
 * and the vendor schematic; they agree on every shared pin. Meshtastic's variant
 * disagrees in three places and is NOT followed here -- see the pin map for why.
 *
 * Derived in part from the Adafruit nRF52 Arduino core variant template.
 * Copyright (c) 2014-2015 Arduino LLC.  All right reserved.
 * Copyright (c) 2016 Sandeep Mistry All right reserved.
 * Copyright (c) 2018, Adafruit Industries (adafruit.com)
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version.
 */

#ifndef _VARIANT_MAXL_TECHO_
#define _VARIANT_MAXL_TECHO_

/** Master clock frequency */
#define VARIANT_MCK (64000000ul)

#define USE_LFXO // board has a 32.768 kHz crystal

#include "WVariant.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAXL_BOARD_T_ECHO

// Number of pins defined in PinDescription array
#define PINS_COUNT (48)
#define NUM_DIGITAL_PINS (48)
#define NUM_ANALOG_INPUTS (1)
#define NUM_ANALOG_OUTPUTS (0)

/*
 * ---------------------------------------------------------------------------
 * POWER RAILS -- read docs/hardware/pinmap.md section 2 before touching these.
 * ---------------------------------------------------------------------------
 *
 * PIN_REG_EN (P0.13) is the enable input of the AP2112K-3.3 regulator, NOT a
 * LED. Driving it LOW divides the 100k pull-up down to ~0.35 V, below the
 * regulator's V_IL(max) of 0.4 V, which switches off the rail the nRF52840
 * itself runs from. Deliberately no LED alias is defined for this pin.
 *
 * PIN_PWR_ON (P0.12) latches battery power and gates the peripheral rail
 * (e-paper, GNSS, BME280, LEDs) through Q8. It has an external 100k pulldown.
 * The LoRa module hangs directly off the regulator and does NOT need this pin
 * -- whether that holds in practice is a phase 0 measurement.
 */
#define PIN_REG_EN (0 + 13)
#define PIN_PWR_ON (0 + 12)

/*
 * LEDs -- common anode on VDD_POWR, so they are active low.
 *
 * Only P0.14 is used. It is the one pin that is a LED on BOTH known hardware
 * revisions (colour differs: blue on current boards, red on VERSION_1), so it
 * cannot damage anything.
 *
 * P1.01 and P1.03 are LEDs on current boards but are LoRa-DIO0 and e-paper MISO
 * on VERSION_1 hardware. Driving them push-pull on the wrong revision causes bus
 * conflicts -- this is what made Meshtastic PR #3051 boot-loop a subset of
 * devices. They stay undefined until the revision of this board is established.
 *
 * P0.15 has no function on current boards; the net ends on an NC pin of the
 * SX1262 module. Do not use.
 */
#define PIN_LED1 (0 + 14)
#define LED_BUILTIN PIN_LED1
#define LED_STATE_ON 0 // LEDs are lit when the pin is LOW

/*
 * Bluefruit52Lib refers to LED_BLUE by name as its connection indicator and does
 * not compile without it. It is pointed at P0.14 -- the same pin -- rather than
 * at P1.01 or P1.03, which are LEDs on the current hardware revision and ePaper
 * MISO and LoRa DIO0 on VERSION_1 boards. Driving those before the revision of
 * this device is known is what boot-looped Meshtastic (PR #3051, reverted by
 * #3304), and docs/hardware/pinmap.md section 1 rules it out.
 *
 * hal/ble_transport_bluefruit.cpp calls Bluefruit.autoConnLed(false), so the
 * library never actually drives it: the pin stays ours to use as a status LED,
 * and the definition exists to satisfy the compiler and nothing else.
 */
#define LED_BLUE PIN_LED1
#define LED_CONN PIN_LED1

/*
 * Buttons
 *
 * PIN_BUTTON2 is deliberately NOT defined: P0.18 is the nRESET pin. Repurposing
 * it via UICR PSELRESET would cost the double-tap-reset route into the
 * bootloader, which is the only recovery path without an SWD probe.
 */
#define PIN_BUTTON1 (32 + 10) // user button, active low with pull-up
#define PIN_BUTTON_TOUCH (0 + 11) // TTP223; polarity unconfirmed, see pin map

/*
 * Analog
 */
#define PIN_A0 (4) // battery sense, AIN2, external 2:1 divider
static const uint8_t A0 = PIN_A0;
#define BATTERY_PIN PIN_A0
#define ADC_RESOLUTION 12
#define BATTERY_SENSE_RESOLUTION_BITS 12
#define VBAT_DIVIDER_RATIO (2.0F)

#define PIN_NFC1 (9)
#define PIN_NFC2 (10)

/*
 * Serial: Serial1 is wired to the L76K GNSS receiver at 9600 baud.
 * The nRF RECEIVES on P1.09 and TRANSMITS on P1.08.
 */
#define PIN_SERIAL1_RX (32 + 9)
#define PIN_SERIAL1_TX (32 + 8)

#define PIN_GPS_WAKEUP (32 + 2)
#define PIN_GPS_RESET (32 + 5) // active low, hold >100 ms to reset
#define PIN_GPS_PPS (32 + 4)

/*
 * I2C -- shared by the BME280 and the PCF8563 RTC.
 */
#define WIRE_INTERFACES_COUNT 1
#define PIN_WIRE_SDA (26)
#define PIN_WIRE_SCL (27)

#define BME280_ADDRESS_DEFAULT 0x77 // 0x76 if SDO is tied low; confirm by scan
#define PCF8563_ADDRESS 0x51
#define PIN_RTC_INT (0 + 16) // open drain

/*
 * SPI0 -- SX1262 radio.
 */
#define SPI_INTERFACES_COUNT 2

#define PIN_SPI_MISO (0 + 23)
#define PIN_SPI_MOSI (0 + 22)
#define PIN_SPI_SCK (0 + 19)

#define SX126X_CS (0 + 24)
#define SX126X_RESET (0 + 25)
#define SX126X_BUSY (0 + 17)
#define SX126X_DIO1 (0 + 20)
#define SX126X_DIO3 (0 + 21)
// DIO2 is bonded inside the module to the TX/RX switch; it is not an MCU pin.
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_DIO3_TCXO_VOLTAGE 1.8

// Default chip select for SPI0. Arduino libraries expect SS to exist; without it
// SdFat (pulled in by TinyUSB's MSC support) does not compile. Must come after
// SX126X_CS -- the static const below expands it immediately.
#define PIN_SPI_SS SX126X_CS

/*
 * The four names every Arduino variant is expected to provide for its default
 * SPI bus. SS was already here because SdFat does not compile without it; the
 * other three were missing, and GxEPD2 does not compile without them -- it
 * builds every panel driver it ships, and the ones for multi-panel displays
 * construct an SPIClass from the bare names.
 *
 * They describe SPI0, the radio bus. The e-paper is on SPI1 and its pins are
 * spelled out separately below; nothing should reach for these to talk to the
 * panel.
 */
static const uint8_t SS = PIN_SPI_SS;
static const uint8_t MOSI = PIN_SPI_MOSI;
static const uint8_t MISO = PIN_SPI_MISO;
static const uint8_t SCK = PIN_SPI_SCK;

/*
 * SPI1 -- e-paper (GDEH0154D67 / SSD1681, 200x200).
 *
 * MISO is P1.06. Meshtastic uses P1.07 here and marks it a FIXME dummy; LilyGO
 * and cfr34k agree on P1.06. Whether the FPC actually routes it is unconfirmed.
 */
#define PIN_SPI1_MISO (32 + 6)
#define PIN_SPI1_MOSI (0 + 29)
#define PIN_SPI1_SCK (0 + 31)

#define PIN_EINK_CS (0 + 30)
#define PIN_EINK_DC (0 + 28)
#define PIN_EINK_RES (0 + 2)
#define PIN_EINK_BUSY (0 + 3)
#define PIN_EINK_BL (32 + 11) // front light

/*
 * External flash -- dedicated QSPI peripheral, no bus sharing.
 *
 * LilyGO fits either a ZD25WQ16B (JEDEC 0xBA6015) or an MX25R1635F
 * (0xC22815) depending on supply. Pass both device descriptors to
 * Adafruit_SPIFlash::begin() rather than guessing.
 */
#define EXTERNAL_FLASH_USE_QSPI
#define PIN_QSPI_SCK (32 + 14)
#define PIN_QSPI_CS (32 + 15)
#define PIN_QSPI_IO0 (32 + 12)
#define PIN_QSPI_IO1 (32 + 13)
#define PIN_QSPI_IO2 (0 + 7)
#define PIN_QSPI_IO3 (0 + 5)

#ifdef __cplusplus
}
#endif

#endif // _VARIANT_MAXL_TECHO_
