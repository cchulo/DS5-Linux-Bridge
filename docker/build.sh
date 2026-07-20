#!/usr/bin/env bash
# Build the firmware inside the pinned Docker toolchain (no host toolchain needed).
#
#   ./docker/build.sh [pico2_w|pico_w|waveshare] [extra cmake args...]
#
# Output: build/docker-<variant>/ds5-bridge.uf2
#
# BUILD_NAME=<name> keeps a differently-flagged configuration in its own
# build/docker-<name> directory instead of re-configuring the variant's
# default one, e.g. a single-slot flavor of the pico2_w board:
#
#   BUILD_NAME=ms1 ./docker/build.sh pico2_w -DMULTI_SLOT_COUNT=1
#
# The CMake cache remembers the -D flags, so re-running with the same
# BUILD_NAME (even without flags) rebuilds that configuration incrementally.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE=ds5-bridge-builder
VARIANT="${1:-pico2_w}"
shift || true

case "${VARIANT}" in
    pico2_w)   CMAKE_FLAGS="" ;;
    pico_w)    CMAKE_FLAGS="-DPICO_W_BUILD=ON" ;;
    waveshare) CMAKE_FLAGS="-DWAVESHARE_RP2350B_PLUS_W_BUILD=ON" ;;
    *) echo "unknown variant '${VARIANT}' (expected pico2_w|pico_w|waveshare)" >&2; exit 1 ;;
esac

if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
    echo ">> building ${IMAGE} image (one-time, downloads toolchain + SDK)"
    docker build -t "${IMAGE}" "${REPO_DIR}/docker"
fi

BUILD_DIR="build/docker-${BUILD_NAME:-${VARIANT}}"
docker run --rm -v "${REPO_DIR}:/work" -w /work "${IMAGE}" bash -c "
    set -euo pipefail
    cmake -S . -B '${BUILD_DIR}' -G Ninja -DCMAKE_BUILD_TYPE=Release ${CMAKE_FLAGS} $*
    cmake --build '${BUILD_DIR}'
"
STAMPED="$(ls "${REPO_DIR}/${BUILD_DIR}"/ds5-bridge-2*.uf2 2>/dev/null | head -1 || true)"
echo ">> done: ${BUILD_DIR}/ds5-bridge.uf2"
if [ -n "${STAMPED}" ]; then
    echo ">> stamped copy: ${BUILD_DIR}/$(basename "${STAMPED}")  (same file; name = build minute, matches the version shown in the web UI)"
fi
