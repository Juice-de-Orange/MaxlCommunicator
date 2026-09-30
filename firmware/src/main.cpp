/*
 * MaxlCommunicator -- the application.
 *
 * Everything below has run on this board already, one layer at a time and then
 * all together in bring-up sketch 12. This is the same wiring without the
 * bring-up scaffolding: no report cycle, no serial commands, and a dead man's
 * timer that resets rather than dropping into DFU when the build says nobody is
 * watching (see hal::deadman::Recovery).
 *
 * WHAT IS DELIBERATELY NOT HERE
 *
 * The radio. hal/radio_sx1262 is written and the SX1262 answers over raw SPI
 * (bring-up 07, read only), but nothing has ever transmitted from this board and
 * there is no second node to transmit at. CLAUDE.md 5 is explicit: do not start
 * a phase before the previous one is confirmed on real hardware, and phase 2's
 * gates all need two devices. app::Node runs without a radio on purpose -- a
 * node that cannot transmit is not a node that cannot run -- so everything else
 * here is real, and attachRadio() is the one line that turns it on when there is
 * something to point it at.
 *
 * WHAT IS HERE
 *
 * The clock, the external flash and its four regions, the network key store, the
 * node with its queue, journal, peers and duty cycle budget, the sensor, the
 * battery, the two inputs, the panel and the five screens, the GNSS, and the
 * GATT server over Bluefruit. A phone can connect to this and drain the journal.
 */

#include <Arduino.h>

#include "bench_key.h"
#include "build_guard.h"

/*
 * Phase 0 bring-up images replace this entry point wholesale. One sketch per
 * peripheral, selected by MAXL_BRINGUP -- see src/bringup/bringup.h.
 */
#if defined(MAXL_BRINGUP)

#include "bringup/bringup.h"

void setup()
{
    bringup::setup();
}

void loop()
{
    bringup::loop();
}

#else

#include <Wire.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <Adafruit_SPIFlash.h>
#pragma GCC diagnostic pop

#include "app/node.h"
#include "app/view.h"
#include "ble/gatt_server.h"
#include "hal/battery_adc.h"
#include "hal/ble_transport_bluefruit.h"
#include "hal/block_store_littlefs.h"
#include "hal/external_flash.h"
#include "hal/clock_pcf8563.h"
#include "hal/deadman.h"
#include "hal/display_ssd1681.h"
#include "hal/gnss_l76k.h"
#include "hal/inputs_gpio.h"
#include "hal/key_store_internal.h"
#include "hal/led.h"
#include "hal/sensor_bme280.h"
#include "link/self_test.h"
#include "ui/ui.h"

#ifndef FW_VERSION_FULL
#define FW_VERSION_FULL "unknown"
#endif
#ifndef FW_GIT_HASH
#define FW_GIT_HASH "unknown"
#endif

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
hal::BluefruitBleTransport g_ble;

app::Node g_node{g_clock, g_store, g_keys};
ble::GattServer g_gatt{g_ble, g_node};
app::Peripherals g_peripherals;
ui::Ui g_ui;

/*
 * File-scope, not a local. The core gives loop() a 4 kB FreeRTOS stack
 * (LOOP_STACK_SZ in cores/nRF5/main.cpp), and ui::ViewModel is around 450 bytes
 * -- a needless fraction of that to spend on every pass.
 */
ui::ViewModel g_model;

link::SelfTestResult g_selfTest;

/*
 * A heartbeat, debug builds only.
 *
 * The application is silent by design -- CLAUDE.md 6 compiles serial output out
 * of release builds, and a node on a hillside has nobody to print to. But a
 * silent image is also an image nobody can tell apart from a hung one, and this
 * one is new. In a debug build it says what it is doing every ten seconds;
 * build_guard.h refuses to compile a release image with MAXL_LOG_LEVEL above 0,
 * so this cannot leak into one by accident.
 */
#if defined(MAXL_LOG_LEVEL) && MAXL_LOG_LEVEL > 0
#define MAXL_APP_HEARTBEAT 1
#endif

#if defined(MAXL_APP_HEARTBEAT)
constexpr uint32_t kHeartbeatMs = 10000;
uint32_t g_lastHeartbeatMs = 0;
#endif

bool g_ready = false;
uint32_t g_lastReadCounter = 0;
uint32_t g_lastSensorMs = 0;

/*
 * Long, because a device on a hillside is not being watched.
 *
 * hal::deadman::defaultRecovery() decides what expiry means: a debug image goes
 * to the bootloader, where a host is standing by; a release image resets,
 * because a node that answered a hang by waiting in DFU would be waiting for
 * somebody with a laptop, in a place chosen for not having one.
 */
constexpr uint32_t kDeadmanMs = 120000;

constexpr uint32_t kSensorIntervalMs = 30000;
constexpr uint32_t kGnssTimeoutMs = 120000;

/// CLAUDE.md 0: device IDs are built for N nodes from day one. Until the
/// provisioning flow assigns one, this is what the image ships with.
/*
 * The address this image answers to, from MAXL_NODE_ID (default 1).
 *
 * A shipping node is given its id over BLE; this is what it starts from, and it
 * is what lets two bench boards be two different nodes without two source trees.
 */
constexpr uint16_t kDefaultNodeId = bench::nodeId();

void sampleSensors()
{
    g_peripherals.sensor = g_sensor.read();
    g_peripherals.batteryMv = g_battery.millivolts();
    g_peripherals.batteryLow =
        g_peripherals.batteryMv != 0 && g_peripherals.batteryMv < hal::kLowBatteryMv;
}

void carryOut(ui::Action action)
{
    switch (action) {
    case ui::Action::None:
    case ui::Action::PowerMenu:
        break;

    case ui::Action::ForceRefresh:
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
        delay(3000);
        g_display.setFrontLight(false);
        break;

    case ui::Action::SendBeacon:
        // Needs a radio. Blinking is an honest answer: something happened, and
        // it was not a transmission.
        g_led.blink(200);
        break;

    case ui::Action::EnterSleep:
    case ui::Action::Shutdown:
        /*
         * Not carried out. What "sleep" means is the CLAUDE.md 3.1 power state
         * machine, and that is phase 5; what "shut down" means on this board is
         * not settled either, because PIN_PWR_ON low does not drop the rail
         * while USB is attached (decisions D2 and D13). Two long blinks say the
         * device heard it. Doing half of it would be worse than doing none.
         */
        g_led.blink(400);
        delay(200);
        g_led.blink(400);
        break;
    }
}

void serviceGnss()
{
    /*
     * One EVT_FIX per acquisition, not one per loop iteration: the driver
     * reports Fixed for as long as the module is powered, and journaling it
     * every pass would fill the 128-entry journal with one fix.
     */
    static bool fixJournaled = false;

    if (!g_peripherals.gnssPowered) {
        fixJournaled = false;
        return;
    }

    const hal::GnssState state = g_gnss.poll(millis());
    g_peripherals.fix = g_gnss.fix();

    if (state == hal::GnssState::Fixed && g_peripherals.fix.hasPosition) {
        g_peripherals.fixTakenAtUnix = g_clock.unixSeconds();

        /*
         * CLAUDE.md 1.2: a device whose clock is not valid is fully
         * transmit-blocked "until time is re-established over BLE or GNSS". This
         * is the GNSS half, and it is the only way a node that lost its RTC can
         * ever transmit again with no phone in range.
         */
        if (g_peripherals.fix.hasTime && !g_clock.timeValid()) {
            g_clock.setUnixSeconds(g_peripherals.fix.unixSeconds);
        }

        if (!fixJournaled) {
            fixJournaled = true;
            // The durable EVT_FIX (bridge-protocol section 4; D11 for hdop).
            g_node.onFix(g_peripherals.fix.latitudeE7, g_peripherals.fix.longitudeE7,
                         g_peripherals.fix.altitudeM, g_peripherals.fix.hdopTenths, 0,
                         g_peripherals.fix.satellites);
        }
    }

    if (state == hal::GnssState::TimedOut) {
        g_peripherals.gnssPowered = false;
        g_peripherals.gnssTimedOut = true;
        fixJournaled = false;
    }
}

#if defined(MAXL_APP_HEARTBEAT)
void heartbeat()
{
    Serial.printf("maxl %s %s | up %lus | crypto %u/%u %s | clock %s | tx %s | ble %s"
                  " | screen %s | refresh %lu/%lu | batt %umV | %s %d.%02dC\n",
                  FW_VERSION_FULL, FW_GIT_HASH,
                  static_cast<unsigned long>(millis() / 1000u),
                  static_cast<unsigned>(g_selfTest.vectorsPassed),
                  static_cast<unsigned>(g_selfTest.vectorsRun),
                  g_selfTest.passed() ? "ok" : "FAIL",
                  g_clock.timeValid() ? "valid" : "INVALID",
                  g_node.transmitAllowed() ? "allowed" : "blocked",
                  g_ble.connected() ? "connected" : "advertising",
                  ui::screenTitle(g_ui.screen()),
                  static_cast<unsigned long>(g_ui.policy().fullCount()),
                  static_cast<unsigned long>(g_ui.policy().partialCount()),
                  static_cast<unsigned>(g_peripherals.batteryMv),
                  g_peripherals.sensor.valid ? "sensor" : "no-sensor",
                  g_peripherals.sensor.tempCentiC / 100,
                  abs(g_peripherals.sensor.tempCentiC % 100));
}
#endif

} // namespace

void setup()
{
    /*
     * First, before any peripheral, so that a hang inside a driver still ends
     * somewhere recoverable. It runs in its own FreeRTOS task, which is what
     * makes that true of a hang inside loop() as well -- see hal/deadman.h and
     * docs/test-results/2026-08-31_deadman_survives_hung_loop.md.
     */
    hal::deadman::arm(kDeadmanMs);

    Serial.begin(115200);

    // docs/test-plan.md gate 2.15: the crypto known-answer test, on the device,
    // at boot, in debug builds. Before the radio could ever be used, and before
    // waiting on a host that may not be there.
    g_selfTest = link::runCryptoSelfTest();

    Wire.begin();

    g_led.begin();
    g_clock.begin();
    g_keys.begin();
    g_sensor.begin();
    g_battery.begin();
    g_inputs.begin();
    g_gnss.begin();  // powers the module DOWN, per CLAUDE.md 1.5
    g_display.begin();

    if (hal::openExternalFlash(g_transport, g_flash) &&
        g_store.begin(g_flash)) {
        /*
         * The geometry each owning module declares. hal/ does not read it from
         * them -- that would be a layer violation, and check_layering.py says so
         * (decision D3).
         */
        g_store.configureRegion(hal::StoreRegion::FrameCounter, 8, 64);
        g_store.configureRegion(hal::StoreRegion::BudgetRing, 10, 512);
        g_store.configureRegion(hal::StoreRegion::MessageQueue, app::kBlobBytes, 1);
        g_store.configureRegion(hal::StoreRegion::EventJournal, app::kJournalRecordBytes,
                                app::kJournalCapacity);

        /*
         * A false here is not a warning. CLAUDE.md 2.1: a device that cannot
         * persist its counter must refuse to transmit, and 1.2 says the same of
         * the budget. The node enforces that itself; the rest of the device --
         * screens, sensors, BLE -- keeps working, which is the point of a
         * communicator that is still useful with no radio.
         */
        g_node.begin(kDefaultNodeId);
    }

    /*
     * The GATT server, so a phone can drain the journal and provision a key.
     *
     * The name carries the last four hex digits of the nRF52840's factory
     * DEVICEID, because it is what appears in the phone's chooser and two
     * devices in the same rucksack both called "MaxlCommunicator" is not a
     * chooser anyone can use. The same register is what firmware/nodes.ini
     * distinguishes nodes by, so the name in the chooser and the name in the
     * notes are the same string.
     */
    char bleName[24];
    snprintf(bleName, sizeof(bleName), "Maxl-%04lX",
             static_cast<unsigned long>(NRF_FICR->DEVICEID[0] & 0xFFFFu));
    g_ble.begin(bleName, g_gatt);
    g_ble.startAdvertising();

    g_ui.begin(g_display);
    sampleSensors();
    g_ready = true;
}

void loop()
{
    if (!g_ready) {
        delay(100);
        hal::deadman::poke();
        return;
    }

    const uint32_t now = millis();

    const hal::InputEvent event = g_inputs.poll(now);
    if (event.event != hal::ButtonEvent::None) {
        carryOut(g_ui.handle(event, g_inputs.heldMs(event.id, now)));
    }

    serviceGnss();
    g_node.tick(now);
    g_gatt.tick(now);

    if (static_cast<uint32_t>(now - g_lastSensorMs) >= kSensorIntervalMs) {
        g_lastSensorMs = now;
        sampleSensors();
    }

    /*
     * Rendered every pass, pushed only when the pixels differ. That sounds
     * wasteful and is not: rendering into RAM costs microseconds, and the
     * comparison is what makes "did anything change?" an exact question rather
     * than a judgement each screen has to make for itself. Measured on node A:
     * 65 minutes idle, 148 657 passes, one full refresh and no partials.
     */
    app::buildViewModel(g_node, g_peripherals, g_clock.unixSeconds(), millis() / 1000u,
                        g_lastReadCounter, g_model);
    g_ui.render(g_model);

#if defined(MAXL_APP_HEARTBEAT)
    if (static_cast<uint32_t>(now - g_lastHeartbeatMs) >= kHeartbeatMs) {
        g_lastHeartbeatMs = now;
        heartbeat();
    }
#endif

    hal::deadman::poke();
    delay(20);
}

#endif // MAXL_BRINGUP
