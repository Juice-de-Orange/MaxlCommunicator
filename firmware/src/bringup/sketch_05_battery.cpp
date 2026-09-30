/*
 * Bring-up 05 -- battery sense and the USB supply state.
 *
 * P0.04 is AIN2, behind an external 2:1 divider (docs/hardware/pinmap.md). Gate
 * 1.6 wants this within 50 mV of a multimeter across 3.3-4.2 V, which needs an
 * instrument and a human, so this sketch cannot close it. What it can do is
 * establish that the channel reads something physical rather than a floating
 * pin, and record the number so the morning's comparison has something to
 * compare against.
 *
 * The reading is only meaningful together with the charge state: on USB the cell
 * sits at the charger's constant-voltage point, not at its resting voltage, so a
 * reading taken with a cable attached says little about capacity. USBREGSTATUS
 * gives that context straight from the hardware.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 5

#include <Arduino.h>

#include "common.h"
#include "report.h"

namespace {

/// The divider halves the cell voltage, so the ADC sees at most ~2.1 V from a
/// 4.2 V cell -- comfortably inside the 3.0 V internal reference.
constexpr float kDividerRatio = 2.0f;
constexpr float kReferenceMv = 3000.0f;
constexpr int kAdcMax = 4095; // 12-bit

/// Averaged: a single conversion on a rail shared with a switching charger is
/// noisy enough to move the last two digits, and those digits are the ones gate
/// 1.6 is about.
constexpr int kSamples = 64;

uint16_t readRaw()
{
    uint32_t total = 0;
    for (int i = 0; i < kSamples; ++i) {
        total += static_cast<uint32_t>(analogRead(PIN_A0));
        delay(1);
    }
    return static_cast<uint16_t>(total / kSamples);
}

} // namespace

namespace bringup {

void setup()
{
    commonSetup(120000);
    analogReference(AR_INTERNAL_3_0);
    analogReadResolution(12);
    // The first conversion after switching reference is unreliable.
    (void)analogRead(PIN_A0);
}

void loop()
{
    if (usbReady()) {
        const uint16_t raw = readRaw();
        const float atPin = (static_cast<float>(raw) / kAdcMax) * kReferenceMv;
        const float battery = atPin * kDividerRatio;

        const uint32_t usbReg = NRF_POWER->USBREGSTATUS;
        const bool vbusPresent = (usbReg & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
        const bool outputReady = (usbReg & POWER_USBREGSTATUS_OUTPUTRDY_Msk) != 0;

        report::begin(5);
        report::info("ain2=P0.04, external 2:1 divider, ref=internal 3.0 V, 12 bit, %d samples",
                     kSamples);
        report::value("adc.raw", "%u", static_cast<unsigned>(raw));
        report::value("adc.at_pin_mv", "%.1f", static_cast<double>(atPin));
        report::value("battery.mv", "%.0f", static_cast<double>(battery));
        report::value("usb.vbus_present", "%d", vbusPresent ? 1 : 0);
        report::value("usb.output_ready", "%d", outputReady ? 1 : 0);

        // A floating pin reads near zero or rails; a real divider on a lithium
        // cell cannot be outside this range while the device is running at all.
        const bool plausible = battery > 3000.0f && battery < 4400.0f;
        report::value("battery.plausible", "%d", plausible ? 1 : 0);
        report::verdict(plausible ? "inconclusive" : "fail",
                        plausible
                            ? "channel reads a plausible cell voltage; gate 1.6 needs a "
                              "multimeter and three points across 3.3-4.2 V"
                            : "reading is outside any voltage a running lithium cell can have");
        report::end(5);
    }

    delay(3000);
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 5
