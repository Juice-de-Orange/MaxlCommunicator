"""Refuse to build a release image with a development key, and pass one to debug.

CLAUDE.md 6: "No secrets in the repository. Keys are provisioned at runtime. The
development-key compile flag must fail a release build."

The key comes from the environment variable TRAILTALK_DEV_KEY -- 32 hex
characters -- and is never written to a file. MAXL_DEV_KEY, its name from before
the rename, is still read with a warning (renamed_env.py), and a release build
refuses either. README.md states this as the rule; this script is what makes it
true. A key that only ever exists in an environment variable and in
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

sys.path.insert(0, os.path.join(env["PROJECT_DIR"], "scripts"))  # noqa: F821
import renamed_env  # noqa: E402

DEV_KEY_VAR = "TRAILTALK_DEV_KEY"
HEX_KEY = re.compile(r"^[0-9a-fA-F]{32}$")

build_type = env.GetProjectOption("build_type", "debug")  # noqa: F821
pio_env = env["PIOENV"]  # noqa: F821
is_release = pio_env == "release" or build_type == "release"

if is_release:
    # Either name refuses. renamed_env lets the new name win, which is right for
    # choosing a key and wrong for this question: an empty TRAILTALK_DEV_KEY next
    # to a set MAXL_DEV_KEY is still a development key in the environment.
    set_vars = [v for v in (DEV_KEY_VAR, renamed_env.old_name(DEV_KEY_VAR))
                if os.environ.get(v, "").strip()]
    if set_vars:
        print("")
        print("=" * 74)
        print("  RELEASE BUILD REFUSED")
        print("")
        print("  {} is set in the environment.".format(" and ".join(set_vars)))
        print("")
        print("  CLAUDE.md 2.1 permits a development key behind a compile flag for bench")
        print("  work in phases 2-3, and requires that flag to fail the build in release")
        print("  configuration. This is that requirement.")
        print("")
        print("  Unset it and build again:   unset {}".format(" ".join(set_vars)))
        print("=" * 74)
        print("")
        sys.exit(1)
    print("release_guard: release build, no development key. Good.")
else:
    dev_key = renamed_env.getenv(DEV_KEY_VAR)
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
            CPPDEFINES=[("TRAILTALK_DEV_KEY", env.StringifyMacro(dev_key.lower()))]  # noqa: F821
        )
        print("release_guard: development key taken from the environment (debug build only)")
    else:
        print("release_guard: debug build, no development key set")
