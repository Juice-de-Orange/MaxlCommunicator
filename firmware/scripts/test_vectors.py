"""Generate the shared test vectors for the device's boot self test.

docs/test-plan.md gate 2.15: "Known-answer test -- fixed key, nonce, plaintext ->
expected ciphertext and tag. Runs on device at boot in debug builds; matches
published AES-CCM test vectors."

The vectors live in test-vectors/*.json and are read by three things now: the
TypeScript bridge (as JSON), the host test build (as a generated header), and --
from here -- the debug firmware image. One source, so a vector that is wrong is
wrong everywhere at once rather than in one place quietly.

Only two groups are emitted. RFC 3610 is what "published AES-CCM test vectors"
means, and frame_crypto pins this project's own construction around the cipher
-- the 13-byte nonce layout, the 12-byte header as AAD, the tag truncated to
four bytes -- which RFC 3610 does not cover because it has no 4-byte-tag
vectors. Carrying the bridge and radio payload vectors into a device image would
be kilobytes of flash for something the device never checks.

Debug builds only. The release guard would be entitled to complain about test
data in a shipping image, and CLAUDE.md 6 compiles serial output out there
anyway, so there would be nowhere to report the result.
"""

import os
import subprocess
import sys

Import("env")  # noqa: F821  (injected by PlatformIO)

project_dir = env["PROJECT_DIR"]  # noqa: F821
build_dir = env.subst("$BUILD_DIR")  # noqa: F821
repo_root = os.path.dirname(project_dir)

generated_dir = os.path.join(build_dir, "generated")
header = os.path.join(generated_dir, "test_vectors.h")

# The same test scripts/release_guard.py makes, for the same reason: a pre-hook
# runs before the environment's build_flags have all been folded in, so reading
# CPPDEFINES here answers about the wrong moment. The environment's name and its
# build_type are settled before any of that.
build_type = env.GetProjectOption("build_type", "debug")  # noqa: F821
pio_env = env["PIOENV"]  # noqa: F821
is_debug = not (pio_env == "release" or build_type == "release")

if is_debug:
    result = subprocess.run(
        [
            sys.executable,
            os.path.join(repo_root, "tools", "gen_vectors.py"),
            "--out",
            header,
            "--group",
            "aes_ccm_rfc3610",
            "--group",
            "frame_crypto",
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit("test_vectors: generation failed")

    env.Append(CPPPATH=[generated_dir])  # noqa: F821
    env.Append(CPPDEFINES=[("MAXL_SELF_TEST", 1)])  # noqa: F821
    print("test_vectors: {}".format(result.stdout.strip()))
else:
    print("test_vectors: release build, no vectors compiled in")
