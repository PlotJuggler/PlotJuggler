#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# rattler-build's build script for recipe.yaml (runs inside the host env).
# Same build type and debug-info level as the AppImage and .deb. Builds the
# app closure only and installs the two binaries the app needs side
# by side; Qt, Python and every library come from the environment.
set -euo pipefail

cmake -G Ninja -S . -B build-conda ${CMAKE_ARGS} \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="${PREFIX}" \
  -DPJ_SYSTEM_QWT=ON \
  -DPJ_BUILD_TESTS=OFF \
  -DPJ_BUILD_DEMOS=OFF \
  -DPJ_DEBUG_INFO=none \
  -DPJ_VERSION="${PKG_VERSION}" \
  -DPJ_INSTALLATION=conda
cmake --build build-conda --target pj_app

# No `install -D`: BSD install on macOS lacks it.
mkdir -p "${PREFIX}/bin"
install -m755 build-conda/pj_app/plotjuggler4 build-conda/pj_app/pj-plugin-check "${PREFIX}/bin/"
