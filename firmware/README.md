# firmware/ — device firmware

C++17 on the Adafruit nRF52 Arduino core, built with PlatformIO. LilyGO T-Echo:
nRF52840 + SX1262, 868 MHz, SSD1681 e-paper, L76K GNSS, BME280, PCF8563, ZD25WQ16B.

`CLAUDE.md` is the normative specification. Where this README and `CLAUDE.md`
disagree, `CLAUDE.md` wins.

## Structure

Five layers, strictly bottom to top. A lower layer never calls into a higher one —
`scripts/check_layering.py` checks this on every build against `layering.toml`, not as a
convention.

| Layer | What | Size |
|---|---|---|
| `hal/` | pins, SPI/I2C, drivers, power rails, RTC, sleep, dead-man timer | ~5300 lines, 18 `.cpp` |
| `link/` | framing, crypto, ARQ, duty cycle budget, adaptive SF | ~3300 lines, 11 `.cpp` |
| `ble/` | GATT server, bridge protocol | ~1050 lines |
| `app/` | sensor scheduling, message queue, journal, peer state, view model | ~2300 lines |
| `ui/` | screens, formatting, geometry | ~1450 lines |
| `bringup/` | 21 sketches, each of which evidences one gate | ~5800 lines |

**No dynamic allocation in `link/` and `app/`** — fixed buffers, checked by
`scripts/check_no_alloc.py` against the object files, with an empty allowlist. RadioLib and the
core allocate; that is outside, and not a fight you win.

The interfaces in `hal/` (`i_radio_link.h`, `i_block_store.h`, `i_clock.h`,
`i_display.h`, …) are the reason why `link/`, `app/`, `ui/` and `ble/` build and run on an
ordinary host. That is decision D3, and the proof of it is
`firmware/test/`.

`src/main.cpp` is the application: clock, flash, key, node, sensor, battery,
inputs, display, GNSS, UI, BLE. Sketch 12 wires up the same without BLE and with
counter output — something a field device should not do, but a bring-up image must.

## Building

```bash
.venv/bin/python -m platformio run -d firmware -e debug
.venv/bin/python -m platformio run -d firmware -e release
.venv/bin/python -m platformio run -d firmware -e debug -t upload --upload-port /dev/ttyACM0
```

Three environments: `debug` (logging on, the dead-man timer jumps into the bootloader), `release`
(logging compiled out, the dead-man timer restarts) and `bringup` (the sketches, selected
via `scripts/bringup_flag.py`).

`build_unflags = -std=gnu++11` is necessary because the core still defaults to gnu++11.

### What the build enforces

| Check | Why |
|---|---|
| `-Wall -Wextra -Werror`, `-Wshadow`, `-Wdouble-promotion` | |
| `-Wframe-larger-than=1024` | the loop task has **4096 bytes**. A single large local object took a device off the USB bus on 2026-08-31, and only a double-click on reset brought it back. When the warning was added it immediately found two more 1600-byte frames in code that had been running for a long time |
| `check_layering.py` | `CLAUDE.md` §3 |
| `check_no_alloc.py` | `CLAUDE.md` §3, the allowlist must stay empty |
| `release_guard.py` | a release build with `MAXL_DEV_KEY` set **must** fail (`CLAUDE.md` §6) |
| `test_vectors.py` | generates the vector header from `test-vectors/*.json` before compiling |

## Checking without a device

```bash
firmware/tools/hosttest.sh        # 276 cases, two simulations, in Docker
python3 firmware/scripts/check_layering.py
python3 firmware/scripts/check_no_alloc.py
python3 firmware/scripts/check_size.py    # Gate 0.6
```

Details in `firmware/test/README.md`. In short: the same `.cpp` files as the ARM build,
no copies. **Green means the logic is internally consistent — not that it runs on an
nRF52840.**

**The host build does not compile `hal/`**, only the portable files listed by name in
`test/Makefile`. Every change to `hal/` must be followed by a
`pio run -e debug`, otherwise the error only shows up when flashing.

## On the device

```bash
tools/nodes.py --list                     # which node is on which port
tools/morning.sh --node A                 # all open sketches in sequence
.venv/bin/python tools/bringup_run.py 12  # individually: build, flash, fetch report
tools/night.sh                            # the three that reset themselves
```

`bringup_run.py` holds the port open and does not survive a reset of the device. Sketches 16,
17 and 18 reset deliberately, 18 even fifty times — that is what `tools/await_report.py`
is for; it reopens the port after every disappearance and distinguishes in its exit code between
**passed (0), gate failed (1), timeout (2)**.

**Do not touch `firmware/src/` while a run is in progress.** PlatformIO compiles all of
`src/`; an error in a new file breaks the build of every following sketch, and the
run then reports hardware failures that are not.

The gates are defined in `docs/test-plan.md`; the measured values, and which sketch
produced them, are in `docs/test-results/`.

## Size

As of 2026-09-01, release build `g79a3670` (RAM counted as statically allocated symbols —
`size(1)`'s bss figure additionally includes the heap region reserved by the linker script and
therefore reads more dramatically than it is):

```
Flash  258.6 kB of 815.1 kB (31.7 %)
RAM     71.2 kB of 248.8 kB (28.6 %)
```

The entire night session (ACK path, D17, D18, three event types, stub ring) cost
about 6 kB of flash and 0.7 kB of RAM of that.

The flash limit `0xC7000` comes from the S140 v6 linker script. `.bss` is the number that
matters: `link/` and `app/` are fixed buffers by design, so consumption there grows
visibly and not in the heap.

## Two rules that cost hardware

- **Never power up without an antenna** — not even while flashing.
- **Never drive P0.13 LOW.** Enable of the 3.3 V regulator that the nRF52840 itself hangs off.
  `hal/board.h` deliberately does not define the pin, and `board.cpp` checks with a `static_assert`
  that no other pin coincides with it. Details in `docs/hardware/pinmap.md`.

## Secrets

None in the repository. The network key is provisioned at runtime over BLE and lives in
**internal** flash (`hal/key_store_internal`), not on the external chip — that one can be read with
a clip and a logic analyser. The development key comes
exclusively from `MAXL_DEV_KEY` and makes a release build fail.

## Two nodes on the bench

Sketch 19 is the same build on both boards, distinguished by two
environment variables:

```bash
KEY=$(openssl rand -hex 16)     # never into a file, never into the repository

MAXL_DEV_KEY=$KEY MAXL_NODE_ID=2 MAXL_BRINGUP=19 \
  ../.venv/bin/python -m platformio run -e bringup -t upload \
  --upload-port "$(../tools/nodes.py --port-of B)"

MAXL_DEV_KEY=$KEY MAXL_NODE_ID=1 MAXL_BRINGUP=19 \
  ../.venv/bin/python -m platformio run -e bringup -t upload \
  --upload-port "$(../tools/nodes.py --port-of A)"
```

**The receiver first.** Node 1 waits three minutes after start-up before it
transmits — precisely for this reason: there is only one usable USB port here, the two
are programmed one after the other, and without a start-up delay node 1 would lose its
messages to a board that is still sitting in the bootloader. Each of them costs three
ARQ attempts and would then remain as `Undelivered` (D18).

`key.shared = 1` in the report means that the key came from `MAXL_DEV_KEY` and
a second board can match it. `0` means that the derivation from the
DEVICEID took effect — that differs per chip, and then every
frame fails the MIC, which cannot be distinguished from a dead radio module.
