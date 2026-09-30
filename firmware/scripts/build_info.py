"""Stamp version and git revision into the firmware image.

docs/versioning-and-updates.md section 3 requires the version and git hash to be
baked into the image and reported by GET_INFO and the STATUS screen. This runs as
a PlatformIO pre-hook and exposes them as preprocessor defines.

The commit timestamp is used rather than the wall clock so that rebuilding the
same commit produces the same image.
"""

import os
import subprocess

Import("env")  # noqa: F821  (injected by PlatformIO)


def _git(*args, default=""):
    try:
        return subprocess.check_output(
            ("git",) + args,
            cwd=env["PROJECT_DIR"],  # noqa: F821
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return default


project_dir = env["PROJECT_DIR"]  # noqa: F821

version_file = os.path.join(project_dir, "version.txt")
try:
    with open(version_file, encoding="utf-8") as handle:
        version = handle.read().strip()
except OSError:
    version = "0.0.0"

sha = _git("rev-parse", "--short=7", "HEAD", default="nogit")
dirty = 1 if _git("status", "--porcelain") else 0
epoch = _git("log", "-1", "--format=%ct", default="0")

version_full = "{}+g{}{}".format(version, sha, "-dirty" if dirty else "")

env.Append(  # noqa: F821
    CPPDEFINES=[
        ("FW_VERSION", env.StringifyMacro(version)),  # noqa: F821
        ("FW_GIT_HASH", env.StringifyMacro(sha)),  # noqa: F821
        ("FW_VERSION_FULL", env.StringifyMacro(version_full)),  # noqa: F821
        ("FW_GIT_DIRTY", dirty),
        ("FW_COMMIT_EPOCH", epoch + "UL"),
    ]
)

env.Replace(PROGNAME="maxl-{}-{}-g{}".format(env["PIOENV"], version, sha))  # noqa: F821

print("build_info: {}".format(version_full))
