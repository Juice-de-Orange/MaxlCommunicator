#!/usr/bin/env python3
"""Verify that link/ and app/ contain no dynamic allocation.

CLAUDE.md 3: "No dynamic allocation in link/ or app/ -- fixed buffers only,
verified by a build check on malloc/new in those translation units. RadioLib and
the Arduino core allocate; that is out of scope and not something to fight."

Which is why docs/decisions D3 puts IRadioLink and IBlockStore in hal/: RadioLib's
types AND its allocations stay below the line this script draws. If the allowlist
below can stay empty, the layering was cut in the right place.

This reads the object files, not the linked image. Unreferenced objects are
dropped by --gc-sections, so a link-time check would quietly pass for any module
the application has not wired up yet.

IMPORTANT: this only works without -flto. With LTO the .o files hold GIMPLE
bytecode, nm finds no symbols, and the check would silently pass for ever. The
script refuses to report success if it found no symbols at all, so that failure
mode is loud rather than silent.
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.path.dirname(HERE)

# Layers the rule applies to. hal/, ble/ and ui/ are out of scope: they talk to
# libraries that allocate, which CLAUDE.md 3 explicitly declines to fight.
CHECKED_LAYERS = ("link", "app")

FORBIDDEN = {
    "malloc": "malloc",
    "calloc": "calloc",
    "realloc": "realloc",
    "free": "free",
    "_Znwm": "operator new",
    "_Znam": "operator new[]",
    "_Znwj": "operator new",
    "_Znaj": "operator new[]",
    "_ZdlPv": "operator delete",
    "_ZdaPv": "operator delete[]",
    "_ZdlPvm": "operator delete (sized)",
    "_ZdaPvm": "operator delete[] (sized)",
    "strdup": "strdup",
    "_sbrk": "sbrk",
}

ALLOWLIST_FILE = os.path.join(FIRMWARE_DIR, "alloc_allowlist.txt")

# nm output: "         U _Znwm" or "00000000 T foo"
NM_LINE = re.compile(r"^\s*(?:[0-9a-fA-F]+)?\s*([A-Za-z])\s+(\S+)\s*$")


def load_allowlist():
    entries = set()
    if not os.path.exists(ALLOWLIST_FILE):
        return entries
    with open(ALLOWLIST_FILE, encoding="utf-8") as handle:
        for line in handle:
            line = line.split("#", 1)[0].strip()
            if line:
                entries.add(line)
    return entries


def find_objects(build_dir):
    objects = []
    for layer in CHECKED_LAYERS:
        layer_dir = os.path.join(build_dir, "src", layer)
        if not os.path.isdir(layer_dir):
            continue
        for dirpath, _dirnames, filenames in os.walk(layer_dir):
            for filename in sorted(filenames):
                if filename.endswith(".o"):
                    objects.append(os.path.join(dirpath, filename))
    return objects


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--build-dir",
        default=os.path.join(FIRMWARE_DIR, ".pio", "build", "debug"),
        help="PlatformIO build directory to inspect",
    )
    parser.add_argument("--nm", default=None, help="nm binary (default: search the toolchain)")
    args = parser.parse_args()

    nm = args.nm or find_nm()
    if nm is None:
        print("check_no_alloc: no arm-none-eabi-nm found; build the firmware first",
              file=sys.stderr)
        return 2

    objects = find_objects(args.build_dir)
    if not objects:
        print("check_no_alloc: no object files under {}/src/{{{}}}".format(
            args.build_dir, ",".join(CHECKED_LAYERS)), file=sys.stderr)
        print("  Build first: pio run -e debug", file=sys.stderr)
        return 2

    allowlist = load_allowlist()
    findings = []
    symbols_seen = 0

    for path in objects:
        try:
            output = subprocess.check_output([nm, "--undefined-only", "-C", "-j", path],
                                             text=True, stderr=subprocess.DEVNULL)
        except subprocess.CalledProcessError:
            output = ""
        # -j is not available on every nm build; fall back to the parsed form.
        if not output.strip():
            output = subprocess.check_output([nm, "--undefined-only", path], text=True)
            output = "\n".join(
                match.group(2) for match in
                (NM_LINE.match(line) for line in output.splitlines()) if match
            )

        relative = os.path.relpath(path, args.build_dir)
        for symbol in output.split():
            symbols_seen += 1
            base = symbol.lstrip("_") if symbol.startswith("__") else symbol
            for forbidden, human in FORBIDDEN.items():
                if symbol == forbidden or base == forbidden:
                    entry = "{}:{}".format(relative, symbol)
                    if entry in allowlist or symbol in allowlist:
                        continue
                    findings.append((relative, symbol, human))

    print("check_no_alloc: {} object files in {}, {} undefined symbols".format(
        len(objects), "/".join(CHECKED_LAYERS), symbols_seen))

    if symbols_seen == 0:
        # The -flto trap. With LTO the objects hold GIMPLE bytecode and nm finds
        # nothing, so a clean run would mean nothing at all.
        print("\n  ERROR: nm reported no symbols whatsoever.", file=sys.stderr)
        print("  That is what -flto looks like: the objects hold GIMPLE bytecode and this",
              file=sys.stderr)
        print("  check would pass silently for ever. Build without LTO.", file=sys.stderr)
        return 1

    if findings:
        print("\n  DYNAMIC ALLOCATION FOUND ({}):".format(len(findings)))
        for relative, symbol, human in findings:
            print("    {} references {} ({})".format(relative, human, symbol))
        print("\n  CLAUDE.md 3: fixed buffers only in link/ and app/.")
        print("  If a reference is genuinely unavoidable, add it to alloc_allowlist.txt")
        print("  WITH a reason -- an unexplained entry is how this check stops meaning anything.")
        return 1

    if allowlist:
        print("  allowlist has {} entr(ies)".format(len(allowlist)))
    else:
        print("  no dynamic allocation, and the allowlist is empty")
    return 0


def find_nm():
    for candidate in ("arm-none-eabi-nm",):
        for directory in os.environ.get("PATH", "").split(os.pathsep):
            path = os.path.join(directory, candidate)
            if os.path.isfile(path) and os.access(path, os.X_OK):
                return path
    # PlatformIO keeps its toolchain outside PATH.
    packages = os.path.expanduser("~/.platformio/packages")
    for root, _dirs, files in os.walk(packages):
        if "arm-none-eabi-nm" in files:
            return os.path.join(root, "arm-none-eabi-nm")
    return None


if __name__ == "__main__":
    sys.exit(main())
