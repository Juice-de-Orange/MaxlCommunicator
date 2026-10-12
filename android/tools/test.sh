#!/usr/bin/env bash
#
# Build and test the Kotlin protocol module in a container.
#
# Same reasoning as firmware/tools/hosttest.sh: this machine has a JRE and no
# JDK, and installing one needs root. Docker does not. The image already carries
# Gradle, a JDK and python3 -- the last one because the shared test vectors are
# generated from test-vectors/*.json at build time, so all three implementations
# check themselves against the same bytes.
#
# Usage:
#   android/tools/test.sh                   build and test :protocol
#   android/tools/test.sh :app:assembleDebug  build the Android module
#   android/tools/test.sh clean             wipe the build directory
#
# The task is passed through verbatim, so anything Gradle understands works.
set -euo pipefail

# MAXL_GRADLE_IMAGE and MAXL_GRADLE_CACHE are the names from before the rename to
# trailtalk: still read, with a warning, and the new names win.
for old in MAXL_GRADLE_IMAGE MAXL_GRADLE_CACHE; do
    new="TRAILTALK_${old#MAXL_}"
    if [ -n "${!old:-}" ]; then
        if [ -n "${!new:-}" ]; then
            echo "warning: $old is set and ignored -- $new wins; unset it" >&2
        else
            echo "warning: $old is the pre-rename name; rename it to $new" >&2
        fi
    fi
done
IMAGE="${TRAILTALK_GRADLE_IMAGE:-${MAXL_GRADLE_IMAGE:-gradle:8.5-jdk21}}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TASK="${1:-:protocol:test}"

# A cache outside the repo, so a clean checkout does not re-download Gradle's
# world and a `git status` is not full of it.
CACHE="${TRAILTALK_GRADLE_CACHE:-${MAXL_GRADLE_CACHE:-$HOME/.cache/trailtalk-gradle}}"
mkdir -p "$CACHE"

# The Android SDK, mounted read-only from the host. :protocol does not need it;
# :app cannot be configured without it, and downloading a second copy into the
# container would be a gigabyte for something already on disk.
SDK="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
SDK_MOUNT=()
if [ -d "$SDK" ]; then
    SDK_MOUNT=(-v "${SDK}:/opt/android-sdk:ro" -e ANDROID_SDK_ROOT=/opt/android-sdk \
               -e ANDROID_HOME=/opt/android-sdk)
fi

exec docker run --rm \
    -v "${REPO_ROOT}:/repo" \
    -v "${CACHE}:/home/gradle/.gradle" \
    "${SDK_MOUNT[@]}" \
    -w /repo/android \
    -u "$(id -u):$(id -g)" \
    -e GRADLE_USER_HOME=/home/gradle/.gradle \
    "${IMAGE}" \
    gradle --no-daemon --console=plain "${TASK}"
