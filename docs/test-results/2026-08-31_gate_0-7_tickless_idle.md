# Phase 0 — Gate 0.7 Tickless idle in the core's FreeRTOS

| | |
|---|---|
| Date | 2026-08-31 |
| Node | A (USB storage serial `0123ABCD4567EF01`) |
| Firmware | 0.1.0, `8eb31e3` |
| Environment | `bringup` |
| Measuring equipment | Source code of the core, `framework-arduinoadafruitnrf52` 1.10700.0 (= BSP 1.7.0) |
| Conditions | No hardware access needed — the question is what the core compiles |

## Result: PASS

Gate 0.7 requires: *"Answered yes or no, with the fallback identified if no."*

**Answer: yes.** Tickless idle is enabled in the core, and the nRF52 port implements it.

| Check | Expected | Measured | Result |
|---|---|---|---|
| `configUSE_TICKLESS_IDLE` | 1 | **1** (`cores/nRF5/freertos/config/FreeRTOSConfig.h:52`) | PASS |
| Port implementation present | `vPortSuppressTicksAndSleep` | present (`portable/CMSIS/nrf52/port_cmsis_systick.c:129`) | PASS |
| Wiring in the port macro | `portSUPPRESS_TICKS_AND_SLEEP` | defined (`portmacro_cmsis.h:133`) | PASS |
| Threshold | — | `configEXPECTED_IDLE_TIME_BEFORE_SLEEP = 2` ticks | — |

## Raw data

```
cores/nRF5/freertos/config/FreeRTOSConfig.h:52:
    #define configUSE_TICKLESS_IDLE                  1
cores/nRF5/freertos/config/FreeRTOSConfig.h:53:
    #define configUSE_TICKLESS_IDLE_SIMPLE_DEBUG     1
cores/nRF5/freertos/config/FreeRTOSConfig.h:97:
    #define configEXPECTED_IDLE_TIME_BEFORE_SLEEP    2
cores/nRF5/freertos/portable/CMSIS/nrf52/port_cmsis_systick.c:129:
    void vPortSuppressTicksAndSleep( TickType_t xExpectedIdleTime )
```

## Observations

The port does not count via the SysTick but via the nRF52's **RTC1**
(`portNRF_RTC_MAXTICKS`). That is the reason tickless achieves anything here at all:
a SysTick stands still in System ON sleep, the RTC keeps running. `CLAUDE.md` §3.1 requires
"nRF52 System-ON sleep with RTC wakeup, not delay loops" — the core brings exactly that.

`configUSE_TICKLESS_IDLE_SIMPLE_DEBUG` is 1 as well. That is a diagnostic mode in the
port; whether it costs current is to be checked in Phase 5 and is **not** decided here.

## Deviations from the specification

None. The gate asks for an answer, not a measurement. The answer is backed by file and
line number.

What this gate explicitly does **not** say: that the application actually sleeps. A
`delay()` in `loop()` blocks the task and the idle task runs anyway — but a
`while (true) {}` without `yield()` prevents tickless completely. That is a question for
Phase 5 and the current meter, not for this file.
