# Cross-compilation toolchain for the native app (ARMv7, arm-webos-linux-gnueabi).
#
# THE ARMv7 TARGET IS STILL CORRECT ON webOS 23, and it is worth writing down
# because it looks wrong. The C3's kernel reports `aarch64` — but the USERLAND is
# 32-bit: /usr/lib/libSDL2, libluna-service2 and libwayland-client are all
# ELFCLASS32 / EM_ARM, and the binary this SDK produces (ELF32, EM_ARM) runs on
# the device at 60 fps. Do not "fix" this to an aarch64 toolchain on the strength
# of `uname -m`; check the libraries the binary actually has to link against.
#
# This recipe exists because the image was assembled by hand on 30/08 and then
# lost: with no versioned Dockerfile there was no way to rebuild the environment,
# and with no ARM binary there is nothing to install on the TV. Do not delete.
#
#   docker build --platform linux/arm64 -t nuvio-webos-sdk tools/
#
# The SDK is openlgtv's buildroot. `relocate-sdk.sh` has to run once after
# extraction: the buildroot records absolute paths from the host it was
# generated on, and without relocating, gcc cannot find its own sysroot.
FROM debian:bookworm-slim

ARG SDK_URL=https://github.com/openlgtv/buildroot-nc4/releases/download/webos-a38c582/arm-webos-linux-gnueabi_sdk-buildroot-aarch64.tar.gz

RUN apt-get update \
  && apt-get install -y --no-install-recommends ca-certificates curl file xz-utils \
  && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL "$SDK_URL" -o /tmp/sdk.tar.gz \
  && mkdir -p /opt \
  && tar -xzf /tmp/sdk.tar.gz -C /opt \
  && rm /tmp/sdk.tar.gz \
  && /opt/arm-webos-linux-gnueabi_sdk-buildroot/relocate-sdk.sh

ENV PATH=/opt/arm-webos-linux-gnueabi_sdk-buildroot/bin:$PATH
ENV NUVIO_SYSROOT=/opt/arm-webos-linux-gnueabi_sdk-buildroot/arm-webos-linux-gnueabi/sysroot

WORKDIR /work
