#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/versions.env"

# Kit directory and CPU count are both spelled per-platform; install_qt6.sh lays
# the kit down under the same name.
case "$(uname -s)" in
  Darwin) QT_KIT=macos;  NPROC="$(sysctl -n hw.ncpu)" ;;
  *)      QT_KIT=gcc_64; NPROC="$(nproc)" ;;
esac

QT_DIR="${SCRIPT_DIR}/.qt/${PJ_QT_VERSION}/${QT_KIT}"

# `./build.sh --tsan` builds + runs the Qt-free foundation concurrency tests under
# ThreadSanitizer in a separate build-tsan/ tree (the default build/ is untouched).
# It guards the datastore worker-thread race regressions; the Linux CI `tsan` job
# runs the same target set. TSan is wired via -DPJ_ENABLE_TSAN=ON on a normal
# RelWithDebInfo configure, so the Conan dependency closure is reused as-is (no
# Debug rebuild) and only our own sources are instrumented.
TSAN=0
SKIP_CONAN_INSTALL=0
for arg in "$@"; do
  case "$arg" in
    --tsan) TSAN=1 ;;
    --skip-conan-install) SKIP_CONAN_INSTALL=1 ;;
    *) echo "unknown argument: $arg (supported: --tsan, --skip-conan-install)" >&2; exit 2 ;;
  esac
done

if [[ ! -d "$QT_DIR" ]]; then
  echo "Qt ${PJ_QT_VERSION} not found at ${QT_DIR}."
  echo "Install it with: ./install_qt6.sh"
  exit 1
fi

CMAKE_CCACHE_ARGS=()
if command -v ccache &>/dev/null; then
  CMAKE_CCACHE_ARGS+=("-DCMAKE_C_COMPILER_LAUNCHER=ccache" "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
fi

# Release/packaging pipelines export these to skip building the test suite
# and the scene3D dev demos (neither ships, and no release flow runs ctest).
# PJ_BUILD_RASTER_HELPER goes the other way: the Linux release turns the
# standalone GPLv2 helper ON so appimage/build_appimage.sh can stage it.
PJ_FLAG_ARGS=()
[[ -n "${PJ_BUILD_TESTS:-}" ]] && PJ_FLAG_ARGS+=("-DPJ_BUILD_TESTS=${PJ_BUILD_TESTS}")
[[ -n "${PJ_BUILD_DEMOS:-}" ]] && PJ_FLAG_ARGS+=("-DPJ_BUILD_DEMOS=${PJ_BUILD_DEMOS}")
[[ -n "${PJ_BUILD_RASTER_HELPER:-}" ]] && PJ_FLAG_ARGS+=("-DPJ_BUILD_RASTER_HELPER=${PJ_BUILD_RASTER_HELPER}")

# CI can opt into PlotJuggler's authenticated Artifactory remote while local
# builds and untrusted pull requests remain reproducible against ConanCenter.
# Keep the remotes explicit so unrelated developer remotes can never shadow the
# stock recipes. Artifactory is a binary/recipe cache; ConanCenter remains the
# fallback for anything that has not been mirrored yet.
CONAN_REMOTE_ARGS=(-r conancenter)
if [[ "${PJ_USE_JFROG:-false}" == "true" ]]; then
  CONAN_REMOTE_ARGS=(-r plotjuggler-conan -r conancenter)
fi

# Committed lockfile pins every recipe revision so local and CI builds resolve
# the exact graph JFrog holds binaries for (rebuilds happen only when the lock
# moves). --lockfile-partial keeps platform-only additions resolvable.
CONAN_LOCKFILE_ARGS=()
if [[ -f "${SCRIPT_DIR}/conan.lock" ]]; then
  CONAN_LOCKFILE_ARGS=(--lockfile="${SCRIPT_DIR}/conan.lock" --lockfile-partial)
fi

# Foundation concurrency tests exercised under ThreadSanitizer. Keep in sync with
# the `tsan` job in .github/workflows/linux-ci.yml.
TSAN_TESTS=(engine_thread_safety_test engine_concurrency_test)

if [[ "$TSAN" == "1" ]]; then
  BUILD_DIR="${SCRIPT_DIR}/build-tsan"

  if [[ "$SKIP_CONAN_INSTALL" == "0" ]]; then
    conan install "$SCRIPT_DIR" --output-folder="$BUILD_DIR" --build=missing "${CONAN_LOCKFILE_ARGS[@]}" \
      -s build_type=RelWithDebInfo -s compiler.cppstd=20 "${CONAN_REMOTE_ARGS[@]}"
  elif [[ ! -f "$BUILD_DIR/conan_toolchain.cmake" ]]; then
    echo "Missing $BUILD_DIR/conan_toolchain.cmake; run Conan install first." >&2
    exit 1
  fi

  # PJ4_BUILD_APP=OFF + building only the foundation test targets keeps Qt out of
  # the picture entirely (no Qt code is compiled), even though configure still
  # finds Qt for the modules it won't build.
  cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/conan_toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="${QT_DIR}" \
    -DPJ_ENABLE_TSAN=ON \
    -DPJ4_BUILD_APP=OFF \
    "${CMAKE_CCACHE_ARGS[@]+"${CMAKE_CCACHE_ARGS[@]}"}" "${PJ_FLAG_ARGS[@]+"${PJ_FLAG_ARGS[@]}"}"

  cmake --build "$BUILD_DIR" --target "${TSAN_TESTS[@]}" -j "${NPROC}"

  # Run under ctest so the per-test CMake TIMEOUT catches a deadlock regression,
  # and so TSan's non-zero exit (it dies on the first report under halt_on_error)
  # fails the test. ^(...)$ restricts the run to the targets we actually built.
  filter="$(IFS='|'; echo "${TSAN_TESTS[*]}")"
  TSAN_OPTIONS="halt_on_error=1 history_size=4 ${TSAN_OPTIONS:-}" \
    ctest --test-dir "$BUILD_DIR" -R "^(${filter})$" --output-on-failure --timeout 120
  exit 0
fi

BUILD_DIR="${SCRIPT_DIR}/build"

# Pin resolution to the explicit remote list selected above. A developer machine
# may have unrelated private remotes that host forked recipes under a user
# channel; those must never shadow PJ4's intended dependency graph.
if [[ "$SKIP_CONAN_INSTALL" == "0" ]]; then
  conan install "$SCRIPT_DIR" --output-folder="$BUILD_DIR" --build=missing "${CONAN_LOCKFILE_ARGS[@]}" \
    -s build_type=RelWithDebInfo -s compiler.cppstd=20 "${CONAN_REMOTE_ARGS[@]}"
elif [[ ! -f "$BUILD_DIR/conan_toolchain.cmake" ]]; then
  echo "Missing $BUILD_DIR/conan_toolchain.cmake; run Conan install first." >&2
  exit 1
fi

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="${QT_DIR}" \
  -DPJ_VERSION="${PJ_VERSION:-${PJ_APP_VERSION}}" \
  -DPJ_INSTALLATION="${PJ_INSTALLATION:-source}" \
  "${CMAKE_CCACHE_ARGS[@]+"${CMAKE_CCACHE_ARGS[@]}"}" "${PJ_FLAG_ARGS[@]+"${PJ_FLAG_ARGS[@]}"}"

# Surface the compile DB (CMAKE_EXPORT_COMPILE_COMMANDS writes it under build/) at
# the repo root so clangd/editors resolve includes without extra config. The root
# path is gitignored; the relative target survives a worktree move.
ln -sf "build/compile_commands.json" "$SCRIPT_DIR/compile_commands.json"

cmake --build "$BUILD_DIR" -j "${NPROC}"
