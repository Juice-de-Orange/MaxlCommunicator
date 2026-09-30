/*
 * The pin map, with the evidence attached.
 *
 * CLAUDE.md 5, phase 0: "Deliver a hal/board.h that is known-correct." What
 * makes this file different from variants/t-echo/variant.h is not the numbers --
 * they are the same numbers -- but that every one of them carries the bring-up
 * sketch and the date that confirmed it on THIS board. docs/hardware/pinmap.md
 * explains where each number came from and which sources disagreed;
 * docs/test-results/ holds the runs.
 *
 * A pin with no evidence line is marked UNCONFIRMED and says what would confirm
 * it. That is the whole point of the file: "the vendor header says so" is a
 * source, not a confirmation, and this project has already been bitten by the
 * difference (docs/hardware/pinmap.md section 4, the LED colour argument, where
 * the schematic net labels and LilyGO's own header contradict each other).
 *
 * Verified on one board (node A). A second board must be re-checked
 * rather than assumed: section 1 of the pin map records two hardware revisions
 * whose LED pins are another peripheral's data lines.
 *
 * ---------------------------------------------------------------------------
 * THREE PINS THAT COST HARDWARE
 *
 *   P0.13  REG_EN, the 3.3 V regulator the nRF52840 itself runs from.
 *          Driven HIGH or left alone. NEVER LOW. Not defined below, on purpose:
 *          nothing in the application has a reason to name it.
 *
 *   P1.01  On VERSION_1 boards these are LoRa DIO0 and ePaper MISO, not LEDs.
 *   P1.03  Untouched until the revision is settled. The revision-safe status LED
 *          is P0.14 and it is the only one this project drives.
 *
 *   P0.18  Wired to nRESET (CLAUDE.md 1.7). Not available to the application,
 *          not defined here, and no UICR register is ever written -- reassigning
 *          PSELRESET would cost the double-tap route into the bootloader, which
 *          without an SWD probe is the only rescue line this project has.
 * ---------------------------------------------------------------------------
 */

#ifndef MAXL_HAL_BOARD_H
#define MAXL_HAL_BOARD_H

#include <stdint.h>

namespace hal {
namespace board {

/*
 * ===========================================================================
 * CONFIRMED -- a sketch ran on node A and the peripheral answered
 * ===========================================================================
 */

// --- I2C (sketch 02, 2026-08-31) -------------------------------------------
// Exactly two devices answered on this bus and both identified themselves.
constexpr uint8_t kPinI2cSda = 26;          // P0.26
constexpr uint8_t kPinI2cScl = 27;          // P0.27

/// BME280. Chip id 0x60 read back -- 0x58 would have been a BMP280 with no
/// humidity sensor, and the field would have gone out as a silent zero.
constexpr uint8_t kAddrBme280 = 0x77;
constexpr uint8_t kChipIdBme280 = 0x60;

/// PCF8563. VL flag clear, so the chip trusts the time it is holding.
constexpr uint8_t kAddrPcf8563 = 0x51;

// --- External flash, QSPI (sketch 04 and 10, 2026-08-31) -------------------
// ZD25WQ16B, JEDEC 0xBA6015, 2 097 152 bytes. 100 write/reboot cycles with no
// record lost; LittleFS mounts through hal::LittleFsBlockStore and all four
// regions round-trip. Deep power-down is NOT confirmed -- see decision D12.
constexpr uint8_t kPinQspiSck = 32 + 14;    // P1.14
constexpr uint8_t kPinQspiCs = 32 + 15;     // P1.15
constexpr uint8_t kPinQspiIo0 = 32 + 12;    // P1.12
constexpr uint8_t kPinQspiIo1 = 32 + 13;    // P1.13
constexpr uint8_t kPinQspiIo2 = 7;          // P0.07
constexpr uint8_t kPinQspiIo3 = 5;          // P0.05
constexpr uint32_t kJedecZd25wq16b = 0xBA6015;
constexpr uint32_t kExternalFlashBytes = 2097152;

// --- SX1262, SPI0 (sketch 07, 2026-08-31, read only) -----------------------
// BUSY handshakes and the sync word register reads its reset value 0x1424.
// Nothing has been transmitted from this board yet.
constexpr uint8_t kPinRadioCs = 24;         // P0.24
constexpr uint8_t kPinRadioSck = 19;        // P0.19
constexpr uint8_t kPinRadioMosi = 22;       // P0.22
constexpr uint8_t kPinRadioMiso = 23;       // P0.23
constexpr uint8_t kPinRadioReset = 25;      // P0.25
constexpr uint8_t kPinRadioBusy = 17;       // P0.17
constexpr uint8_t kPinRadioDio1 = 20;       // P0.20
/// DIO3 supplies the TCXO at 1.8 V (CLAUDE.md phase 2). DIO2 is the RF switch
/// and is internal to the module -- it is not an MCU pin and must not be one.
constexpr uint8_t kPinRadioDio3Tcxo = 21;   // P0.21

// --- GNSS L76K, UART1 (sketch 06, 2026-08-31) ------------------------------
// 4391 bytes, 79 sentences, 11 satellites in view and a valid fix, indoors.
// Holding reset low silences the module completely: 0 bytes while asserted,
// 1207 after release. That is what hal::GnssL76k::powerOff() relies on.
//
// The direction of the pair is the one thing in the pin map with a warning:
// Meshtastic's comments contradict its own defines, and the defines are right.
constexpr uint8_t kPinGnssRx = 32 + 9;      // P1.09, nRF receives here
constexpr uint8_t kPinGnssTx = 32 + 8;      // P1.08, nRF transmits here
constexpr uint8_t kPinGnssWakeup = 32 + 2;  // P1.02
constexpr uint8_t kPinGnssReset = 32 + 5;   // P1.05, active low
constexpr uint8_t kPinGnssPps = 32 + 4;     // P1.04
constexpr uint32_t kGnssBaud = 9600;

// --- E-paper, SPI1 (sketch 08 and 11, 2026-08-31) --------------------------
// SSD1681 through GxEPD2_154_D67, 200x200, partial refresh working.
// SPI1 and not SPI0: sharing a bus between a radio that must answer an
// interrupt and a panel that holds it for 300 ms is not a trade worth making.
constexpr uint8_t kPinEpaperCs = 30;        // P0.30
constexpr uint8_t kPinEpaperDc = 28;        // P0.28
constexpr uint8_t kPinEpaperReset = 2;      // P0.02
constexpr uint8_t kPinEpaperBusy = 3;       // P0.03
constexpr uint8_t kPinEpaperSck = 31;       // P0.31
constexpr uint8_t kPinEpaperMosi = 29;      // P0.29
constexpr uint8_t kPinEpaperFrontLight = 32 + 11; // P1.11
constexpr uint16_t kPanelWidth = 200;
constexpr uint16_t kPanelHeight = 200;

/// UNCONFIRMED. The vendor header and cfr34k say P1.06; Meshtastic uses P1.07
/// with an explicit FIXME. Whether either is electrically connected to the FPC
/// is unknown, and open decision D1 exists because gate 0.2's "read back the
/// SSD1681 id" needs this line. Nothing in this project reads from the panel.
constexpr uint8_t kPinEpaperMisoUnconfirmed = 32 + 6; // P1.06

// --- Status LED (sketch 01, 2026-08-30) ------------------------------------
// Common anode to VDD_POWR: the pin sinks, so LOW is lit.
// P0.14 is the ONLY pin that is an LED on both hardware revisions.
constexpr uint8_t kPinStatusLed = 14;       // P0.14
constexpr uint8_t kLedOnLevel = 0;          // active low

/*
 * ===========================================================================
 * UNCONFIRMED -- named here so nothing goes looking in variant.h instead
 * ===========================================================================
 */

// --- Inputs ----------------------------------------------------------------
/// User button, active low with a pull-up. Every source agrees; no sketch has
/// yet counted a press. Bring-up sketch 09 does, and it settles gate 1.4.
constexpr uint8_t kPinButton = 32 + 10;     // P1.10

/*
 * Capacitive touch pad, TTP223.
 *
 * POLARITY: measured but not yet proven. Bring-up sketch 11 (2026-08-31) found
 * the pad RESTING HIGH across 16 samples with nobody near the device, which
 * makes it ACTIVE LOW -- so Meshtastic's ACTIVE_LOW define is right and LilyGO's
 * header, and Meshtastic's own comment beside its define, are wrong.
 *
 * A resting level alone is not proof: a floating input reads something too. The
 * counter-check is a press that demonstrably inverts it, and that is sketch 09.
 *
 * hal::GpioInputs defaults to the vendor header's reading, records the level the
 * pad rests at during begin(), and hal::PressDetector refuses to report a
 * release it never saw begin. Getting it backwards therefore produces a pad that
 * does nothing -- visible and harmless -- rather than a device that changes
 * screen whenever a hand comes near it.
 *
 * Sketch 09 reports the resting level and settles it. Gate 1.5.
 */
constexpr uint8_t kPinTouch = 11;           // P0.11

/// Still the vendor header's reading until sketch 09 confirms the inversion.
/// Getting this wrong in this direction costs a pad that does nothing; getting
/// it wrong the other way costs a device that changes screen by itself.
constexpr bool kTouchActiveLowDefault = false;

/// What sketch 11 measured with nobody touching it. If sketch 09 confirms that a
/// press inverts this, kTouchActiveLowDefault becomes true.
constexpr bool kTouchRestingLevelMeasured = true; // HIGH

// --- Battery sense ---------------------------------------------------------
/*
 * P0.04 is AIN2 behind an external 2:1 divider. The pin is right; what is on
 * the other side of the divider is NOT settled.
 *
 * Sketch 05 read 4807 mV with USB attached and sketch 11 read 4821 mV through a
 * different ADC reference -- no lithium cell has that voltage, and the two agree.
 * The likely explanation is that the divider sits on the latch node VBUS feeds
 * through D5, so with a cable in it reads the USB rail minus a diode drop rather
 * than the cell. The same structure already explains why the RTC keeps answering
 * when PIN_PWR_ON goes low. See decisions D13 and D2: one run on battery alone
 * answers both.
 *
 * Until then a reading taken with USB attached is not a cell voltage.
 */
constexpr uint8_t kPinBatteryAdc = 4;       // P0.04 / AIN2
constexpr uint8_t kBatteryDividerRatio = 2;

// --- Power rails -----------------------------------------------------------
/*
 * PIN_PWR_ON (P0.12) latches the peripheral rail. Driving it low does NOT drop
 * the rail while USB is attached, confirmed by sketch 03: the PCF8563 keeps
 * answering, and VL stays clear across the attempt. Named here because it has to
 * be driven HIGH at boot, and marked because what it does when the cable is out
 * has never been observed.
 *
 * P0.13 (REG_EN) is deliberately absent. See the header of this file.
 */
constexpr uint8_t kPinPeripheralPower = 12; // P0.12

/// P0.15 is not unconnected, whatever is often claimed: the net runs to pin 6 of
/// the SX1262 module, where it is NC on this variant and A7682_PWR on another.
/// Left as an input. Never driven.
constexpr uint8_t kPinDoNotDrive = 15;      // P0.15

} // namespace board
} // namespace hal

#endif // MAXL_HAL_BOARD_H
