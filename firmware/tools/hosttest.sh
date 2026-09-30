#!/usr/bin/env bash
#
# Run the host test suite in a container.
#
# Not every machine this project is developed on has a host C++ compiler, and
# installing one needs root. Docker does not. See docs/decisions D8.
#
# The image needs nothing but make, g++ and python3, which the stock gcc image
# already has -- so there is no Dockerfile to build and nothing to keep in sync.
#
# Usage:
#   firmware/tools/hosttest.sh          build and run everything
#   firmware/tools/hosttest.sh clean    remove the build directory
set -euo pipefail

IMAGE="${MAXL_TEST_IMAGE:-gcc:14}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TARGET="${1:-test}"

exec docker run --rm \
    -v "${REPO_ROOT}:/repo" \
    -w /repo/firmware/test \
    -u "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    "${IMAGE}" \
    make -j "$(nproc)" "${TARGET}"
