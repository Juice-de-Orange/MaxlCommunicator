"""Select the phase 0 bring-up sketch from the environment.

    MAXL_BRINGUP=2 pio run -e bringup -t upload

PlatformIO's ${sysenv.VAR} has no default, and an unset variable would expand to
a bare `-DMAXL_BRINGUP=` that fails deep inside the preprocessor with a message
about the wrong thing. Doing it here gives a default and, more importantly, one
clear line in the build output saying which sketch is in the image -- which
matters when the images differ only in what they touch and the device gives no
other clue about what is running on it.
"""

import os

Import("env")  # noqa: F821  -- provided by SCons

SKETCHES = {
    0: "dfu_escape -- prove the way back to the bootloader, no Serial, no bus",
    1: "usb_cdc -- does the device enumerate, and does Serial carry anything",
    2: "i2c -- scan the bus, identify the BME280 and the PCF8563",
    3: "rtc -- set, read back, survive a soft reset, and D2 (rail cut)",
    4: "flash -- JEDEC id, 100 write/reboot cycles, deep power-down (gate 0.3)",
    5: "battery -- AIN2 through the 2:1 divider, plus the USB supply state",
    6: "gnss -- power the L76K, read NMEA, confirm the rail actually cuts",
    7: "radio -- SX1262 identity over raw SPI, read only, never transmits",
    8: "epaper -- SSD1681 through GxEPD2, refresh timings, test pattern left on screen",
    9: "inputs -- user button and touch pad, logged for a human to press",
    10: "hal -- the shipping LittleFS store and PCF8563 clock (gate 0.3, second half)",
    11: "drivers -- the phase 1 hal drivers and the ui screens, on the real panel (gate 1.1)",
    12: "app -- the whole stack minus the radio, interactive, left running (gates 4.1, 1.4, 1.5)",
    13: "unplugged -- battery and RTC with the cable OUT (decisions D2 and D13, gate 1.6)",
    14: "deadman -- hangs loop() on purpose; the timer must return the device to TECHOBOOT",
    15: "power -- holds each CLAUDE.md 3.1 state for 30 s so a meter can read it (gate 0.4, 5.1-5.4)",
    16: "storage -- queue and journal on real flash across a real reset (gates 3.1, 3.2, 3.3)",
    17: "budget -- an hour of real transmission, then a reset (gates 2.7 and 2.8a). TRANSMITS.",
    18: "counter -- 50 forced power cycles, some mid-transmit (gate 2.6). TRANSMITS.",
    19: "link -- TWO NODES, one key, one link. Gates 2.1 and 2.4. TRANSMITS.\n          Needs MAXL_DEV_KEY on both boards and MAXL_NODE_ID=1 and 2",
    20: "label -- writes the node letter on the panel and hibernates. No gate; e-paper\n          holds the image with no power, so it is a sticker you cannot lose",
}

raw = os.environ.get("MAXL_BRINGUP", "0").strip()
try:
    selected = int(raw, 0)
except ValueError:
    raise SystemExit(
        "MAXL_BRINGUP must be a number, got {!r}. Known sketches: {}".format(
            raw, ", ".join(str(k) for k in sorted(SKETCHES))
        )
    )

if selected not in SKETCHES:
    raise SystemExit(
        "No bring-up sketch {}. Known: {}".format(
            selected, ", ".join("{} ({})".format(k, v.split(" --")[0]) for k, v in sorted(SKETCHES.items()))
        )
    )

print("bringup: sketch {} -- {}".format(selected, SKETCHES[selected]))
env.Append(CPPDEFINES=[("MAXL_BRINGUP", selected)])

# Optional knobs for the link sketches, so gate 2.1's full runs (100 frames,
# SF7/SF9/SF12, no start delay on a hub) need no source edit. Absent means the
# sketch's own defaults.
for var, define in (
    ("MAXL_LINK_MESSAGES", "MAXL_LINK_MESSAGES"),
    ("MAXL_LINK_SF", "MAXL_LINK_SF"),
    ("MAXL_LINK_START_DELAY_MS", "MAXL_LINK_START_DELAY_MS"),
    # Not a link knob: makes commonSetup() wait for the host and print how far
    # setup() got, for a sketch that hangs before loop() and therefore says
    # nothing at all. See bringup/common.h.
    ("MAXL_SETUP_TRACE", "MAXL_SETUP_TRACE"),
):
    raw_value = os.environ.get(var, "").strip()
    if not raw_value:
        continue
    try:
        value = int(raw_value, 0)
    except ValueError:
        raise SystemExit("{} must be a number, got {!r}".format(var, raw_value))
    print("bringup: {} = {}".format(define, value))
    env.Append(CPPDEFINES=[(define, value)])
