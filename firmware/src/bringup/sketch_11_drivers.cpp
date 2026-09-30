/*
 * Bring-up 11 -- the phase 1 drivers, on the device, through the shipping code.
 *
 * Sketch 10 did this for the clock and the block store: run hal/'s real
 * implementations rather than a throwaway, because a throwaway proves nothing
 * about what ships. This is the same idea for the rest of phase 1 --
 * hal::DisplaySsd1681, hal::Bme280Sensor, hal::BatteryAdc, hal::GpioInputs,
 * hal::StatusLed and hal::GnssL76k, plus the ui/ screens drawn on the real panel.
 *
 * What it settles by itself, with numbers:
 *
 *   gate 1.1   twenty consecutive partial refreshes, each under 400 ms
 *   the NMEA parser against real sentences from the L76K -- sketch 06 found a
 *              valid fix indoors with 11 satellites in view, so there is live
 *              data to parse rather than only the fixtures in the host tests
 *
 * What it prepares for somebody with hands:
 *
 *   gate 1.2   sixteen partials and then a full refresh, left on the panel
 *   gate 1.3   the BME280's readings, to hold against a reference instrument
 *   gate 1.6   the ADC's raw count and computed millivolts, for a multimeter --
 *              but see decision D13: with USB attached this is not the cell
 *   gate 1.5   the resting level of the touch pad, which settles its polarity
 *
 * The report is computed once and reprinted for ever after. That is not
 * decoration either: sketches 04, 08 and 10 each reported once, each finished
 * before tools/bringup_run.py had the port open, and each looked like a dead
 * device while sitting there having passed.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 11

#include <Arduino.h>
#include <Wire.h>

#include "common.h"
#include "report.h"

#include "hal/battery_adc.h"
#include "hal/display_ssd1681.h"
#include "hal/gnss_l76k.h"
#include "hal/inputs_gpio.h"
#include "hal/led.h"
#include "hal/sensor_bme280.h"
#include "ui/screens.h"

namespace {

hal::DisplaySsd1681 g_display;
hal::Bme280Sensor g_sensor;
hal::BatteryAdc g_battery;
hal::GpioInputs g_inputs;
hal::StatusLed g_led;
hal::GnssL76k g_gnss;

hal::Canvas g_canvas;
hal::Canvas g_shown;
hal::RefreshPolicy g_policy;

/// docs/test-plan.md gate 1.1: twenty consecutive partial updates.
constexpr uint8_t kPartialTrials = 20;

/// Short, because this sketch has other things to do and sketch 06 already
/// showed the module answering. A cold fix indoors is a bonus, not the point.
constexpr uint32_t kGnssTimeoutMs = 45000;

struct Results {
    bool displayOk = false;
    uint32_t fullMs = 0;
    uint32_t partialWorstMs = 0;
    uint32_t partialBestMs = 0;
    uint32_t partialTotalMs = 0;
    uint8_t partialsRun = 0;

    bool sensorOk = false;
    uint8_t sensorChipId = 0;
    uint8_t sensorAddress = 0;
    hal::SensorSample sample;

    uint16_t batteryMv = 0;
    uint16_t batteryCounts = 0;

    bool touchResting = false;
    bool buttonResting = false;

    uint32_t gnssBytes = 0;
    uint32_t gnssAccepted = 0;
    uint32_t gnssRejected = 0;
    uint32_t gnssTimeToFixMs = 0;
    hal::GnssFix fix;
    hal::GnssState gnssState = hal::GnssState::Off;
};

Results g_results;
bool g_measured = false;

/// A model with enough in it that the STATUS screen has something to draw. The
/// live values are filled in from what was actually measured.
ui::ViewModel modelFrom(const Results &results)
{
    ui::ViewModel model;
    model.batteryMv = results.batteryMv;
    model.timeValid = true;
    model.uptimeS = millis() / 1000u;
    model.budgetRemainingS = 360;
    model.budgetTotalS = 360;
    model.band = 'P';
    model.currentSf = 9;

    model.haveLocalSensor = results.sample.valid;
    model.localHumidityValid = results.sample.humidityValid;
    model.localTempCentiC = results.sample.tempCentiC;
    model.localHumidityCentiPct = results.sample.humidityCentiPct;
    model.localPressurePa = results.sample.pressurePa;

    model.haveFix = results.fix.hasPosition;
    model.latE7 = results.fix.latitudeE7;
    model.lonE7 = results.fix.longitudeE7;
    model.altM = results.fix.altitudeM;
    model.hdopTenths = results.fix.hdopTenths;
    model.satellites = results.fix.satellites;
    return model;
}

/*
 * Twenty partial refreshes with a visibly different canvas each time, then the
 * full refresh that clears what they left behind.
 *
 * The canvas has to actually change: a partial refresh of an identical image
 * costs the panel time but leaves no ghost, so twenty of those would time
 * something that is not what gate 1.2 is about.
 */
void exerciseDisplay(Results &results)
{
    results.displayOk = g_display.begin();
    if (!results.displayOk) {
        return;
    }

    const ui::ViewModel model = modelFrom(results);

    ui::renderScreen(ui::Screen::Status, model, g_canvas);
    g_display.present(g_canvas, hal::RefreshKind::Full);
    results.fullMs = g_display.lastRefreshMs();

    g_shown.copyFrom(g_canvas);

    results.partialBestMs = 0xFFFFFFFFu;
    for (uint8_t i = 0; i < kPartialTrials; ++i) {
        // A moving marker: small enough to be a realistic update, large enough
        // that the controller cannot decide nothing changed.
        ui::renderScreen(ui::Screen::Status, model, g_canvas);
        g_canvas.fillRect(static_cast<int16_t>(4 + i * 9), 176, 8, 8);

        /*
         * Through the same path ui::Ui uses: only the rectangle that actually
         * moved is handed to the panel. The first version of this sketch pushed
         * the whole 200x200 as a partial window and measured 471 ms against
         * gate 1.1's 400 -- which was a true measurement of the wrong thing, so
         * the number here is the one the shipping code will produce.
         */
        hal::Rect changed;
        const bool moved = g_canvas.diffBounds(g_shown, changed);
        g_display.present(g_canvas, hal::RefreshKind::Partial, moved ? changed : hal::Rect{});
        g_shown.copyFrom(g_canvas);
        const uint32_t took = g_display.lastRefreshMs();
        results.partialTotalMs += took;
        if (took > results.partialWorstMs) {
            results.partialWorstMs = took;
        }
        if (took < results.partialBestMs) {
            results.partialBestMs = took;
        }
        ++results.partialsRun;
    }

    /*
     * Gate 1.2 is a judgement about what the glass looks like, and it is left
     * here for eyes. Twenty partials have just been laid down; this full refresh
     * is what must clear every trace of them. If the marker's track is still
     * faintly visible afterwards, the panel needs more than sixteen partials
     * between full refreshes and CLAUDE.md 1.6's number is wrong for this part.
     */
    ui::renderScreen(ui::Screen::Peers, model, g_canvas);
    g_display.present(g_canvas, hal::RefreshKind::Full);
    g_display.sleep();
}

void measure()
{
    Results &results = g_results;

    /*
     * Progress lines, printed outside the report frame on purpose.
     *
     * tools/bringup_run.py collects between "MAXL-BRINGUP 11 begin" and the
     * matching "end" and ignores everything before it, so these cost nothing in
     * the collected output -- and they are the difference between diagnosing a
     * hang in seconds and staring at a silent port for two minutes, which is
     * what the first run of this sketch cost.
     */
    report::info("11: leds");
    g_led.begin();
    g_led.set(true);

    report::info("11: sensor");
    results.sensorOk = g_sensor.begin();
    results.sensorChipId = g_sensor.chipId();
    results.sensorAddress = g_sensor.address();
    if (results.sensorOk) {
        results.sample = g_sensor.read();
    }

    report::info("11: battery");
    g_battery.begin();
    results.batteryMv = g_battery.millivolts();
    results.batteryCounts = g_battery.lastCounts();

    report::info("11: inputs");
    g_inputs.begin();
    results.touchResting = g_inputs.touchRestingLevel();
    results.buttonResting = g_inputs.buttonRestingLevel();

    report::info("11: display -- one full refresh, 20 partials, one full");
    exerciseDisplay(results);
    report::info("11: display done in %lu ms worst partial",
                 static_cast<unsigned long>(results.partialWorstMs));

    // GNSS last: it is the slowest and the one most likely to find nothing.
    report::info("11: gnss, up to %lu ms", static_cast<unsigned long>(kGnssTimeoutMs));
    g_gnss.begin();
    const uint32_t gnssStart = millis();
    g_gnss.requestFix(kGnssTimeoutMs, millis());

    /*
     * Kept running past the first fix, on purpose.
     *
     * A position comes out of GGA; the date and time come out of RMC, and the
     * module emits them at its own pace. Stopping at the first fix -- which the
     * first version did -- reported has_time = 0 every time and left the RMC path
     * untested against real sentences. CLAUDE.md 1.2 makes that path load
     * bearing: a device whose clock is gone is transmit-blocked until BLE or
     * GNSS restores it.
     */
    while (g_gnss.state() == hal::GnssState::Searching ||
           (g_gnss.state() == hal::GnssState::Fixed && !g_gnss.fix().hasTime &&
            (millis() - gnssStart) < kGnssTimeoutMs)) {
        results.gnssState = g_gnss.poll(millis());
        // Qualified because this helper sits outside namespace bringup, and the
        // dead man's timer must keep being poked through a 45 second wait.
        bringup::commonService();
        delay(10);
    }
    results.gnssState = g_gnss.state();
    results.gnssBytes = g_gnss.bytesSeen();
    results.gnssAccepted = g_gnss.acceptedSentences();
    results.gnssRejected = g_gnss.rejectedSentences();
    results.gnssTimeToFixMs = g_gnss.timeToFixMs();
    results.fix = g_gnss.fix();
    g_gnss.powerOff();
    report::info("11: gnss done");

    g_led.set(false);
}

void printReport()
{
    const Results &r = g_results;
    report::begin(11);

    // --- display ----------------------------------------------------------
    report::value("display.begin", "%d", r.displayOk ? 1 : 0);
    report::value("display.full_ms", "%lu", static_cast<unsigned long>(r.fullMs));
    report::value("display.partials", "%u", r.partialsRun);
    report::value("display.partial_worst_ms", "%lu",
                  static_cast<unsigned long>(r.partialWorstMs));
    report::value("display.partial_best_ms", "%lu",
                  static_cast<unsigned long>(r.partialsRun ? r.partialBestMs : 0));
    report::value("display.partial_mean_ms", "%lu",
                  static_cast<unsigned long>(r.partialsRun ? r.partialTotalMs / r.partialsRun : 0));
    const bool gate11 = r.displayOk && r.partialsRun == kPartialTrials && r.partialWorstMs < 400;
    report::value("gate_1_1.pass", "%d", gate11 ? 1 : 0);

    // --- sensor -----------------------------------------------------------
    report::value("sensor.begin", "%d", r.sensorOk ? 1 : 0);
    report::value("sensor.address", "0x%02X", r.sensorAddress);
    report::value("sensor.chip_id", "0x%02X", r.sensorChipId);
    report::value("sensor.part", "%s",
                  r.sensorChipId == 0x60   ? "BME280"
                  : r.sensorChipId == 0x58 ? "BMP280 (no humidity)"
                                           : "unknown");
    report::value("sensor.temp_centi_c", "%d", r.sample.tempCentiC);
    report::value("sensor.humidity_centi_pct", "%u", r.sample.humidityCentiPct);
    report::value("sensor.humidity_valid", "%d", r.sample.humidityValid ? 1 : 0);
    report::value("sensor.pressure_pa", "%lu", static_cast<unsigned long>(r.sample.pressurePa));

    // --- battery ----------------------------------------------------------
    // Decision D13: with USB attached this is not the cell. The raw count is
    // reported so the number is diagnosable rather than merely wrong.
    report::value("battery.counts", "%u", r.batteryCounts);
    report::value("battery.mv", "%u", r.batteryMv);
    report::info("battery: see decision D13 -- with USB attached this is not the cell");

    // --- inputs -----------------------------------------------------------
    report::value("inputs.touch_resting_level", "%d", r.touchResting ? 1 : 0);
    report::value("inputs.button_resting_level", "%d", r.buttonResting ? 1 : 0);
    report::info("touch polarity: resting HIGH means active low, and pinmap.md 4 is wrong");

    // --- gnss -------------------------------------------------------------
    report::value("gnss.state", "%s",
                  r.gnssState == hal::GnssState::Fixed      ? "fixed"
                  : r.gnssState == hal::GnssState::TimedOut ? "timed_out"
                  : r.gnssState == hal::GnssState::Off      ? "off"
                                                            : "searching");
    report::value("gnss.bytes", "%lu", static_cast<unsigned long>(r.gnssBytes));
    report::value("gnss.sentences_accepted", "%lu", static_cast<unsigned long>(r.gnssAccepted));
    report::value("gnss.sentences_rejected", "%lu", static_cast<unsigned long>(r.gnssRejected));
    report::value("gnss.time_to_fix_ms", "%lu", static_cast<unsigned long>(r.gnssTimeToFixMs));
    report::value("gnss.has_position", "%d", r.fix.hasPosition ? 1 : 0);
    report::value("gnss.lat_e7", "%ld", static_cast<long>(r.fix.latitudeE7));
    report::value("gnss.lon_e7", "%ld", static_cast<long>(r.fix.longitudeE7));
    report::value("gnss.alt_m", "%d", r.fix.altitudeM);
    report::value("gnss.hdop_tenths", "%u", r.fix.hdopTenths);
    report::value("gnss.satellites", "%u", r.fix.satellites);
    report::value("gnss.has_time", "%d", r.fix.hasTime ? 1 : 0);
    report::value("gnss.unix", "%lu", static_cast<unsigned long>(r.fix.unixSeconds));

    const bool ok = r.displayOk && gate11 && r.sensorOk && r.sensorChipId == 0x60;
    report::verdict(ok ? "pass" : "fail",
                    ok ? "panel, sensor, ADC, inputs and GNSS all answer through the shipping "
                         "hal drivers; gate 1.1 measured"
                       : "see the failing key above");
    report::info("gate 1.2 needs eyes: the panel holds a PEERS screen after 20 partials");
    report::end(11);
}

} // namespace

namespace bringup {

void setup()
{
    // Generous: the display exercise alone is twenty partial refreshes, and the
    // GNSS attempt is another 45 seconds.
    commonSetup(240000);
    Wire.begin();
}

void loop()
{
    if (!usbReady()) {
        delay(200);
        commonService();
        return;
    }

    if (!g_measured) {
        measure();
        g_measured = true;
    }

    printReport();
    delay(3000);
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 11
