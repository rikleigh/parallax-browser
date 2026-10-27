# Build environment for pacbrew-repo's Mesa 26.2 PS5 port (RADV + zink + EGL
# "videoout"). Mirrors pacbrew-repo/.github/workflows/ps5-payload-libs.yml,
# which runs on ubuntu-latest (24.04) -- same LLVM 18 / libclc 18 /
# SPIRV-LLVM-Translator 18 packages the host mesa_clc build expects.
# Built natively as linux/arm64 on Apple Silicon (no emulation): the PS5 SDK
# is a set of clang wrappers, so the host CPU does not matter.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl wget git xz-utils bzip2 file patch \
        build-essential cmake ninja-build pkg-config pkgconf \
        clang-18 lld-18 llvm-18 llvm-18-dev libclang-18-dev libclang-cpp18-dev \
        libclc-18-dev libllvmspirvlib-18-dev \
        spirv-tools glslang-tools \
        bison flex gperf autoconf libtool \
        python3 python3-pip python3-mako python3-yaml python3-packaging \
        python3-pyelftools \
        zlib1g-dev libarchive-tools \
        makepkg pacman-package-manager \
        socat \
    && python3 -m pip install --break-system-packages --no-cache-dir 'meson>=1.4.0' \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# makepkg refuses to run as root.
RUN useradd -m -u 1000 -s /bin/bash builder || useradd -m -s /bin/bash builder
ENV LLVM_CONFIG=/usr/bin/llvm-config-18
USER builder
WORKDIR /home/builder
