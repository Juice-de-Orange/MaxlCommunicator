#!/usr/bin/env python3
"""Flash and RAM budget, and the Gate 0.6 headroom requirement.

docs/test-plan.md gate 0.6: "Internal flash and RAM budget from the map file --
recorded; >= 200 KB flash headroom remains."

Records the numbers rather than only answering yes or no. A gate that prints
"pass" teaches you nothing on the day it starts failing; one that prints
"493 KiB free, was 512 KiB last month" tells you what happened.

Reads the ELF's section sizes via `size`, and the limits from the board
definition -- boards/t-echo.json's maximum_size matches the linker script
nrf52840_s140_v6.ld exactly (0xED000 - 0x26000 = 0xC7000), which is the check that
the toolchain chain is intact.
"""

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.path.dirname(HERE)
BOARD_FILE = os.path.join(FIRMWARE_DIR, "boards", "t-echo.json")

# Gate 0.6.
REQUIRED_FLASH_HEADROOM = 200 * 1024


def find_tool(name):
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        path = os.path.join(directory, name)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    packages = os.path.expanduser("~/.platformio/packages")
    for root, _dirs, files in os.walk(packages):
        if name in files:
            return os.path.join(root, name)
    return None


def find_elf(build_dir):
    if not os.path.isdir(build_dir):
        return None
    candidates = [
        os.path.join(build_dir, name)
        for name in sorted(os.listdir(build_dir))
        if name.endswith(".elf")
    ]
    return candidates[0] if candidates else None


def section_sizes(size_tool, elf):
    """Per-section sizes from `size -A`.

    NOT the Berkeley format. Berkeley folds every allocated NOBITS section into
    `bss`, and on this board that includes `.heap` -- the Adafruit core reserves
    all remaining RAM for it, currently about 222 KiB. Summing that as "RAM used"
    reports 95 % occupancy on an image that actually uses 8 KiB, which is the kind
    of number that makes people stop reading the tool.

    So the sections are separated: static RAM is .data + .bss, and the heap and
    stack reservations are reported beside it as what is left over.
    """
    output = subprocess.check_output([size_tool, "-A", elf], text=True)
    sizes = {}
    for line in output.splitlines():
        match = re.match(r"^(\.\S+)\s+(\d+)\s+(\d+)\s*$", line.strip())
        if match:
            sizes[match.group(1)] = int(match.group(2))
    if ".text" not in sizes:
        raise RuntimeError("could not parse `size -A` output:\n" + output)
    return sizes


def human(value):
    return "{:,} B ({:.1f} KiB)".format(value, value / 1024.0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir",
                        default=os.path.join(FIRMWARE_DIR, ".pio", "build", "release"))
    parser.add_argument("--size", default=None)
    args = parser.parse_args()

    elf = find_elf(args.build_dir)
    if elf is None:
        print("check_size: no .elf in {}".format(args.build_dir), file=sys.stderr)
        print("  Build first: pio run -e release", file=sys.stderr)
        return 2

    size_tool = args.size or find_tool("arm-none-eabi-size")
    if size_tool is None:
        print("check_size: arm-none-eabi-size not found", file=sys.stderr)
        return 2

    with open(BOARD_FILE, encoding="utf-8") as handle:
        board = json.load(handle)
    flash_limit = board["upload"]["maximum_size"]
    ram_limit = board["upload"]["maximum_ram_size"]

    sizes = section_sizes(size_tool, elf)
    text = sizes.get(".text", 0)
    data = sizes.get(".data", 0)
    bss = sizes.get(".bss", 0)
    heap = sizes.get(".heap", 0)
    stack = sizes.get(".stack_dummy", 0) + sizes.get(".stack", 0)

    flash_used = text + data + sizes.get(".ARM.exidx", 0)
    ram_used = data + bss
    flash_free = flash_limit - flash_used
    ram_free = ram_limit - ram_used

    print("check_size: {}".format(os.path.basename(elf)))
    print("")
    print("  Flash  used {:>22}  of {:>22}  ({:.1f} %)".format(
        human(flash_used), human(flash_limit), 100.0 * flash_used / flash_limit))
    print("         free {:>22}".format(human(flash_free)))
    print("  RAM    used {:>22}  of {:>22}  ({:.1f} %)".format(
        human(ram_used), human(ram_limit), 100.0 * ram_used / ram_limit))
    print("         free {:>22}".format(human(ram_free)))
    print("")
    print("  sections: .text {}  .data {}  .bss {}".format(
        human(text), human(data), human(bss)))
    if heap or stack:
        print("  reserved: .heap {}  .stack {}".format(human(heap), human(stack)))
        print("            -- what is left over, not what is used. It shrinks as .bss")
        print("               grows, and link/ and app/ are all fixed buffers by design")
        print("               (CLAUDE.md 3), so .bss is the number to watch.")
    print("")
    print("  Flash limit is 0xC7000, from the S140 v6 linker script: the application")
    print("  starts at 0x26000 (after the SoftDevice) and ends at 0xED000 (before the")
    print("  bootloader settings). RAM excludes what the SoftDevice reserves.")
    print("")

    if flash_free < REQUIRED_FLASH_HEADROOM:
        print("  GATE 0.6 FAILED: {} of flash headroom, {} required".format(
            human(flash_free), human(REQUIRED_FLASH_HEADROOM)))
        return 1

    print("  Gate 0.6: {} of flash headroom, {} required -- pass".format(
        human(flash_free), human(REQUIRED_FLASH_HEADROOM)))
    print("")
    print("  Note: unreferenced modules are dropped by --gc-sections, so this figure")
    print("  only covers what the application actually calls. It will grow as app/ and")
    print("  ui/ wire up link/ -- re-run it then rather than trusting today's number.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
