/*
 * Bring-up 14 -- prove the dead man's timer survives a hung loop().
 *
 * This sketch exists to fail on purpose, and it is the only way to know that the
 * change made on 2026-08-31 actually does what it claims.
 *
 * The failure it reproduces was real. hal::GnssL76k::begin() called
 * Serial1.end() on a UART that had never been begun; the core's end() does
 *
 *     while (!(nrfUart->EVENTS_TXSTOPPED && nrfUart->EVENTS_RXTO)) yield();
 *
 * and a disabled UARTE raises neither event. The yield() kept FreeRTOS
 * scheduling, so USB stayed up and the host saw a perfectly healthy device --
 * while the loop task was gone for good. The timer was armed the whole time and
 * never got a chance to look at itself, because service() ran from loop().
 *
 * Now it runs in its own task at a higher priority. So this sketch arms a short
 * timer, reports for a few seconds so the host can see it is alive, and then
 * enters the same shape of hang. If the change works, the device disappears from
 * USB and comes back as TECHOBOOT without anybody touching it.
 *
 *   pass  -> the device reappears in the bootloader
 *   fail  -> it sits there enumerated and silent, exactly as it did that morning
 *
 * The timeout is deliberately short. Every other sketch uses minutes; this one
 * is meant to be watched.
 */

#include "bringup.h"

#if defined(MAXL_BRINGUP) && MAXL_BRINGUP == 14

#include <Arduino.h>

#include "common.h"
#include "hal/deadman.h"
#include "report.h"

namespace {

/// Short enough to watch, long enough for the host to open the port and read
/// the report first.
constexpr uint32_t kDeadmanMs = 20000;

/// How long to behave before hanging. The host needs to have collected at least
/// one complete report, or the run proves nothing about the timer -- only that
/// the device stopped talking.
constexpr uint32_t kBehaveMs = 12000;

bool g_hung = false;

void printReport()
{
    report::begin(14);
    report::value("deadman.task_running", "%d", hal::deadman::taskRunning() ? 1 : 0);
    report::value("deadman.timeout_ms", "%lu", static_cast<unsigned long>(kDeadmanMs));
    report::value("deadman.remaining_ms", "%lu",
                  static_cast<unsigned long>(hal::deadman::remainingMs()));
    report::value("uptime_ms", "%lu", static_cast<unsigned long>(millis()));
    report::value("hang.at_ms", "%lu", static_cast<unsigned long>(kBehaveMs));
    report::value("hang.entered", "%d", g_hung ? 1 : 0);

    if (hal::deadman::taskRunning()) {
        report::verdict("inconclusive",
                        "the watchdog task exists; whether it fires is decided by this "
                        "device reappearing as TECHOBOOT, which only the host can see");
    } else {
        report::verdict("fail",
                        "the watchdog task could not be created -- the timer has degraded "
                        "to the loop-driven behaviour and a hung loop is not covered");
    }
    report::info("14: hanging at %lu ms, expect TECHOBOOT about %lu ms later",
                 static_cast<unsigned long>(kBehaveMs),
                 static_cast<unsigned long>(kDeadmanMs));
    report::end(14);
}

} // namespace

namespace bringup {

void setup()
{
    commonSetup(kDeadmanMs);
}

void loop()
{
    if (!usbReady()) {
        delay(100);
        commonService();
        return;
    }

    printReport();

    if (millis() >= kBehaveMs) {
        report::info("14: entering the hang now -- nothing more will be printed");
        Serial.flush();
        delay(50);

        g_hung = true;

        /*
         * The same shape as the real one: a loop that never ends and yields on
         * every pass, so the scheduler keeps running and everything except this
         * task stays healthy. commonService() is NOT called from in here, which
         * is the point -- nothing pokes the timer any more.
         */
        for (;;) {
            yield();
        }
    }

    delay(1000);
    commonService();
}

} // namespace bringup

#endif // MAXL_BRINGUP == 14
