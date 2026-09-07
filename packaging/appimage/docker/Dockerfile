# syntax=docker/dockerfile:1
# SPDX-License-Identifier: MPL-2.0
# Ubuntu 22.04 build environment for the PJ4 AppImage — pins the build host's
# glibc/libstdc++ baseline to the one .github/workflows/release.yml already
# publishes releases from (runs-on: ubuntu-22.04), independent of whatever the
# developer's own machine runs. glibc symbol versioning is forward-compatible
# only: a binary linked against a newer glibc refuses to run against an older
# one, so building on a bleeding-edge host silently produces an AppImage that
# breaks on the mainstream distros AppImages are supposed to be portable to.
#
# Qt itself is not rebuilt here: aqtinstall ships prebuilt binaries linked
# against an intentionally old/portable glibc baseline (that's the point of an
# official Qt distribution), so a host's existing ./.qt is fine to reuse as-is
# inside this container — only PJ4's own sources and the Conan dependency
# closure are toolchain-sensitive, and those are what this image builds fresh.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Base toolchain + every package linux-ci.yml / release.yml installs on top of
# the ubuntu-22.04 GitHub runner (which ships most of this preinstalled; a bare
# ubuntu:22.04 image does not).
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git python3 python3-pip pkg-config ca-certificates \
    libgl1-mesa-dev libegl1-mesa-dev libxkbcommon-dev \
    libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
    libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 \
    libxcb-xkb1 libxkbcommon-x11-0 libfontconfig1 libdbus-1-3 \
    libva-dev libdrm-dev \
    imagemagick wget file unzip \
    ccache zstd \
  && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir 'conan>=2' 'aqtinstall>=3.3'

# The repo is bind-mounted from the host and owned by the host UID; newer git
# refuses to operate on a working tree it doesn't own ("dubious ownership").
RUN git config --system --add safe.directory '*'

# Writable, UID-independent HOME: build_appimage_in_docker.sh runs the container with
# --user "$(id -u):$(id -g)" (so build output lands on the host owned by the
# invoking user, not root), which means there is no /etc/passwd entry for that
# UID inside the container. Tools that need a resolvable $HOME (pip, conan,
# ccache) get one anyway via this fixed, world-writable directory.
RUN mkdir -p /home/build && chmod 1777 /home/build
ENV HOME=/home/build
