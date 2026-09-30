/*
 * Bring-up 12 -- the whole device, minus the radio, on the panel and under a
 * thumb.
 *
 * Everything below this line has been tested somewhere. hal/ was proven by
 * sketches 02 to 11, app/ and ui/ on the host against fakes. What has never
 * happened is all of it running together on the board, with the real panel, the
 * real touch pad and the real clock -- and "each part is tested" is not the same
 * claim as "the parts fit". Sketch 11 was the same argument one layer down; it
 * found a hang that would have stopped every device at boot.
 *
 * The radio is deliberately absent. app::Node runs without one on purpose (see
 * its attachRadio comment): a node with no radio cannot transmit, which is not
 * the same as a node that cannot run. Pointing two radios at each other needs a
 * second device, and there is only one.
 *
 * WHY THIS IS A BRING-UP IMAGE AND NOT src/main.cpp
 *
 * Only bring-up images carry the dead man's timer. An application image that
 * hangs before USB comes up needs a thumb on the reset button, and the whole
 * point of this session is that there is nobody in the room. main.cpp gets this
 * wiring when somebody is present to catch it.
 *
 * WHAT IT IS FOR
 *
 *   - The five screens on the real glass, at the real refresh times.
 *   - The touch pad walking the cycle and the button forcing a refresh, which is
 *     the CLAUDE.md 3.2 map with real hardware underneath it.
 *   - docs/test-plan.md gate 4.1 as a number: partial refreshes performed
 *     against seconds elapsed with nothing changing. The counters are reported
 *     every ten seconds, so a long run answers it without anybody watching.
 *   - Gate 1.4 and 1.5 fall out of the same run: every press is counted.
 *
 * The report repeats. Sketches 04, 08 and 10 each passed on the device and
 * looked like a timeout from the host because theirs did not.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 12

#include <Arduino.h>
#include <Wire.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <Adafruit_SPIFlash.h>
#pragma GCC diagnostic pop

#include "app/node.h"
#include "app/view.h"
#include "common.h"
#include "hal/battery_adc.h"
#include "hal/block_store_littlefs.h"
#include "hal/external_flash.h"
#include "hal/clock_pcf8563.h"
#include "hal/display_ssd1681.h"
#include "hal/gnss_l76k.h"
#include "hal/inputs_gpio.h"
#include "hal/key_store_internal.h"
#include "hal/led.h"
#include "hal/sensor_bme280.h"
#include "link/self_test.h"
#include "report.h"
#include "ui/ui.h"

namespace {

Adafruit_FlashTransport_QSPI g_transport;
Adafruit_SPIFlash g_flash(&g_transport);

hal::LittleFsBlockStore g_store;
hal::Pcf8563Clock g_clock;
hal::InternalKeyStore g_keys;
hal::DisplaySsd1681 g_display;
hal::Bme280Sensor g_sensor;
hal::BatteryAdc g_battery;
hal::GpioInputs g_inputs;
hal::GnssL76k g_gnss;
hal::StatusLed g_led;

app::Node g_node{g_clock, g_store, g_keys};
app::Peripherals g_peripherals;
ui::Ui g_ui;

/*
 * The view model is a file-scope object, not a local.
 *
 * The core gives loop() a 4 kB FreeRTOS stack (LOOP_STACK_SZ in
 * cores/nRF5/main.cpp, 1024 words). ui::ViewModel is around 450 bytes and
 * building one on the stack next to everything else this loop does is a needless
 * fraction of that budget to spend every pass.
 */
ui::ViewModel g_model;

bool g_ready = false;
bool g_storeOk = false;
bool g_nodeOk = false;

/// The newest received message the user has seen. ui::Action::MarkAllRead moves
/// it; nothing else does.
uint32_t g_lastReadCounter = 0;

uint32_t g_lastSensorMs = 0;
uint32_t g_lastReportMs = 0;
uint32_t g_bootUnix = 0;
uint32_t g_actionsHandled = 0;
uint32_t g_screenChanges = 0;

/// docs/test-plan.md gate 2.15, run once at boot. This image is a debug build --
/// env:bringup extends env:debug -- so the vectors are compiled in.
link::SelfTestResult g_selfTest;

constexpr uint32_t kSensorIntervalMs = 30000;
constexpr uint32_t kReportIntervalMs = 10000;
constexpr uint32_t kGnssTimeoutMs = 90000;

void sampleSensors()
{
    g_peripherals.sensor = g_sensor.read();
    g_peripherals.batteryMv = g_battery.millivolts();
    g_peripherals.batteryLow = g_peripherals.batteryMv != 0 &&
                               g_peripherals.batteryMv < hal::kLowBatteryMv;
}

void carryOut(ui::Action action)
{
    ++g_actionsHandled;
    switch (action) {
    case ui::Action::None:
        --g_actionsHandled;
        break;

    case ui::Action::ForceRefresh:
        // CLAUDE.md 3.2: "Wake / force refresh (sample sensors, open RX window
        // now)". The RX window half needs a radio and is not here.
        sampleSensors();
        break;

    case ui::Action::SampleSensors:
        sampleSensors();
        break;

    case ui::Action::RequestGnssFix:
        // CLAUDE.md 1.5: never on its own, only when asked, and with a hard
        // timeout. This is the only place the GNSS is ever powered.
        g_gnss.requestFix(kGnssTimeoutMs, millis());
        g_peripherals.gnssPowered = true;
        g_peripherals.gnssTimedOut = false;
        break;

    case ui::Action::MarkAllRead:
        g_lastReadCounter = app::newestReceivedCounter(g_node);
        break;

    case ui::Action::ToggleFrontLight:
        g_display.setFrontLight(true);
        delay(2000);
        g_display.setFrontLight(false);
        break;

    case ui::Action::SendBeacon:
        // Needs a radio. Blinking is an honest answer: something happened, and
        // it was not a transmission.
        g_led.blink(200);
        break;

    case ui::Action::PowerMenu:
        // The controller has already opened the menu; the next render draws it.
        // Acknowledged with the LED so a two-second hold is visibly not ignored
        // even before the panel catches up 300 ms later.
        g_led.blink(120);
        break;

    case ui::Action::EnterSleep:
    case ui::Action::Shutdown:
        /*
         * Deliberately not carried out.
         *
         * What "sleep" means is the CLAUDE.md 3.1 power state machine, and that
         * is phase 5. What "shut down" means on this board is not settled
         * either: PIN_PWR_ON low does not drop the rail while USB is attached
         * (decisions D2 and D13), so a shutdown here would look like a hang.
         *
         * The menu is finished and its choice arrives; two long blinks say the
         * device heard it. Doing half of it would be worse than doing none.
         */
        g_led.blink(400);
        delay(200);
        g_led.blink(400);
        report::info("12: %s chosen -- phase 5 implements it, see D2 and D13",
                     action == ui::Action::EnterSleep ? "sleep" : "shutdown");
        break;
    }
}

void serviceGnss()
{
    if (!g_peripherals.gnssPowered) {
        return;
    }

    const hal::GnssState state = g_gnss.poll(millis());
    g_peripherals.fix = g_gnss.fix();

    if (state == hal::GnssState::Fixed && g_peripherals.fix.hasPosition) {
        g_peripherals.fixTakenAtUnix = g_clock.unixSeconds();

        /*
         * CLAUDE.md 1.2: a device whose clock is not valid is fully
         * transmit-blocked "until time is re-established over BLE or GNSS".
         * This is the GNSS half, and it is the only reason a node that lost its
         * RTC can ever transmit again without a phone in range.
         */
        if (g_peripherals.fix.hasTime && !g_clock.timeValid()) {
            g_clock.setUnixSeconds(g_peripherals.fix.unixSeconds);
        }
    }

    if (state == hal::GnssState::TimedOut) {
        g_peripherals.gnssPowered = false;
        g_peripherals.gnssTimedOut = true;
    }
}

void printReport()
{
    const hal::RefreshPolicy &policy = g_ui.policy();

    report::begin(12);
    report::value("boot.store", "%d", g_storeOk ? 1 : 0);
    report::value("boot.node", "%d", g_nodeOk ? 1 : 0);
    report::value("boot.clock_valid", "%d", g_clock.timeValid() ? 1 : 0);
    report::value("boot.sensor", "%d", g_peripherals.sensor.valid ? 1 : 0);

    // --- gate 2.15 --------------------------------------------------------
    report::value("crypto.self_test_available", "%d", g_selfTest.available ? 1 : 0);
    report::value("crypto.rfc3610_run", "%u", g_selfTest.vectorsRun);
    report::value("crypto.rfc3610_passed", "%u", g_selfTest.vectorsPassed);
    report::value("crypto.frame_construction", "%d", g_selfTest.frameConstruction ? 1 : 0);
    report::value("gate_2_15.pass", "%d", g_selfTest.passed() ? 1 : 0);
    report::value("uptime_s", "%lu", static_cast<unsigned long>(millis() / 1000u));

    // --- gate 4.1 ---------------------------------------------------------
    // "Zero partial refreshes over 1 h with no state change." Reported as three
    // counters rather than asserted: skipped is how many passes decided nothing
    // had moved, and it should be almost every one of them.
    report::value("screen", "%s", ui::screenTitle(g_ui.screen()));
    report::value("refresh.full", "%lu", static_cast<unsigned long>(policy.fullCount()));
    report::value("refresh.partial", "%lu", static_cast<unsigned long>(policy.partialCount()));
    report::value("refresh.skipped", "%lu", static_cast<unsigned long>(policy.skippedCount()));
    report::value("display.worst_partial_ms", "%lu",
                  static_cast<unsigned long>(g_display.worstPartialMs()));
    report::value("display.worst_full_ms", "%lu",
                  static_cast<unsigned long>(g_display.worstFullMs()));
    report::value("ui.screen_changes", "%lu", static_cast<unsigned long>(g_screenChanges));
    report::value("ui.actions", "%lu", static_cast<unsigned long>(g_actionsHandled));

    // --- gates 1.4 and 1.5 ------------------------------------------------
    report::value("input.button_presses", "%lu",
                  static_cast<unsigned long>(g_inputs.pressCount(hal::InputId::Button)));
    report::value("input.button_bounces", "%lu",
                  static_cast<unsigned long>(g_inputs.bounceCount(hal::InputId::Button)));
    report::value("input.touch_presses", "%lu",
                  static_cast<unsigned long>(g_inputs.pressCount(hal::InputId::Touch)));
    report::value("input.touch_bounces", "%lu",
                  static_cast<unsigned long>(g_inputs.bounceCount(hal::InputId::Touch)));
    report::value("input.touch_resting", "%d", g_inputs.touchRestingLevel() ? 1 : 0);

    report::value("sensor.temp_centi_c", "%d", g_peripherals.sensor.tempCentiC);
    report::value("sensor.humidity_centi_pct", "%u", g_peripherals.sensor.humidityCentiPct);
    report::value("battery.mv", "%u", g_peripherals.batteryMv);
    report::value("gnss.powered", "%d", g_peripherals.gnssPowered ? 1 : 0);
    report::value("gnss.has_fix", "%d", g_peripherals.fix.hasPosition ? 1 : 0);

    const bool ok = g_storeOk && g_nodeOk && g_peripherals.sensor.valid && g_selfTest.passed();
    report::verdict(ok ? "inconclusive" : "fail",
                    ok ? "the stack runs on the board and drives the panel; gates 4.1, 1.4 and "
                         "1.5 are the counters above and need time and a thumb, not a verdict"
                       : "see boot.* above");
    report::info("touch = next screen, long touch = the screen's action, button = refresh");
    report::end(12);
}

} // namespace

namespace bringup {

void setup()
{
    // Long, because this image is meant to be left running under a thumb.
    // commonService() pokes the timer on every pass while USB is up, so it only
    // ever fires if USB goes away -- which is exactly when nobody can help.
    commonSetup(300000);
    Wire.begin();

    /*
     * Before any peripheral. It needs nothing but the CPU, and a device whose
     * crypto is wrong should say so before it says anything else.
     */
    g_selfTest = link::runCryptoSelfTest();

    g_led.begin();
    g_clock.begin();
    g_keys.begin();
    g_sensor.begin();
    g_battery.begin();
    g_inputs.begin();
    g_gnss.begin();  // powers the module DOWN, per CLAUDE.md 1.5
    g_display.begin();

    if (hal::openExternalFlash(g_transport, g_flash)) {
        g_storeOk = g_store.begin(g_flash);
    }
    if (g_storeOk) {
        // The geometry each owning module declares -- hal/ does not read it from
        // them, which would be a layer violation (decision D3).
        g_store.configureRegion(hal::StoreRegion::FrameCounter, 8, 64);
        g_store.configureRegion(hal::StoreRegion::BudgetRing, 10, 512);
        g_store.configureRegion(hal::StoreRegion::MessageQueue, app::kBlobBytes, 1);
        g_store.configureRegion(hal::StoreRegion::EventJournal, app::kJournalRecordBytes,
                                app::kJournalCapacity);
        g_nodeOk = g_node.begin(0x0001);
    }

    g_bootUnix = g_clock.unixSeconds();
    g_ui.begin(g_display);
    sampleSensors();
    g_ready = true;
}

void loop()
{
    if (!g_ready) {
        delay(100);
        commonService();
        return;
    }

    const uint32_t now = millis();

    const hal::InputEvent event = g_inputs.poll(now);
    if (event.event != hal::ButtonEvent::None) {
        const ui::Screen before = g_ui.screen();
        carryOut(g_ui.handle(event, g_inputs.heldMs(event.id, now)));
        if (g_ui.screen() != before) {
            ++g_screenChanges;
        }
    }

    serviceGnss();

    if (static_cast<uint32_t>(now - g_lastSensorMs) >= kSensorIntervalMs) {
        g_lastSensorMs = now;
        sampleSensors();
    }

    /*
     * Rendered every pass, pushed only when the pixels differ.
     *
     * That sounds wasteful and is not: rendering into RAM costs microseconds and
     * the comparison is what makes "did anything change?" an exact question.
     * Gate 4.1's failure mode -- a redraw triggered by a value that technically
     * changed -- cannot happen when the comparison is made after rendering
     * rather than before it.
     */
    app::buildViewModel(g_node, g_peripherals, g_clock.unixSeconds(),
                        (millis() / 1000u), g_lastReadCounter, g_model);
    g_ui.render(g_model);

    if (usbReady() && static_cast<uint32_t>(now - g_lastReportMs) >= kReportIntervalMs) {
        g_lastReportMs = now;
        printReport();
    }

    delay(20);
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 12
