# The touch pad does not respond through the closed enclosure

| | |
|---|---|
| Date | 2026-08-31, evening |
| Node | **A**, `0123ABCD4567EF01` |
| Sketch | 09, with pull cross-check and LED feedback |
| Enclosure | closed, never opened |

**Result: not a single level change on P0.11, across several series of attempts and a
complete sweep of the enclosure.** Gate 1.5 is therefore not reachable, and gate 1.4
stands at 367 of 500 (clean, see below).

## What is ruled out

| Suspicion | How it was ruled out |
|---|---|
| Wrong pin | `PIN_BUTTON_TOUCH = (0 + 11)`, and `g_ADigitalPinMap[11] = 11` → P0.11. Four external sources name the same pin (Meshtastic variant, LilyGO `utilities.h`, LilyGO README, cfr34k) |
| Wrong polarity | `touch.edges` counts **raw** level changes, independent of the interpretation. Zero edges means: the pin did not move |
| Pin floating, chip missing | `touch.with_pullup = 0` — the pin stays LOW even though the internal 13 kOhm pull-up pulls against it. Something is actively driving it |
| No supply | According to the schematic, U6 hangs on `VDD_nRF`, not on the switched `VDD_POWR`. It is powered as soon as the nRF runs |
| Wrong operating technique | Laid flat, held for 2 s, ≥ 4 s pause because of the self-calibration, finger away at boot — all per the datasheet |
| Wrong spot | The whole enclosure swept: front top left and right of the acrylic window, above the display, top narrow edge, both side edges, back. With LED feedback on the pin, i.e. without round-trip delay |

## What remains

The TTP223 is **fitted, powered and drives its output** — it rests LOW, which corresponds to
the active-high configuration that the schematic (TOG and AHLB open) and the datasheet
(`TOG=0, AHLB=0` → direct mode, CMOS active high) prescribe.

It just does not trigger. The electrode is not a copper area but hangs via C42 (10 pF) on
`P3`, a spring contact. A spring couples to the enclosure surface, and C42 deliberately lowers
the sensitivity. Both together make poor or missing coupling the most obvious explanation —
**it can only be proven with the enclosure open, and that deliberately has not been done.**

## What this means for the design

`CLAUDE.md` §3.2 gives the short touch "next screen", i.e. the **primary navigation**, and the
long touch the screen's main action. Neither is operable on this device.

§1.7 limits the damage — "Button interaction must never be required for correct operation,
all configuration is possible over BLE" — but the five screens could no longer be paged
through on the device. That is a design question, not a measurement question, and is open as
**D17**.

## The button next to it is fine

```
RESULT button.edges            = 734
RESULT button.activations      = 367
RESULT button.longest_press_ms = 414
```

734 = 2 × 367, exactly: every press exactly one falling and one rising edge, no double
triggering, no bounce. Gate 1.4 requires 500 counted presses; 367 uncounted ones show the
quality, not the completeness. The gate stays open and is cheap to catch up on.
