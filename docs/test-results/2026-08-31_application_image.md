# The application — `src/main.cpp` runs on the device

| | |
|---|---|
| Date | 2026-08-31, 12:05 |
| Node | A (USB storage serial `0123ABCD4567EF01`) |
| Firmware | 0.1.0, `5a7ab49`, `env:debug` — **not a bring-up image** |
| Environment | `debug` |
| Measuring equipment | Serial heartbeat; `bluetoothctl scan le` from the same computer |
| Conditions | USB power, antenna attached. **Nothing was transmitted** — no radio wired up. |

Not a gate — but the point at which layers become a device. Until today,
`src/main.cpp` was the toolchain proof from Phase 0, which printed a banner.

## Result: runs

```
maxl 0.1.0+g5a7ab49 5a7ab49 | up 30s | crypto 24/24 ok | clock valid | tx blocked
   | ble advertising | screen STATUS | refresh 1/0 | batt 4909mV | sensor 28.01C
```

| Check | Result |
|---|---|
| Crypto self-test at boot | **24/24**, gate 2.15 in the application too |
| Clock | valid, from the PCF8563 |
| External flash, LittleFS, four regions | mounted, node started up |
| Sending | **blocked** — correct, no network key is provisioned |
| BLE | **advertising** |
| Panel | `STATUS`, one full refresh, **zero partials** |
| Sensor | 28.01 °C |
| Battery | 4909 mV — see D13, on the cable, not the cell |

**Visible over BLE**, scanned from the same computer:

```
[NEW] Device AA:BB:CC:DD:EE:FF Maxl-ABCD
```

The name carries the last four hex digits of the nRF52840's DEVICEID — the same identifier
`firmware/nodes.ini` uses to tell the nodes apart (`0123ABCD4567EF01`). The name in the
phone's picker dialog and the name in the notes are therefore the same string.
Two devices in the same backpack, both called "MaxlCommunicator", would not make a dialog
anyone could find their way around.

## What this unlocks

**Gates 6.1 and 6.2 are reachable from now on.** They require a device with BLE firmware,
and there is one. Web Bluetooth requires a secure context — `http://127.0.0.1:<port>`
through an SSH tunnel to the server is one, because browsers treat localhost as secure. So
this works **without** a public DNS entry.

## What is deliberately not in it

**The radio.** `hal/radio_sx1262` is written and the SX1262 answers over raw SPI
(bring-up 07, read-only), but nothing has ever been transmitted from this board and there is
no second one to transmit to. `CLAUDE.md` §5 is unambiguous: no phase before the previous one
is confirmed, and the Phase 2 gates all need two devices.

`app::Node` deliberately runs without a radio — *"a node that cannot transmit is not a node that
cannot run"*. Everything else here is real, and `attachRadio()` is the one line that switches
it on as soon as there is a target.

## Two things that came up along the way

**1. `ble::GattServer` implemented `hal::IBleChunkSink` only by accident.** The transport's
header literally said *"ble::GattServer implements this shape"* — and that was true without
anyone checking it, because until this `main.cpp` nobody had ever handed one to the other.
It did not compile, and that is the good outcome: an interface that two classes agree on by
accident breaks silently at the next signature change.

`IBleChunkSink` now lives in `hal/i_ble_transport.h`, where interfaces belong, and
`GattServer` really inherits from it. It could not stay in the driver header: `ble/` has to
inherit from it, and a class in `ble/` must not include a driver — that would drag
Bluefruit52Lib and the SoftDevice into the host build, where gates 6.3 to 6.6 are checked.

**2. The dead-man timer's way back is not the same in both worlds.** For bring-up, DFU is
right: a host is standing next to it. For a device in the field it would be exactly wrong — it
would go into the bootloader and wait for someone with a computer, at a place that was chosen
precisely because nobody with a computer is there. `hal::deadman::Recovery` therefore follows
the build: debug → bootloader, release → reset.

## Size

`check_size.py` measures something meaningful from now on — before, `--gc-sections` threw
away half the application because `main.cpp` did not touch it.

| | Flash | RAM |
|---|---|---|
| before (banner image) | 127.7 KB | 25.5 KB |
| **now (`release`)** | **252.5 KB (31 %)** | **68.9 KB (27.7 %)** |
| Headroom | **549 KiB** against 200 required | 174 KiB |

The jump is BLE: the SoftDevice binding and Bluefruit52Lib. **Gate 0.6 stays passed**,
now with a number that applies to the finished device. The radio is still to come on top.

## The heartbeat

The application is silent in release — `CLAUDE.md` §6 compiles serial output out there,
and a node on a hillside has nobody to talk to. But a silent image is also one that nobody
can tell apart from a hung one, and this one is new. In the debug build it says every ten
seconds what it is doing; `build_guard.h` refuses a release image with
`MAXL_LOG_LEVEL > 0`, so it cannot slip in by accident.
