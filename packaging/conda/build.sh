#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# rattler-build's build script for recipe.yaml (runs inside the host env).
# Same build type and debug-info level as the AppImage and .deb. A plain
# configure/build/install: the app's install rules put plotjuggler4 and
# pj-plugin-check side by side in bin/ (and the desktop entry and icons on
# Linux); Qt, Python and every library come from the environment.
set -euo pipefail

cmake -G Ninja -S . -B build-conda ${CMAKE_ARGS} \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="${PREFIX}" \
  -DPJ_SYSTEM_QWT=ON \
  -DPJ_BUILD_TESTS=OFF \
  -DPJ_BUILD_DEMOS=OFF \
  -DPJ_DEBUG_INFO=none \
  -DPJ_VERSION="${PKG_VERSION}" \
  -DPJ_INSTALLATION=conda \
  -DPJ_QT_MIN_VERSION="${PJ_QT_MIN_VERSION}"
cmake --build build-conda
cmake --install build-conda
