# Build environment for DS5-Linux-Bridge firmware.
# Pins match the versions recorded in CMakeLists.txt: Pico SDK 2.2.0,
# ARM GNU toolchain 14.2.Rel1, picotool 2.2.0. Multi-arch (amd64/arm64).
#
# Usage: ./docker/build.sh [pico2_w|pico_w|waveshare]

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl xz-utils git \
        cmake ninja-build build-essential \
        python3 pkg-config libusb-1.0-0-dev \
    && rm -rf /var/lib/apt/lists/*

# ARM GNU toolchain 14.2.Rel1 (the version pinned in CMakeLists.txt)
ARG TARGETARCH
RUN set -eux; \
    case "${TARGETARCH}" in \
        amd64) ARCH=x86_64 ;; \
        arm64) ARCH=aarch64 ;; \
        *) echo "unsupported arch: ${TARGETARCH}" >&2; exit 1 ;; \
    esac; \
    mkdir -p /opt/toolchain; \
    curl -fsSL "https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-${ARCH}-arm-none-eabi.tar.xz" \
        | tar -xJ -C /opt/toolchain --strip-components=1
ENV PICO_TOOLCHAIN_PATH=/opt/toolchain
ENV PATH="/opt/toolchain/bin:${PATH}"

# Pico SDK 2.2.0 with its bundled stacks (BTstack, TinyUSB, cyw43-driver, lwIP)
RUN git clone --depth 1 --branch 2.2.0 https://github.com/raspberrypi/pico-sdk.git /opt/pico-sdk \
    && git -C /opt/pico-sdk submodule update --init --recursive
ENV PICO_SDK_PATH=/opt/pico-sdk

# Pin TinyUSB to the same post-0.20.0 commit as .github/workflows/build.yml
# (TINYUSB_REF): the SDK 2.2.0 submodule ships 0.18.0, whose audio driver and
# usbd control-transfer handling do not match this firmware's tusb_config.h.
ARG TINYUSB_REF=08f9855e3dc51291a8c30e2eeccab31f8f07d0a9
RUN git -C /opt/pico-sdk/lib/tinyusb fetch --depth 1 origin "${TINYUSB_REF}" \
    && git -C /opt/pico-sdk/lib/tinyusb checkout --detach "${TINYUSB_REF}"

# picotool 2.2.0, preinstalled so the SDK does not re-fetch it per build dir
RUN git clone --depth 1 --branch 2.2.0 https://github.com/raspberrypi/picotool.git /tmp/picotool \
    && cmake -S /tmp/picotool -B /tmp/picotool/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /tmp/picotool/build \
    && cmake --install /tmp/picotool/build \
    && rm -rf /tmp/picotool

WORKDIR /work
