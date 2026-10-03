# Host tests

The portable firmware modules, compiled and run on a normal computer.

These are **the same `.cpp` files** the ARM build compiles — not copies, not stubs.
That is the point: decision D3 puts `IRadioLink` and `IBlockStore` in `hal/` so that
`link/` depends on interfaces rather than on RadioLib. If `link/` builds on a plain host
toolchain with no Arduino core in sight, the layering claim is demonstrated rather than
asserted.

**276 test cases**, plus 7 224 checks in the link simulation and 39 in the
node simulation.

## Running

```bash
# In a container, if the machine has no host g++ (the usual case):
firmware/tools/hosttest.sh

# Directly, if g++ and make are available:
make -C firmware/test test
```

Only `make`, `g++` and `python3` are needed — no CMake. That is why it runs unchanged
in the standard `gcc` image and on a bare CI runner.

## What is in it

| Path | Contents |
|---|---|
| `unit/` | One test per module, with doctest |
| `fakes/` | Clock, block store, radio channel, BLE transport, key store, display — all of them can fail on purpose |
| `sim/two_node.cpp` | Two nodes over a simulated channel, hours of simulated time |
| `sim/two_node_app.cpp` | Two **complete** nodes: a message is written into A's GATT characteristic and found again in B's journal |
| `vendor/doctest.h` | Test framework, one header, MIT |

The fakes are the real lever. `FakeBlockStore` knows a power failure and a
write budget that runs out — otherwise the sentences "if the counter cannot be persisted, the
device must refuse to transmit" (`CLAUDE.md` §2.1) and "a budget that a power cycle can clear
is not a budget" (§1.2) cannot be tested at all. `FakeDisplay` remembers what it was handed,
and thereby makes it testable that a full refresh takes the whole area and a partial only
what changed — the difference between 471 and 324 ms on the real panel (Gate 1.1).

## Which layers are built here — and which are not

`link/`, `ble/`, `app/` and `ui/` are collected **by glob**. They are portable by
construction; a new module that does *not* build here is exactly what this build is meant to catch,
and a hand-maintained list would let one of them slip through.

`hal/` is **not** globbed, and this asymmetry is intentional. Most of it talks to
the Arduino core — Wire, SPI, GxEPD2, RadioLib — and could never build here. The exceptions
are listed **by name** in the `Makefile`:

| File | Why it is here |
|---|---|
| `canvas.cpp` | 200×200 drawing surface against which every screen can be tested |
| `refresh_policy.cpp` | the ghosting counter — it turns Gate 4.1 into a counter instead of an hour of watching |
| `press_detector.cpp` | debouncing and short/long — Gates 1.4 and 1.5 are counts |
| `bme280_compensation.cpp` | Bosch's integer arithmetic, the place with the easily wrong shifts |
| `nmea.cpp` | GGA and RMC, cross-checked against real sentences from the L76K |

By name and not by pattern, so that a new driver does not accidentally pull the Arduino core
into this build. Anyone adding a file must first consider whether it is really
portable.

**A consequence that costs time if you forget it:** a change to a `hal/` driver is
not compiled here at all. Every change in `hal/` must be followed by a
`pio run -d firmware -e debug`, otherwise the error only shows up when flashing.

## What this is **not**

Not a replacement for `docs/test-plan.md`. Every gate except **2.15** needs two devices, two
antennas and, from Phase 2 on, a step attenuator. The simulated channel delivers or it does not;
it models neither preamble detection nor the RxDutyCycle sniff cycle, neither collisions
nor TCXO start-up nor the LoRaWAN gateways 50 kHz next to the default channel.

Green here means: the logic is internally consistent. Not: it runs on an nRF52840.

And not: a screen looks right either. What is checked is that the same model yields the same
pixels, that the three delivery states differ and that nobody draws into the
footer — whether the result is *readable* is Gates 4.2 and 4.3 and needs eyes.

## Test vectors

`test-vectors/*.json` is read by **both** implementations — here via
`tools/gen_vectors.py`, which turns it into a header of byte arrays, and in `bridge/test`
directly as JSON. The vectors are written by hand from the specification, not generated from either
of the two implementations: a generator would bake in one side's bugs
and make the cross-check worthless.

Cross-check that both sides really read them: corrupt one byte in a vector file —
afterwards **both** test suites must fail.
