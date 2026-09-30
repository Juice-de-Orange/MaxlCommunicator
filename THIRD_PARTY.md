# Third-party components and licences

The project's own code is under the [MIT License](LICENSE). It uses or contains the following
third-party components.

## Vendored in this repository

| Component | Path | Licence | Note |
|---|---|---|---|
| TinyCrypt (AES-128, CCM, utils) | `firmware/lib/tinycrypt/` | BSD-3-Clause (Intel) | From `zephyrproject-rtos/tinycrypt`; headers intact. |
| doctest | `firmware/test/vendor/doctest.h` | MIT (Viktor Kirilov) | Host test framework. |
| T-Echo board variant | `firmware/variants/t-echo/variant.{h,cpp}` | LGPL-2.1-or-later | Derived from the Adafruit nRF52 core's variant template (Arduino LLC, Sandeep Mistry, Adafruit); headers intact. These two files stay under LGPL-2.1+. |

## Fetched at build time (not in this repository)

Firmware (PlatformIO `lib_deps` and the platform):

| Component | Licence |
|---|---|
| Adafruit nRF52 Arduino core (incl. Bluefruit) | LGPL-2.1 |
| Adafruit TinyUSB | MIT |
| Adafruit SPIFlash | MIT |
| RadioLib | MIT |
| **GxEPD2** | **GPL-3.0** |
| Adafruit GFX | BSD |

**Firmware binaries are GPL-3.0.** The firmware links GxEPD2, which is GPL-3.0, so a distributed
firmware image (`.uf2`, `.hex`, `.zip`) is a combined work under GPL-3.0 — even though this
project's own sources are MIT. If you distribute firmware images, provide the corresponding source
(this repository at the matching commit) under GPL-3.0 terms.

Phone bridge (`bridge/`) and dashboard (`web/`) npm dependencies are permissive (MIT, ISC,
Apache-2.0, BSD, MIT-0, BlueOak, CC0). Exceptions, none of which affects the MIT licence of this
project's code: `@img/sharp-libvips-*` (LGPL-3.0-or-later, the dynamically linked native library of
`sharp`; a container image that ships it should carry its notice), `lightningcss` (MPL-2.0, build
tool, unmodified) and `caniuse-lite` (CC-BY-4.0, development only).

Android client (`android/`): androidx appcompat and core-ktx, kotlinx-coroutines, kotlin-test —
all Apache-2.0.
