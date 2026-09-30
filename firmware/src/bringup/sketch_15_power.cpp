/*
 * Bring-up 15 -- the power measurement scaffold.
 *
 * CLAUDE.md 3.1 gives four states and a current target for each, and phase 5
 * asks for all of them measured rather than calculated. docs/test-plan.md 5.5
 * wants a per-subsystem breakdown. None of that is code -- it is somebody with a
 * meter -- and the reason it takes an afternoon rather than a quarter of an hour
 * is that holding the device in a known state long enough to read is fiddly.
 *
 * This holds each state for thirty seconds, in a fixed order, for ever. Count
 * the blinks, read the meter, wait for the next.
 *
 * It also carries the battery log that used to live in bring-up 13, so that one
 * unplugging of the cable covers gates 0.4, 1.6 and 5.1 to 5.4 and decision D13
 * together. Bring-up 13 stays for D2, because that one ends by switching the
 * device off.
 *
 *   1 blink   ACTIVE            everything on, CPU busy          ~15 mA expected
 *   2 blinks  GNSS_FIX          the L76K powered and searching   ~40 mA
 *   3 blinks  IDLE, flash AWAKE  panel asleep, MCU asleep         -- see below
 *   4 blinks  DEEP_IDLE          the same, flash in power-down    < 20 uA target
 *   5 blinks  SNIFF              SX1262 RxDutyCycle, MCU asleep   see 2.3's table
 *
 * THE THIRD AND FOURTH STATES ARE GATE 0.4
 *
 * They differ in exactly one thing: whether the external flash was told to enter
 * deep power-down. The difference between the two readings IS the flash's
 * contribution, which is what gate 0.4 asks for and what decision D12 could not
 * settle from inside the firmware.
 *
 * D12 is worth re-reading before taking the measurement. Sketch 04 sent the
 * power-down command and the chip still answered its JEDEC id afterwards. That
 * has two explanations -- it never slept, or the act of asking woke it -- and
 * they are indistinguishable from software. A meter distinguishes them in one
 * reading, because a sleeping chip draws about 12 uA less than a waking one.
 *
 * MEASURE WITH THE CABLE OUT
 *
 * USB draws current of its own and holds the peripheral latch through D5, so a
 * reading taken with the cable in is a reading of the wrong thing. The device
 * keeps running on the battery; the cycle continues; the blinks still count.
 * Plug it back in afterwards and this will still be here.
 *
 * WHAT IS IN THE MEASUREMENT THAT IS NOT THE STATE
 *
 * The dead man's timer's task wakes four times a second for a few microseconds,
 * and this sketch keeps it armed on purpose -- unattended is exactly when a hang
 * is expensive. It is small and it is real, and it is named here rather than
 * left for somebody to wonder about.
 *
 * The LED is OFF during every measurement window. It blinks only in the second
 * before a state begins, because at a couple of milliamps it would swamp a
 * twenty-microamp reading.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 15

#include <Arduino.h>
#include <Wire.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <Adafruit_SPIFlash.h>
#pragma GCC diagnostic pop

#include "common.h"
#include "hal/battery_adc.h"
#include "hal/clock_pcf8563.h"
#include "hal/deadman.h"
#include "hal/display_ssd1681.h"
#include "hal/gnss_l76k.h"
#include "hal/led.h"
#include "hal/radio_sx1262.h"
#include "link/airtime.h"
#include "report.h"

namespace {

Adafruit_FlashTransport_QSPI g_transport;
Adafruit_SPIFlash g_flash(&g_transport);
const SPIFlash_Device_t kFlashCandidates[] = {ZD25WQ16B, MX25R1635F};

hal::DisplaySsd1681 g_display;
hal::GnssL76k g_gnss;
hal::StatusLed g_led;
hal::Sx1262Radio g_radio;
hal::BatteryAdc g_battery;
hal::Pcf8563Clock g_clock;
hal::Canvas g_canvas;

/*
 * The battery reading, with and without VBUS -- folded in from bring-up 13 so
 * that one unplugging covers everything the meter is out for.
 *
 * Decision D13: with the cable in, this divider reads 4807 and 4821 mV, and no
 * lithium cell has that voltage. The two averages side by side are the answer.
 * The device keeps running on the battery, so nothing is lost by pulling the
 * cable -- and this sketch was already going to run for minutes at a time.
 */
struct BatteryLog {
    uint32_t withVbusSum = 0;
    uint32_t withVbusCount = 0;
    uint32_t noVbusSum = 0;
    uint32_t noVbusCount = 0;
    uint16_t noVbusMin = 0xFFFF;
    uint16_t noVbusMax = 0;
    uint16_t lastMv = 0;
    uint16_t lastCounts = 0;
    uint32_t transitions = 0;
    bool lastVbus = false;
    bool seenAny = false;
};

BatteryLog g_batteryLog;
uint32_t g_lastBatterySampleMs = 0;

/// One a second while a state is being held. At 30 s a state that is thirty
/// readings per window, which is plenty to average and cheap to take.
constexpr uint32_t kBatteryIntervalMs = 1000;

/// True while the USB regulator sees VBUS -- the one bit that says which side of
/// the experiment the reading is on.
bool vbusPresent()
{
    return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}

void sampleBattery()
{
    BatteryLog &log = g_batteryLog;
    const bool vbus = vbusPresent();
    const uint16_t mv = g_battery.millivolts();

    log.lastMv = mv;
    log.lastCounts = g_battery.lastCounts();

    if (log.seenAny && vbus != log.lastVbus) {
        ++log.transitions;
    }
    log.lastVbus = vbus;
    log.seenAny = true;

    if (mv == 0) {
        return;
    }
    if (vbus) {
        log.withVbusSum += mv;
        ++log.withVbusCount;
    } else {
        log.noVbusSum += mv;
        ++log.noVbusCount;
        if (mv < log.noVbusMin) {
            log.noVbusMin = mv;
        }
        if (mv > log.noVbusMax) {
            log.noVbusMax = mv;
        }
    }
}

/// Long enough to settle a meter and read it without hurrying.
constexpr uint32_t kHoldMs = 30000;

/// Datasheet commands. Sketch 04 uses the same two.
constexpr uint8_t kCmdDeepPowerDown = 0xB9;
constexpr uint8_t kCmdReleaseDeepPowerDown = 0xAB;

enum class State : uint8_t {
    Active = 1,
    GnssFix,
    IdleFlashAwake,
    DeepIdle,
    Sniff,
    Count,
};

bool g_flashOk = false;
bool g_radioOk = false;
uint8_t g_cycles = 0;

const char *stateName(State state)
{
    switch (state) {
    case State::Active: return "ACTIVE";
    case State::GnssFix: return "GNSS_FIX";
    case State::IdleFlashAwake: return "IDLE_FLASH_AWAKE";
    case State::DeepIdle: return "DEEP_IDLE";
    case State::Sniff: return "SNIFF";
    case State::Count: break;
    }
    return "?";
}

/// N short blinks, then dark. The dark part is the measurement.
void announce(State state)
{
    for (uint8_t i = 0; i < static_cast<uint8_t>(state); ++i) {
        g_led.set(true);
        delay(120);
        g_led.set(false);
        delay(180);
    }
    g_led.set(false);
}

/*
 * Sleep for `ms` the way CLAUDE.md 3.1 asks for: System-ON with an RTC wakeup,
 * not a delay loop.
 *
 * delay() on this core is vTaskDelay, and gate 0.7 confirmed the core's FreeRTOS
 * has tickless idle -- so a blocked task lets the CPU stop rather than spin. The
 * measurement is of that, which is the state the device actually spends its life
 * in.
 */
void sleepFor(uint32_t ms)
{
    /*
     * Broken into one-second naps so the battery can be sampled through the
     * window. Each nap is still a vTaskDelay and gate 0.7 confirmed the core's
     * FreeRTOS has tickless idle, so the CPU still stops between them -- an ADC
     * conversion once a second is microseconds against a second of sleep.
     */
    const uint32_t until = millis() + ms;
    while (static_cast<int32_t>(millis() - until) < 0) {
        delay(kBatteryIntervalMs);
        sampleBattery();
    }
}

void enterActive()
{
    // Deliberately busy: this is the state a button press or an arriving frame
    // puts the device in, and it is meant to be the expensive one.
    const uint32_t until = millis() + kHoldMs;
    volatile uint32_t churn = 0;
    while (static_cast<int32_t>(millis() - until) < 0) {
        for (uint32_t i = 0; i < 20000; ++i) {
            churn = churn + i;
        }
        if (static_cast<uint32_t>(millis() - g_lastBatterySampleMs) >= kBatteryIntervalMs) {
            g_lastBatterySampleMs = millis();
            sampleBattery();
        }
    }
    (void)churn;
}

void enterGnssFix()
{
    g_gnss.requestFix(kHoldMs + 5000, millis());
    const uint32_t until = millis() + kHoldMs;
    while (static_cast<int32_t>(millis() - until) < 0) {
        g_gnss.poll(millis());
        delay(20);
        if (static_cast<uint32_t>(millis() - g_lastBatterySampleMs) >= kBatteryIntervalMs) {
            g_lastBatterySampleMs = millis();
            sampleBattery();
        }
    }
    g_gnss.powerOff();
}

void enterIdle(bool flashAsleep)
{
    if (g_flashOk) {
        g_transport.runCommand(flashAsleep ? kCmdDeepPowerDown : kCmdReleaseDeepPowerDown);
        delay(5);
    }
    sleepFor(kHoldMs);
    if (g_flashOk && flashAsleep) {
        g_transport.runCommand(kCmdReleaseDeepPowerDown);
        delay(5);
    }
}

void enterSniff()
{
    if (!g_radioOk) {
        // Nothing to measure, and saying so beats holding a state that is not
        // the state.
        sleepFor(kHoldMs);
        return;
    }

    /*
     * CLAUDE.md 2.3's default: 2 s sniff interval, 8 symbols for SF <= 10.
     *
     * The preamble has to be set first, because that is what the SX1262 derives
     * its sleep period from -- the interval is the consequence. Without it this
     * measured continuous receive and reported it as SNIFF, which would have put
     * a number in the phase 5 breakdown that no shipping configuration produces.
     */
    g_radio.setPreambleLength(link::preambleSymbolsForInterval(9, 2000));
    g_radio.startReceiveDutyCycle(8);
    if (!g_radio.rxDutyCycleActive()) {
        report::info("15: RxDutyCycle did not engage -- this reading is continuous receive");
    }
    sleepFor(kHoldMs);
    g_radio.sleep();
}

void printPlan()
{
    report::begin(15);
    report::value("flash.ready", "%d", g_flashOk ? 1 : 0);
    report::value("radio.ready", "%d", g_radioOk ? 1 : 0);
    report::value("hold_ms", "%lu", static_cast<unsigned long>(kHoldMs));
    report::value("cycles_done", "%u", g_cycles);

    for (uint8_t i = 1; i < static_cast<uint8_t>(State::Count); ++i) {
        report::info("15: %u blink(s) -> %s", i, stateName(static_cast<State>(i)));
    }

    // --- the battery log, folded in from bring-up 13 -----------------------
    const BatteryLog &b = g_batteryLog;
    report::value("battery.now_mv", "%u", b.lastMv);
    report::value("battery.now_counts", "%u", b.lastCounts);
    report::value("battery.vbus_now", "%d", vbusPresent() ? 1 : 0);
    report::value("battery.with_vbus_mv", "%lu",
                  static_cast<unsigned long>(b.withVbusCount ? b.withVbusSum / b.withVbusCount : 0));
    report::value("battery.with_vbus_samples", "%lu", static_cast<unsigned long>(b.withVbusCount));
    report::value("battery.no_vbus_mv", "%lu",
                  static_cast<unsigned long>(b.noVbusCount ? b.noVbusSum / b.noVbusCount : 0));
    report::value("battery.no_vbus_samples", "%lu", static_cast<unsigned long>(b.noVbusCount));
    report::value("battery.no_vbus_min_mv", "%u", b.noVbusCount ? b.noVbusMin : 0);
    report::value("battery.no_vbus_max_mv", "%u", b.noVbusCount ? b.noVbusMax : 0);
    report::value("battery.vbus_transitions", "%lu", static_cast<unsigned long>(b.transitions));
    report::value("clock.unix", "%lu", static_cast<unsigned long>(g_clock.unixSeconds()));

    if (b.noVbusCount == 0) {
        report::info("15: no reading without VBUS yet -- gate 1.6 and D13 need the cable OUT");
    } else {
        report::info("15: D13 -- a volt between with_vbus and no_vbus means the divider is "
                     "not on the cell");
    }

    report::info("15: gate 0.4 is the difference between IDLE_FLASH_AWAKE and DEEP_IDLE");
    report::info("15: measure with the cable OUT -- USB draws current and holds the latch");
    report::info("15: the LED is dark during every window; it only blinks between them");

    if (!g_radioOk) {
        report::info("15: the SX1262 did not come up -- SNIFF holds an idle state instead");
    }

    report::verdict("inconclusive",
                    "this sketch holds states; gates 0.4 and 5.1 to 5.4 are read off a "
                    "meter, which is not something firmware can do for itself");
    report::end(15);
}

} // namespace

namespace bringup {

void setup()
{
    // One full cycle is five states of thirty seconds plus the blinks. Ten
    // minutes leaves room for two of them between pokes.
    commonSetup(600000);
    Wire.begin();

    g_led.begin();
    g_battery.begin();
    g_clock.begin();
    g_gnss.begin();  // powers the module DOWN, per CLAUDE.md 1.5
    sampleBattery();

    g_flashOk = g_flash.begin(kFlashCandidates,
                              sizeof(kFlashCandidates) / sizeof(kFlashCandidates[0]));

    /*
     * The panel is put to sleep and left there. Its own consumption with the
     * image standing is what CLAUDE.md 1.6 relies on -- e-paper holds a picture
     * with no power at all -- and a screen being redrawn during a current
     * measurement would be measuring the redraw.
     */
    if (g_display.begin()) {
        g_canvas.clear();
        g_canvas.text(4, 40, "POWER MEASUREMENT");
        g_canvas.text(4, 60, "sketch 15");
        g_canvas.text(4, 88, "count the blinks:");
        g_canvas.text(4, 104, "1 ACTIVE   2 GNSS");
        g_canvas.text(4, 120, "3 IDLE     4 DEEP");
        g_canvas.text(4, 136, "5 SNIFF");
        g_canvas.text(4, 164, "cable OUT to measure");
        g_display.present(g_canvas, hal::RefreshKind::Full);
        g_display.sleep();
    }

    /*
     * The radio is brought up for the SNIFF state only, and it never transmits:
     * startReceiveDutyCycle and sleep are the only two calls this sketch makes.
     * CLAUDE.md 2.5's rendezvous configuration, so the number means something
     * against the table in 2.3.
     */
    const hal::Modulation rendezvous{9, 869575000u, 22};
    g_radioOk = g_radio.begin(rendezvous);
    if (g_radioOk) {
        g_radio.sleep();
    }
}

void loop()
{
    if (usbReady()) {
        printPlan();
    }

    for (uint8_t i = 1; i < static_cast<uint8_t>(State::Count); ++i) {
        const State state = static_cast<State>(i);

        // Poked before every window, so a cycle that takes minutes does not walk
        // into the timer -- and a cycle that stops walking still does.
        hal::deadman::poke();

        if (usbReady()) {
            report::info("15: entering %s for %lu ms", stateName(state),
                         static_cast<unsigned long>(kHoldMs));
            Serial.flush();
        }

        announce(state);

        switch (state) {
        case State::Active: enterActive(); break;
        case State::GnssFix: enterGnssFix(); break;
        case State::IdleFlashAwake: enterIdle(false); break;
        case State::DeepIdle: enterIdle(true); break;
        case State::Sniff: enterSniff(); break;
        case State::Count: break;
        }
    }

    ++g_cycles;
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 15
