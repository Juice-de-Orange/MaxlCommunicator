# Phase 0 — Gate 0.6 Flash and RAM budget from the map file

| | |
|---|---|
| Date | 2026-08-31 |
| Node | — (host evaluation of the release image) |
| Firmware | 0.1.0, `maxl-release-0.1.0-g338581f.elf` |
| Environment | `release` |
| Measuring equipment | `firmware/scripts/check_size.py` against the ELF and the map file |

## Result: PASS

Gate 0.6 requires: *"Internal flash and RAM budget from the map file — recorded;
≥ 200 KB flash headroom remains."*

| Check | Expected | Measured | Result |
|---|---|---|---|
| Flash headroom | ≥ 200 KiB | **671.3 KiB** | PASS |
| Flash used | — | 124.7 KiB of 796.0 KiB (15.7 %) | |
| RAM used | — | 24.9 KiB of 243.0 KiB (10.3 %) | |
| `.bss` | — | 23.9 KiB | |

```
Flash  used  127,708 B  of  815,104 B  (15.7 %)
       free  687,396 B
RAM    used   25,548 B  of  248,832 B  (10.3 %)
       free  223,284 B
sections: .text 126,636 B  .data 1,064 B  .bss 24,484 B
```

The flash limit `0xC7000` comes from the S140 v6 linker script: the application starts
at `0x26000` behind the SoftDevice and ends at `0xED000` before the
bootloader settings.

## Observations

**The number is more generous today than it will stay**, and `check_size.py` says so on its
own: `--gc-sections` throws away everything the application does not call. `src/main.cpp`
does not wire up `ui/` yet, so the whole UI layer — canvas, five screens, font, geometry — is
**not included** in this measurement. The surcharge can be read off the bring-up image that
really uses it:

| Image | Flash | RAM | contains |
|---|---|---|---|
| `release` | 127.7 KB | 25.5 KB | `link/`, `ble/`, parts of `app/` — `ui/` is thrown away |
| Sketch 11 | 161.1 KB | 31.2 KB | plus the `hal/` drivers, GxEPD2, the screens |
| **Sketch 12** | **186.5 KB** | **64.7 KB** | **everything except the radio**: node, queue, journal, peers, BLE-capable stack, UI controller with two canvases |

**Sketch 12 is the reliable number.** It wires up everything the finished device wires up
too, just without the radio loop — `--gc-sections` no longer throws anything away there.

Even so: **616 KB flash free and 179 KB RAM free.** The RAM number rises the most
(25 → 65 KB), and that is explainable and intended: the event journal is
128 × 70 = 9 KB, the message queue 1.5 KB, the two canvases in the UI controller
10 KB and GxEPD2's own buffer another 5 KB. All fixed buffers, as `CLAUDE.md` §3
requires — none of it grows at runtime.

The radio is still to come on top: RadioLib plus the crypto buffers. With over half a
megabyte of headroom the gate is not tight and will not become tight.

`.bss` is the number to watch: `link/` and `app/` work exclusively with fixed buffers per
`CLAUDE.md` §3, so the heap is what is left over, not what is needed.

**To be measured again** as soon as `main.cpp` wires up the layers and the radio is added.
The `release` number above applies to today's `release` image, not to the finished device;
sketch 12 is the better estimate for that.
