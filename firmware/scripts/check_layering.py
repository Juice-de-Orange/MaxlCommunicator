#!/usr/bin/env python3
"""Enforce the layer policy from layering.toml.

CLAUDE.md 3: "Five modules, strictly layered. Lower layers must not call into
higher ones." docs/decisions/0001-open-decisions.md D3 records the resulting
permission matrix and why IRadioLink sits in hal/ rather than in link/.

This is a source-level check, not a link-time one, so it needs no compiler and
runs in every CI job. It only looks at quoted includes: <angle> includes are the
toolchain, the Arduino core and vendored third-party headers, none of which
belong to a layer.

Exit code 0 if the policy holds, 1 if it does not.
"""

import os
import re
import sys

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.path.dirname(HERE)
SRC_DIR = os.path.join(FIRMWARE_DIR, "src")
POLICY_FILE = os.path.join(FIRMWARE_DIR, "layering.toml")

SOURCE_SUFFIXES = (".h", ".hpp", ".c", ".cpp", ".cc")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)

ROOT_LAYER = "root"


def load_policy(path):
    with open(path, "rb") as handle:
        policy = tomllib.load(handle)
    directories = policy["layers"]
    allowed = policy["allowed"]

    # Every layer may include itself, root included -- main.cpp sits beside
    # build_guard.h and has to be able to include it. Spelling this out in the
    # TOML for each layer would be noise, so it is added here instead.
    for layer in list(directories.values()) + [ROOT_LAYER]:
        if layer not in allowed:
            allowed[layer] = []
        if layer not in allowed[layer]:
            allowed[layer].append(layer)
    return directories, allowed


def layer_of(rel_path, directories):
    """Which layer a path under src/ belongs to. Files directly in src/ are root."""
    head = rel_path.split(os.sep)[0]
    if head in directories:
        return directories[head]
    return ROOT_LAYER


def resolve(include, source_rel):
    """Turn an include as written into a path relative to src/, or None.

    Both spellings are accepted: relative to the including file ("frame.h" from
    within link/) and relative to src/ ("link/frame.h"), because platformio.ini
    puts -I src on the command line.
    """
    candidates = [
        os.path.normpath(os.path.join(os.path.dirname(source_rel), include)),
        os.path.normpath(include),
    ]
    for candidate in candidates:
        if os.path.isfile(os.path.join(SRC_DIR, candidate)):
            return candidate
    return None


def main():
    directories, allowed = load_policy(POLICY_FILE)
    violations = []
    unresolved = []
    checked = 0

    for dirpath, _dirnames, filenames in os.walk(SRC_DIR):
        for filename in sorted(filenames):
            if not filename.endswith(SOURCE_SUFFIXES):
                continue
            abs_path = os.path.join(dirpath, filename)
            source_rel = os.path.relpath(abs_path, SRC_DIR)
            checked += 1
            with open(abs_path, encoding="utf-8") as handle:
                text = handle.read()

            source_layer = layer_of(source_rel, directories)
            for include in INCLUDE_RE.findall(text):
                target_rel = resolve(include, source_rel)
                if target_rel is None:
                    # A quoted include that is not under src/ -- a vendored
                    # header, for instance. Reported so a typo cannot hide as a
                    # silently ignored include.
                    unresolved.append((source_rel, include))
                    continue
                target_layer = layer_of(target_rel, directories)
                if target_layer not in allowed[source_layer]:
                    violations.append(
                        (source_rel, source_layer, target_rel, target_layer)
                    )

    print("check_layering: {} source files under src/".format(checked))

    if unresolved:
        print("\n  includes that do not resolve under src/ (not an error, but check them):")
        for source_rel, include in unresolved:
            print('    {} -> "{}"'.format(source_rel, include))

    if violations:
        print("\n  LAYER VIOLATIONS ({}):".format(len(violations)))
        for source_rel, source_layer, target_rel, target_layer in violations:
            print(
                "    {} [{}] must not include {} [{}]".format(
                    source_rel, source_layer, target_rel, target_layer
                )
            )
        print("\n  The matrix is in layering.toml; the reasoning is in")
        print("  docs/decisions/0001-open-decisions.md D3.")
        return 1

    print("  no layer violations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
