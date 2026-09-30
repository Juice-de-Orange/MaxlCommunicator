"""Refuse to build a release image with a development key, and pass one to debug.

CLAUDE.md 6: "No secrets in the repository. Keys are provisioned at runtime. The
development-key compile flag must fail a release build."

The key comes from the environment variable MAXL_DEV_KEY -- 32 hex characters --
and is never written to a file. README.md states this as the rule; this script is
what makes it true. A key that only ever exists in an environment variable and in
the object code of a debug build is a key that `git grep` cannot find, because
there has never been one in the tree to find.

Paired with src/build_guard.h, which fails the compile as well. One check is a
policy; two checks in different layers is a guard, and it means a build invoked
around this pre-hook still fails.
"""

import os
import re
import sys

Import("env")  # noqa: F821  (injected by PlatformIO)

DEV_KEY_VAR = "MAXL_DEV_KEY"
HEX_KEY = re.compile(r"^[0-9a-fA-F]{32}$")

build_type = env.GetProjectOption("build_type", "debug")  # noqa: F821
pio_env = env["PIOENV"]  # noqa: F821
is_release = pio_env == "release" or build_type == "release"

dev_key = os.environ.get(DEV_KEY_VAR, "").strip()

if is_release:
    if dev_key:
        print("")
        print("=" * 74)
        print("  RELEASE BUILD REFUSED")
        print("")
        print("  {} is set in the environment.".format(DEV_KEY_VAR))
        print("")
        print("  CLAUDE.md 2.1 permits a development key behind a compile flag for bench")
        print("  work in phases 2-3, and requires that flag to fail the build in release")
        print("  configuration. This is that requirement.")
        print("")
        print("  Unset it and build again:   unset {}".format(DEV_KEY_VAR))
        print("=" * 74)
        print("")
        sys.exit(1)
    print("release_guard: release build, no development key. Good.")
else:
    if dev_key:
        if not HEX_KEY.match(dev_key):
            print("")
            print("  {} must be exactly 32 hex characters (a 128-bit AES key).".format(
                DEV_KEY_VAR))
            print("  Got {} character(s). Refusing to build with a malformed key rather".format(
                len(dev_key)))
            print("  than silently producing a node that cannot talk to its peer.")
            print("")
            sys.exit(1)
        env.Append(  # noqa: F821
            CPPDEFINES=[("MAXL_DEV_KEY", env.StringifyMacro(dev_key.lower()))]  # noqa: F821
        )
        print("release_guard: development key taken from ${} (debug build only)".format(
            DEV_KEY_VAR))
    else:
        print("release_guard: debug build, no development key set")
