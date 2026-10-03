#!/usr/bin/env python3
"""Inspect the flash contents of an Adafruit-UF2-bootloader nRF52 device.

The UF2 bootloader exposes the entire flash as CURRENT.UF2 on its mass-storage
volume. This script reconstructs the raw image from it and reports:

  * the SoftDevice family, version and size (and therefore the application base)
  * which flash pages are actually programmed
  * whether an application is present at all

That is exactly the ground truth needed by docs/test-plan.md gate 0.6, and it is
the first thing to run whenever a node stops booting.

Usage:
    python tools/uf2_inspect.py [PATH_TO_CURRENT.UF2]

With no argument the script looks for a mounted volume labelled TECHOBOOT.

Exit status: 0 only if an application image was found. 1 if there is nothing to
read, or if the dump holds no recognisable SoftDevice or no plausible
application -- so a script can ask "is this node flashed?" without parsing the
report.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

UF2_MAGIC_START0 = 0x0A324655  # "UF2\n"
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_BLOCK_SIZE = 512

MBR_SIZE = 0x1000
# The nRF SoftDevice information structure lives at MBR_SIZE + 0x2000.
SD_INFO_OFFSET = MBR_SIZE + 0x2000
SD_INFO_MAGIC = 0x51B1E5DB

PAGE_SIZE = 0x1000


def find_uf2() -> Path | None:
    """Locate CURRENT.UF2 on a mounted UF2 bootloader volume."""
    candidates = []
    if sys.platform == "win32":
        for letter in "DEFGHIJKLMNOPQRSTUVWXYZ":
            candidates.append(Path(f"{letter}:/CURRENT.UF2"))
    else:
        for base in (Path("/media"), Path("/Volumes"), Path("/run/media")):
            if base.is_dir():
                candidates.extend(base.glob("**/CURRENT.UF2"))
    for path in candidates:
        try:
            if path.is_file():
                return path
        except OSError:
            continue
    return None


def read_uf2(path: Path) -> tuple[dict[int, bytes], set[int]]:
    """Parse a UF2 file into {target_address: payload} plus the family IDs seen."""
    blocks: dict[int, bytes] = {}
    families: set[int] = set()
    data = path.read_bytes()
    if len(data) % UF2_BLOCK_SIZE:
        print(f"warning: {path} is not a multiple of {UF2_BLOCK_SIZE} bytes")
    for offset in range(0, len(data) - UF2_BLOCK_SIZE + 1, UF2_BLOCK_SIZE):
        block = data[offset : offset + UF2_BLOCK_SIZE]
        start0, start1, _flags, addr, payload_len, _seq, _total, family = struct.unpack(
            "<8I", block[:32]
        )
        (end,) = struct.unpack("<I", block[508:512])
        if start0 != UF2_MAGIC_START0 or start1 != UF2_MAGIC_START1 or end != UF2_MAGIC_END:
            print(f"warning: bad UF2 magic in block {offset // UF2_BLOCK_SIZE}, stopping")
            break
        if payload_len > 476:
            print(f"warning: implausible payload length {payload_len}, stopping")
            break
        blocks[addr] = block[32 : 32 + payload_len]
        families.add(family)
    return blocks, families


def flatten(blocks: dict[int, bytes]) -> tuple[int, bytearray]:
    """Concatenate contiguous blocks starting at the lowest address."""
    if not blocks:
        return 0, bytearray()
    base = min(blocks)
    image = bytearray()
    addr = base
    while addr in blocks:
        chunk = blocks[addr]
        image += chunk
        addr += len(chunk)
    return base, image


def describe_softdevice(base: int, image: bytes) -> int | None:
    """Print the SoftDevice info structure. Returns the application base address."""
    off = SD_INFO_OFFSET - base
    if off < 0 or off + 0x18 > len(image):
        print("SoftDevice info structure is outside the dumped range.")
        return None

    (_size,) = struct.unpack("<I", image[off + 0x00 : off + 0x04])
    (magic,) = struct.unpack("<I", image[off + 0x04 : off + 0x08])
    (sd_size,) = struct.unpack("<I", image[off + 0x08 : off + 0x0C])
    (fwid,) = struct.unpack("<I", image[off + 0x0C : off + 0x10])
    (sd_id,) = struct.unpack("<I", image[off + 0x10 : off + 0x14])
    (version,) = struct.unpack("<I", image[off + 0x14 : off + 0x18])

    print(f"SoftDevice information structure @ 0x{SD_INFO_OFFSET:X}")
    ok = magic == SD_INFO_MAGIC
    print(f"  magic       : 0x{magic:08X} ({'valid' if ok else 'INVALID'})")
    if not ok:
        print("  -> no recognisable SoftDevice; the rest of this report is unreliable.")
        return None

    major, minor, patch = version // 1000000, (version // 1000) % 1000, version % 1000
    print(f"  SoftDevice  : S{sd_id} v{major}.{minor}.{patch}")
    print(f"  FWID        : 0x{fwid & 0xFFFF:04X}")
    print(f"  size        : 0x{sd_size:06X} ({sd_size} B)")
    print(f"  -> application base address: 0x{sd_size:X}")
    return sd_size


def map_pages(base: int, image: bytes, app_base: int | None) -> bool:
    """Print a run-length map of programmed vs erased pages.

    Returns whether a plausible application image was found.
    """
    print("\nFlash page map (4 KiB granularity, 0xFF = erased):")
    runs: list[list] = []
    for addr in range(base, base + len(image), PAGE_SIZE):
        page = image[addr - base : addr - base + PAGE_SIZE]
        used = any(b != 0xFF for b in page)
        if runs and runs[-1][2] == used:
            runs[-1][1] = addr + len(page)
        else:
            runs.append([addr, addr + len(page), used])
    for start, end, used in runs:
        state = "PROGRAMMED" if used else "erased    "
        print(f"  0x{start:06X}-0x{end:06X}  {state}  {(end - start) / 1024:6.0f} KiB")

    if app_base is None:
        return False
    off = app_base - base
    if off < 0 or off + 8 > len(image):
        print("\nApplication region is outside the dumped range.")
        return False
    sp, reset = struct.unpack("<II", image[off : off + 8])
    print(f"\nApplication vector table @ 0x{app_base:X}:")
    if sp == 0xFFFFFFFF and reset == 0xFFFFFFFF:
        print("  erased -> NO APPLICATION IS FLASHED.")
        print("  The device will stay in the bootloader until an image is written.")
        return False
    plausible = 0x20000000 <= sp <= 0x20040000 and bool(reset & 1)
    print(f"  initial SP  : 0x{sp:08X}")
    print(f"  reset vector: 0x{reset:08X}")
    print(f"  -> {'looks like a valid image' if plausible else 'does NOT look like a valid image'}")
    return plausible


def main(argv: list[str]) -> int:
    if len(argv) > 1:
        path = Path(argv[1])
    else:
        found = find_uf2()
        if found is None:
            print(
                "No CURRENT.UF2 found. Put the node into the bootloader "
                "(double-tap reset) or pass the path explicitly."
            )
            return 1
        path = found

    if not path.is_file():
        print(f"Not a file: {path}")
        return 1

    print(f"Reading {path}\n")

    info = path.with_name("INFO_UF2.TXT")
    if info.is_file():
        print("INFO_UF2.TXT:")
        for line in info.read_text(errors="replace").splitlines():
            if line.strip():
                print(f"  {line.strip()}")
        print()

    blocks, families = read_uf2(path)
    if not blocks:
        print("No valid UF2 blocks found.")
        return 1
    base, image = flatten(blocks)
    print(f"Reconstructed 0x{base:06X}-0x{base + len(image):06X} ({len(image)} B)")
    print("Family IDs: " + ", ".join(f"0x{f:08X}" for f in sorted(families)))
    print()

    app_base = describe_softdevice(base, image)
    return 0 if map_pages(base, image, app_base) else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
