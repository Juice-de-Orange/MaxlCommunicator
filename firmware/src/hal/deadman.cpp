#include "deadman.h"

#include <Arduino.h>

namespace hal {
namespace deadman {
namespace {

/*
 * volatile and 32-bit, written by the loop task and read by the watchdog task.
 *
 * No mutex: an aligned 32-bit load or store is a single instruction on
 * Cortex-M and cannot be seen half-written. The worst interleaving is that the
 * watchdog reads a poke one cycle late, which costs nothing against a timeout
 * measured in tens of seconds.
 */
volatile bool g_armed = false;
volatile uint32_t g_timeoutMs = 0;
volatile uint32_t g_lastPokeMs = 0;

TaskHandle_t g_task = nullptr;
Recovery g_recovery = Recovery::Bootloader;

/// A kilobyte. The task compares two integers and, once, does not return.
constexpr uint16_t kTaskStackWords = 256;

/// Four times a second. The timeout is tens of seconds, so this is far finer
/// than it needs to be and still costs nothing -- the task is blocked between
/// checks.
constexpr uint32_t kCheckIntervalMs = 250;

/// Neither of these returns.
void fire()
{
    if (g_recovery == Recovery::Bootloader) {
        // enterUf2Dfu() disables the SoftDevice, masks every interrupt, writes
        // GPREGRET = 0x57 and resets. The bootloader reads that register before
        // any application code runs.
        enterUf2Dfu();
    }
    NVIC_SystemReset();
}

bool expired()
{
    if (!g_armed) {
        return false;
    }
    // Unsigned subtraction, so the 49-day millis() wrap costs nothing.
    return (millis() - g_lastPokeMs) >= g_timeoutMs;
}

void watchdogTask(void *)
{
    for (;;) {
        if (expired()) {
            fire();
        }
        vTaskDelay(pdMS_TO_TICKS(kCheckIntervalMs));
    }
}

} // namespace

Recovery defaultRecovery()
{
#if defined(MAXL_BUILD) && MAXL_BUILD == 2
    return Recovery::Reset;
#else
    return Recovery::Bootloader;
#endif
}

void arm(uint32_t timeoutMs, Recovery recovery)
{
    g_recovery = recovery;
    g_timeoutMs = timeoutMs;
    g_lastPokeMs = millis();
    g_armed = true;

    if (g_task != nullptr) {
        return;
    }

    /*
     * TASK_PRIO_NORMAL: above the loop task (TASK_PRIO_LOW) so a spinning loop
     * cannot starve it, and below the Bluefruit task so it never delays the
     * radio stack. It runs for microseconds four times a second.
     */
    if (xTaskCreate(watchdogTask, "deadman", kTaskStackWords, nullptr, TASK_PRIO_NORMAL,
                    &g_task) != pdPASS) {
        g_task = nullptr;
    }
}

void poke()
{
    g_lastPokeMs = millis();
}

void disarm()
{
    g_armed = false;
}

bool taskRunning()
{
    return g_task != nullptr;
}

uint32_t remainingMs()
{
    if (!g_armed) {
        return 0;
    }
    const uint32_t elapsed = millis() - g_lastPokeMs;
    return elapsed >= g_timeoutMs ? 0 : g_timeoutMs - elapsed;
}

void service()
{
    // The task does this. This path exists only for the case where it could not
    // be created, and then it is the sole check there is.
    if (g_task != nullptr) {
        return;
    }
    if (expired()) {
        fire();
    }
}

} // namespace deadman
} // namespace hal
