#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/versions.env"

QT_DIR="${SCRIPT_DIR}/.qt/${PJ_QT_VERSION}/gcc_64"

usage() {
  printf '%s\n' \
    "Usage: ./build.sh [OPTIONS]" \
    "" \
    "Options:" \
    "  --tsan               Build and run concurrency tests with ThreadSanitizer" \
    "  --skip-test          Build without the test suite" \
    "  --skip-conan-install Reuse the existing Conan toolchain" \
    "  --sdk-local[=PATH]   Build plotjuggler_sdk from a local tree instead of the" \
    "                       Conan package (default PATH: ../plotjuggler_sdk sibling)." \
    "                       Dev-only: not reproducible, refused in CI." \
    "  --help               Show this help message"
}

# `./build.sh --tsan` builds + runs the Qt-free foundation concurrency tests under
# ThreadSanitizer in a separate build-tsan/ tree (the default build/ is untouched).
# It guards the datastore worker-thread race regressions; the Linux CI `tsan` job
# runs the same target set. TSan is wired via -DPJ_ENABLE_TSAN=ON on a normal
# RelWithDebInfo configure, so the Conan dependency closure is reused as-is (no
# Debug rebuild) and only our own sources are instrumented.
TSAN=0
SKIP_TEST=0
SKIP_CONAN_INSTALL=0
SDK_LOCAL_DIR=""
for arg in "$@"; do
  case "$arg" in
    --tsan) TSAN=1 ;;
    --skip-test) SKIP_TEST=1 ;;
    --skip-conan-install) SKIP_CONAN_INSTALL=1 ;;
    --sdk-local) SDK_LOCAL_DIR="${SCRIPT_DIR}/../plotjuggler_sdk"
                 [[ -d "$SDK_LOCAL_DIR" ]] || SDK_LOCAL_DIR="${HOME}/ws_plotjuggler/plotjuggler_sdk" ;;
    --sdk-local=*) SDK_LOCAL_DIR="${arg#--sdk-local=}" ;;
    --help) usage; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -n "$SDK_LOCAL_DIR" ]]; then
  if [[ -n "${CI:-}" ]]; then
    echo "--sdk-local is a local development mode; refusing to run in CI" >&2
    exit 2
  fi
  if [[ ! -f "$SDK_LOCAL_DIR/pj_base/CMakeLists.txt" ]]; then
    echo "--sdk-local: no plotjuggler_sdk tree at $SDK_LOCAL_DIR" >&2
    exit 2
  fi
  echo "=================================================================="
  echo " plotjuggler_sdk from LOCAL TREE: $SDK_LOCAL_DIR"
  echo " NOT reproducible — do not use for release artifacts"
  echo "=================================================================="
fi

if [[ "$TSAN" == "1" && "$SKIP_TEST" == "1" ]]; then
  echo "--tsan and --skip-test cannot be used together" >&2
  exit 2
fi

if [[ ! -d "$QT_DIR" ]]; then
  echo "Qt ${PJ_QT_VERSION} not found at ${QT_DIR}."
  echo "Install it with: ./scripts/install_qt6.sh"
  exit 1
fi

CMAKE_CCACHE_ARGS=()
if command -v ccache &>/dev/null; then
  CMAKE_CCACHE_ARGS+=("-DCMAKE_C_COMPILER_LAUNCHER=ccache" "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
fi

# Release/packaging pipelines export these to skip building the test suite
# and the scene3D dev demos (neither ships, and no release flow runs ctest).
# PJ_BUILD_RASTER_HELPER goes the other way: the Linux release turns the
# standalone GPLv2 helper ON so packaging/appimage/build_appimage.sh can stage it.
PJ_FLAG_ARGS=()
if [[ "$SKIP_TEST" == "1" ]]; then
  PJ_FLAG_ARGS+=("-DPJ_BUILD_TESTS=OFF")
elif [[ -n "${PJ_BUILD_TESTS:-}" ]]; then
  PJ_FLAG_ARGS+=("-DPJ_BUILD_TESTS=${PJ_BUILD_TESTS}")
fi
[[ -n "${PJ_BUILD_DEMOS:-}" ]] && PJ_FLAG_ARGS+=("-DPJ_BUILD_DEMOS=${PJ_BUILD_DEMOS}")
[[ -n "${PJ_BUILD_RASTER_HELPER:-}" ]] && PJ_FLAG_ARGS+=("-DPJ_BUILD_RASTER_HELPER=${PJ_BUILD_RASTER_HELPER}")

# plotjuggler_sdk is served only by PlotJuggler's Artifactory remote (anonymous
# read); ConanCenter supplies everything else. Keep the remotes explicit so
# unrelated developer remotes can never shadow the stock recipes.
"${SCRIPT_DIR}/scripts/configure_conan_remote.sh"
CONAN_REMOTE_ARGS=(-r plotjuggler-conan -r conancenter)

# Committed lockfile pins every recipe revision so local and CI builds resolve
# the exact graph JFrog holds binaries for (rebuilds happen only when the lock
# moves). --lockfile-partial keeps platform-only additions resolvable.
CONAN_LOCKFILE_ARGS=()
if [[ -f "${SCRIPT_DIR}/conan.lock" ]]; then
  CONAN_LOCKFILE_ARGS=(--lockfile="${SCRIPT_DIR}/conan.lock" --lockfile-partial)
fi

# Foundation concurrency tests exercised under ThreadSanitizer. Keep in sync with
# the `tsan` job in .github/workflows/linux-ci.yml.
TSAN_TESTS=(engine_thread_safety_test engine_concurrency_test recorder_test)

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

  cmake --build "$BUILD_DIR" --target "${TSAN_TESTS[@]}" -j "$(nproc)"

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
  -DPJ_SDK_LOCAL_DIR="${SDK_LOCAL_DIR}" \
  "${CMAKE_CCACHE_ARGS[@]+"${CMAKE_CCACHE_ARGS[@]}"}" "${PJ_FLAG_ARGS[@]+"${PJ_FLAG_ARGS[@]}"}"

# Surface the compile DB (CMAKE_EXPORT_COMPILE_COMMANDS writes it under build/) at
# the repo root so clangd/editors resolve includes without extra config. The root
# path is gitignored; the relative target survives a worktree move.
ln -sf "build/compile_commands.json" "$SCRIPT_DIR/compile_commands.json"

cmake --build "$BUILD_DIR" -j "$(nproc)"
