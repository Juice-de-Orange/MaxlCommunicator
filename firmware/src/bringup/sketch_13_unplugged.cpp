/*
 * Bring-up 13 -- the measurement that cannot be taken with the cable in.
 *
 * Three open questions all turn on the same thing, and none of them can be
 * answered while USB is attached:
 *
 *   D2      Does the PCF8563 have a supply of its own? PIN_PWR_ON low does not
 *           drop the rail while USB is there, because VBUS reaches the same
 *           latch node through D5.
 *   D13     What is the battery divider actually attached to? It reads 4807 and
 *           4821 mV with the cable in -- no lithium cell has that voltage.
 *   gate 1.6  The ADC against a multimeter across 3.3-4.2 V, which is not a range
 *           the reading even enters while VBUS is present.
 *
 * The awkward part is that unplugging the cable also takes away the only
 * diagnosis channel this board has. So this sketch does not try to report while
 * unplugged. It REMEMBERS, in RAM, and tells the story when the cable comes
 * back -- which works because the device keeps running on the battery the whole
 * time. That is the entire trick, and it needs no flash and no persistence.
 *
 * HOW TO RUN IT
 *
 *   1. tools/bringup_run.py 13
 *   2. Unplug USB. The device stays alive on the battery and keeps sampling,
 *      now with VBUS absent -- which is the reading that was never taken.
 *   3. Wait a minute or two. Longer is better; the log holds five minutes.
 *   4. Plug USB back in and run: tools/bringup_run.py 13 --no-flash
 *      The report shows every sample either side of the moment VBUS went away.
 *
 * AND THE SECOND HALF, FOR D2
 *
 *   5. Unplug again, then hold the user button for two seconds. The device
 *      powers itself off -- properly off, see below.
 *   6. Wait ten minutes, or overnight. The longer the better.
 *   7. Plug USB back in. The boot report says whether the clock survived.
 *
 * WHY POWERING OFF IS SAFE HERE
 *
 * P0.12 is not merely a peripheral enable. docs/hardware/pinmap.md section 3,
 * from the schematic: it drives Q5, which pulls the gates of Q6 (the main
 * BAT -> system switch) and Q8. Pulling it low on battery therefore cuts the
 * whole system, the MCU included.
 *
 * That sounds alarming and is exactly what makes it recoverable: the same node
 * is fed from VBUS through D5, so plugging the cable back in latches the system
 * on again. There is no state in which this sketch can leave a device that a
 * USB cable does not undo.
 *
 * It also means the shutdown must never be offered while USB is attached -- it
 * would do nothing at all, and "nothing happened" is the most confusing possible
 * answer to a button press.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 13

#include <Arduino.h>
#include <Wire.h>

#include "common.h"
#include "hal/battery_adc.h"
#include "hal/clock_pcf8563.h"
#include "hal/inputs_gpio.h"
#include "hal/led.h"
#include "report.h"

namespace {

hal::BatteryAdc g_battery;
hal::Pcf8563Clock g_clock;
hal::GpioInputs g_inputs;
hal::StatusLed g_led;

/// One sample a second. Five minutes is more than long enough to unplug, wait,
/// and plug back in, and 300 of these is 3.6 kB of the 248 kB available.
constexpr size_t kLogCapacity = 300;
constexpr uint32_t kSampleIntervalMs = 1000;

struct Sample {
    uint32_t uptimeS;
    uint32_t rtcUnix;
    uint16_t counts;
    uint16_t millivolts;
    bool vbus;
    bool rtcAnswers;
};

Sample g_log[kLogCapacity];
size_t g_logCount = 0;
size_t g_logNext = 0;
uint32_t g_lastSampleMs = 0;

/// What the boot looked like. This is the D2 answer, and it is only meaningful
/// on the boot that follows a real power-down.
uint32_t g_bootResetReason = 0;
bool g_bootRtcAnswers = false;
bool g_bootRtcVoltageLow = true;
uint32_t g_bootRtcUnix = 0;
bool g_bootVbus = false;

bool g_shutdownArmed = false;
uint32_t g_lastReportMs = 0;

/// True while the USB regulator sees VBUS. The one bit that says which side of
/// the experiment we are on.
bool vbusPresent()
{
    return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}

void takeSample()
{
    Sample &sample = g_log[g_logNext];
    sample.uptimeS = millis() / 1000u;
    sample.vbus = vbusPresent();
    sample.millivolts = g_battery.millivolts();
    sample.counts = g_battery.lastCounts();
    sample.rtcAnswers = g_clock.timeValid();
    sample.rtcUnix = g_clock.unixSeconds();

    g_logNext = (g_logNext + 1) % kLogCapacity;
    if (g_logCount < kLogCapacity) {
        ++g_logCount;
    }
}

/*
 * Cut the latch. On battery this powers the device down; with USB attached it
 * does nothing, which is why it is refused there.
 */
void powerOff()
{
    report::info("13: powering off -- plug USB back in to bring it up");
    Serial.flush();
    delay(100);

    // Three long blinks, so somebody watching the device knows it heard them
    // before it goes dark.
    for (int i = 0; i < 3; ++i) {
        g_led.blink(250);
        delay(150);
    }

    pinMode(PIN_PWR_ON, OUTPUT);
    digitalWrite(PIN_PWR_ON, LOW);

    // If we are still here a second later, the latch did not open -- which is
    // the answer for a device that still has VBUS on it.
    delay(1000);
    digitalWrite(PIN_PWR_ON, HIGH);
    report::info("13: still running -- the latch did not open, VBUS is holding it");
}

void printReport()
{
    report::begin(13);

    // --- the boot, which is where D2 is answered ---------------------------
    report::value("boot.reset_reason", "0x%08lX", static_cast<unsigned long>(g_bootResetReason));
    report::value("boot.cold", "%d", g_bootResetReason == 0 ? 1 : 0);
    report::value("boot.vbus", "%d", g_bootVbus ? 1 : 0);
    report::value("boot.rtc_answers", "%d", g_bootRtcAnswers ? 1 : 0);
    report::value("boot.rtc_vl_flag", "%d", g_bootRtcVoltageLow ? 1 : 0);
    report::value("boot.rtc_unix", "%lu", static_cast<unsigned long>(g_bootRtcUnix));
    report::info("D2: after a real power-down, vl_flag 0 and a correct time mean the "
                 "PCF8563 has a supply of its own. vl_flag 1 means it does not.");

    // --- the log, which is where D13 is answered ---------------------------
    report::value("log.samples", "%u", static_cast<unsigned>(g_logCount));
    report::value("vbus.now", "%d", vbusPresent() ? 1 : 0);

    /*
     * The two averages side by side are the whole point. If they differ by
     * roughly a volt, the divider is not on the cell and D13's reading of the
     * schematic is right. If they agree, it is on the cell and something else
     * explains 4807 mV.
     */
    uint32_t withSum = 0, withoutSum = 0;
    uint32_t withCount = 0, withoutCount = 0;
    uint16_t withoutMin = 0xFFFF, withoutMax = 0;

    const size_t start = (g_logCount == kLogCapacity) ? g_logNext : 0;
    for (size_t i = 0; i < g_logCount; ++i) {
        const Sample &sample = g_log[(start + i) % kLogCapacity];
        if (sample.vbus) {
            withSum += sample.millivolts;
            ++withCount;
        } else {
            withoutSum += sample.millivolts;
            ++withoutCount;
            if (sample.millivolts < withoutMin) {
                withoutMin = sample.millivolts;
            }
            if (sample.millivolts > withoutMax) {
                withoutMax = sample.millivolts;
            }
        }
    }

    report::value("battery.with_vbus_mv", "%lu",
                  static_cast<unsigned long>(withCount ? withSum / withCount : 0));
    report::value("battery.with_vbus_samples", "%lu", static_cast<unsigned long>(withCount));
    report::value("battery.no_vbus_mv", "%lu",
                  static_cast<unsigned long>(withoutCount ? withoutSum / withoutCount : 0));
    report::value("battery.no_vbus_samples", "%lu", static_cast<unsigned long>(withoutCount));
    report::value("battery.no_vbus_min_mv", "%u", withoutCount ? withoutMin : 0);
    report::value("battery.no_vbus_max_mv", "%u", withoutCount ? withoutMax : 0);

    if (withoutCount == 0) {
        report::info("13: no sample without VBUS yet -- unplug the cable, wait, plug it back");
    }

    // The transitions, which is where the interesting samples are.
    bool previous = g_logCount > 0 ? g_log[start].vbus : false;
    for (size_t i = 1; i < g_logCount; ++i) {
        const Sample &sample = g_log[(start + i) % kLogCapacity];
        if (sample.vbus != previous) {
            report::info("13: vbus %s at uptime %lus -- %u mV (%u counts), rtc %lu",
                         sample.vbus ? "back" : "gone",
                         static_cast<unsigned long>(sample.uptimeS), sample.millivolts,
                         sample.counts, static_cast<unsigned long>(sample.rtcUnix));
            previous = sample.vbus;
        }
    }

    report::value("shutdown.armed", "%d", g_shutdownArmed ? 1 : 0);
    if (vbusPresent()) {
        report::info("13: hold the user button to power off -- refused while USB is attached");
    } else {
        report::info("13: hold the user button for two seconds to power off");
    }

    report::verdict("inconclusive",
                    "this sketch collects; D2, D13 and gate 1.6 are read off the numbers "
                    "above and gate 1.6 still wants a multimeter");
    report::end(13);
}

} // namespace

namespace bringup {

void setup()
{
    // Generous: this image is meant to be left running across an unplug.
    commonSetup(300000);
    Wire.begin();

    g_led.begin();
    g_battery.begin();
    g_inputs.begin();

    /*
     * Read the clock before anything else touches it. On the boot that follows a
     * real power-down this is the D2 answer, and nothing later in this sketch
     * can produce it again.
     */
    g_bootRtcAnswers = g_clock.begin();
    g_bootRtcVoltageLow = g_clock.voltageLowAtBoot();
    g_bootRtcUnix = g_clock.unixSeconds();
    g_bootResetReason = NRF_POWER->RESETREAS;
    g_bootVbus = vbusPresent();

    // Cleared so the NEXT boot's reason is about the next boot. RESETREAS is
    // cumulative until somebody writes the bits back.
    NRF_POWER->RESETREAS = 0xFFFFFFFFu;

    // PIN_PWR_ON must be held high or the device turns itself off the moment
    // USB goes away. initVariant() does this; it is restated because this sketch
    // is the one that deliberately drives it low.
    pinMode(PIN_PWR_ON, OUTPUT);
    digitalWrite(PIN_PWR_ON, HIGH);

    takeSample();
}

void loop()
{
    const uint32_t now = millis();

    if (static_cast<uint32_t>(now - g_lastSampleMs) >= kSampleIntervalMs) {
        g_lastSampleMs = now;
        takeSample();
    }

    const hal::InputEvent event = g_inputs.poll(now);
    if (event.id == hal::InputId::Button) {
        if (event.event == hal::ButtonEvent::LongPressArmed) {
            g_shutdownArmed = true;
            g_led.set(true);
        } else if (event.event == hal::ButtonEvent::LongPress && g_shutdownArmed) {
            g_shutdownArmed = false;
            g_led.set(false);
            if (vbusPresent()) {
                // Refused rather than attempted. With VBUS on the latch node the
                // attempt does nothing, and a button that silently does nothing
                // is the worst answer available.
                report::info("13: refused -- unplug USB first, the latch is held by VBUS");
            } else {
                powerOff();
            }
        }
    }

    if (usbReady() && static_cast<uint32_t>(now - g_lastReportMs) >= 5000u) {
        g_lastReportMs = now;
        printReport();
    }

    delay(20);
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 13
