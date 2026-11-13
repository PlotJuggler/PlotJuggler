#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# rattler-build's build script for recipe.yaml (runs inside the host env).
# Same build type and debug-info level as the AppImage and .deb. Builds the
# app closure only and installs the two binaries the app needs side
# by side; Qt, Python and every library come from the environment.
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
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="${PREFIX}" \
  -DPJ_SYSTEM_QWT=ON \
  -DPJ_BUILD_TESTS=OFF \
  -DPJ_BUILD_DEMOS=OFF \
  -DPJ_DEBUG_INFO=none \
  -DPJ_VERSION="${PKG_VERSION}" \
  -DPJ_INSTALLATION=conda \
  -DPJ_QT_MIN_VERSION="${PJ_QT_MIN_VERSION}"
cmake --build build-conda --target pj_app
if [[ -n "${CCACHE_DIR:-}" ]]; then ccache --show-stats; fi

# No `install -D`: BSD install on macOS lacks it.
mkdir -p "${PREFIX}/bin"
install -m755 build-conda/pj_app/plotjuggler4 build-conda/pj_app/pj-plugin-check "${PREFIX}/bin/"
