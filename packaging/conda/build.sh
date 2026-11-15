#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# rattler-build's build script for recipe.yaml (runs inside the host env).
# Same build type and debug-info level as the AppImage and .deb. A plain
# configure/build/install: the app's install rules put plotjuggler4 and
# pj-plugin-check side by side in bin/ (and the desktop entry and icons on
# Linux); Qt, Python and every library come from the environment.
#
# Not FETCHCONTENT_FULLY_DISCONNECTED: plotjuggler_sdk (the pinned release) and
# pocketfft have no package on conda-forge or the plotjuggler channel yet, so the
# configure step still downloads both, pinned by SHA-256.
set -euo pipefail

# Compiler cache, when the caller sets CCACHE_DIR. Every build runs in a new
# directory with its own copy of the compiler, so paths are hashed relative to
# that directory and the compiler by content; otherwise nothing would ever hit.
LAUNCHER_ARGS=""
if [[ -n "${CCACHE_DIR:-}" ]]; then
  export CCACHE_BASEDIR="$(dirname "${SRC_DIR}")"
  export CCACHE_COMPILERCHECK=content
  export CCACHE_MAXSIZE=1G
  LAUNCHER_ARGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
  ccache --zero-stats
fi

cmake -G Ninja -S . -B build-conda ${CMAKE_ARGS} ${LAUNCHER_ARGS} \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="${PREFIX}" \
  -DPJ_SYSTEM_QWT=ON \
  -DPJ_SYSTEM_ADS=ON \
  -DPJ_BUILD_TESTS=OFF \
  -DPJ_BUILD_DEMOS=OFF \
  -DPJ_DEBUG_INFO=none \
  -DPJ_VERSION="${PKG_VERSION}" \
  -DPJ_INSTALLATION=conda \
  -DPJ_QT_MIN_VERSION="${PJ_QT_MIN_VERSION}"
cmake --build build-conda
if [[ -n "${CCACHE_DIR:-}" ]]; then ccache --show-stats; fi
cmake --install build-conda
