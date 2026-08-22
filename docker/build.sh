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
# Every project option is UNSET in the cache before this invocation's flags
# are applied, so a build is fully determined by its command line. Without
# this, a one-off "-DENABLE_WIFI_WOL=OFF" test build silently stuck in the
# cache and every later default build shipped without WiFi/wake (2026-08-22).
RESET_OPTS="-UENABLE_WIFI_WOL -UENABLE_WAKE_HID -UENABLE_LED_STRIP -ULED_STRIP_GPIO \
 -UENABLE_POWER_PIN -UPOWER_PIN_GPIO -UMULTI_SLOT_COUNT -UENABLE_VERBOSE \
 -UENABLE_BATT_LED -UDISABLE_SPEAKER_PROC -UPICO_W_BUILD -UWAVESHARE_RP2350B_PLUS_W_BUILD \
 -UVERSION"
docker run --rm -v "${REPO_DIR}:/work" -w /work "${IMAGE}" bash -c "
    set -euo pipefail
    cmake -S . -B '${BUILD_DIR}' -G Ninja -DCMAKE_BUILD_TYPE=Release ${RESET_OPTS} ${CMAKE_FLAGS} $*
    cmake --build '${BUILD_DIR}'
"
echo ">> options: $(grep -E '^(ENABLE_WIFI_WOL|ENABLE_WAKE_HID|ENABLE_LED_STRIP|MULTI_SLOT_COUNT):' "${REPO_DIR}/${BUILD_DIR}/CMakeCache.txt" | sed 's/:[A-Z]*=/=/' | tr '\n' ' ')"
STAMPED="$(ls "${REPO_DIR}/${BUILD_DIR}"/ds5-bridge-[0-9]*.uf2 2>/dev/null | head -1 || true)"
echo ">> done: ${BUILD_DIR}/ds5-bridge.uf2"
if [ -n "${STAMPED}" ]; then
    echo ">> stamped copy: ${BUILD_DIR}/$(basename "${STAMPED}")  (same file; name = build minute, matches the version shown in the web UI)"
fi
