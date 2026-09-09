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
    "  --minimal            App closure only, no debug info: same as" \
    "                       --no-tests --no-demos --target pj_app --debug-info none" \
    "  --no-tests           Build without the test suite (also turns off the widget" \
    "                       demos and the standalone marketplace app, which follow it)" \
    "  --no-demos           Skip the scene3D demos and benchmarks" \
    "  --no-widget-demos    Skip the pj_widgets demo apps" \
    "  --no-marketplace-app Skip the standalone marketplace executable" \
    "  --target NAME        CMake target to build (default: all; pj_app = app + plugin checker)" \
    "  --debug-info LEVEL   none|lines|full|split (Linux default: split; see cmake/PjDebugInfo.cmake)" \
    "  --no-compress-debug  Keep DWARF uncompressed (default on Linux: zlib-compressed)" \
    "  --tsan               Build and run concurrency tests with ThreadSanitizer" \
    "  --skip-conan-install Reuse the existing Conan toolchain" \
    "  --sdk-local[=PATH]   Build plotjuggler_sdk from a local tree instead of the" \
    "                       Conan package (default PATH: ../plotjuggler_sdk sibling)." \
    "                       Dev-only: not reproducible, refused in CI." \
    "  --help               Show this help message" \
    "" \
    "Examples:" \
    "  ./build.sh                    full dev tree: tests + demos, split DWARF" \
    "  ./build.sh --minimal          what the packaging build compiles, on the host" \
    "  ./build.sh --no-tests --debug-info full" \
    "" \
    "The same settings are accepted as PJ_BUILD_TESTS, PJ_BUILD_DEMOS," \
    "PJ_BUILD_WIDGET_DEMOS, PJ_BUILD_MARKETPLACE_APP, PJ_BUILD_TARGET, PJ_DEBUG_INFO" \
    "and PJ_COMPRESS_DEBUG environment variables (how CI and the container wrapper" \
    "drive this script); an option always wins over the variable."
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
# Options set the same PJ_* variables the environment can provide, so the
# option always wins and everything below reads one source.
# Values are validated where they are assigned, for both `--opt VALUE` and
# `--opt=VALUE`, so a bad value is rejected even when a later option (e.g.
# --minimal) would overwrite it. Repeated valid options: the last one wins.
bad_value() { echo "$1" >&2; usage >&2; exit 2; }
set_target() {
  [[ -n "$1" && "$1" != -* ]] || bad_value "--target needs a target name"
  PJ_BUILD_TARGET="$1"
}
set_debug_info() {
  [[ "$1" =~ ^(none|lines|full|split)$ ]] || bad_value "--debug-info must be none, lines, full or split (got '$1')"
  PJ_DEBUG_INFO="$1"
}
set_sdk_local() {
  [[ -n "$1" ]] || bad_value "--sdk-local= needs a path"
  SDK_LOCAL_DIR="$1"
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --tsan) TSAN=1 ;;
    --no-tests | --skip-test) SKIP_TEST=1 ;;
    --no-demos) PJ_BUILD_DEMOS=OFF ;;
    --no-widget-demos) PJ_BUILD_WIDGET_DEMOS=OFF ;;
    --no-marketplace-app) PJ_BUILD_MARKETPLACE_APP=OFF ;;
    --target) set_target "${2:-}"; shift ;;
    --target=*) set_target "${1#--target=}" ;;
    --debug-info) set_debug_info "${2:-}"; shift ;;
    --debug-info=*) set_debug_info "${1#--debug-info=}" ;;
    --no-compress-debug) PJ_COMPRESS_DEBUG=OFF ;;
    --minimal) SKIP_TEST=1; PJ_BUILD_DEMOS=OFF; PJ_BUILD_TARGET=pj_app; PJ_DEBUG_INFO=none ;;
    --skip-conan-install) SKIP_CONAN_INSTALL=1 ;;
    --sdk-local) SDK_LOCAL_DIR="${SCRIPT_DIR}/../plotjuggler_sdk"
                 [[ -d "$SDK_LOCAL_DIR" ]] || SDK_LOCAL_DIR="${HOME}/ws_plotjuggler/plotjuggler_sdk" ;;
    --sdk-local=*) set_sdk_local "${1#--sdk-local=}" ;;
    -h | --help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done
# The environment may also carry a level (CI, the container wrapper); validate
# it the same way an option is.
if [[ -n "${PJ_DEBUG_INFO:-}" ]]; then
  set_debug_info "${PJ_DEBUG_INFO}"
fi

if [[ -n "$SDK_LOCAL_DIR" ]]; then
  # Existence, not content: the root CMakeLists refuses PJ_SDK_LOCAL_DIR under
  # `if(DEFINED ENV{CI})`, so an exported empty CI must fail here, early, too.
  if [[ -n "${CI+x}" ]]; then
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

if [[ "$TSAN" == "1" ]]; then
  # TSan builds a fixed set of concurrency tests, so it needs the test suite
  # configured ON and does not take a target selection (from the option or the
  # environment); refuse both up front instead of failing after Conan.
  if [[ "$SKIP_TEST" == "1" ]] || { [[ -n "${PJ_BUILD_TESTS:-}" && ! "${PJ_BUILD_TESTS^^}" =~ ^(ON|1|TRUE|YES|Y)$ ]]; }; then
    echo "--tsan needs the test suite: it cannot be combined with --no-tests/--minimal or PJ_BUILD_TESTS=OFF" >&2
    exit 2
  fi
  if [[ -n "${PJ_BUILD_TARGET:-}" && "${PJ_BUILD_TARGET}" != all ]]; then
    echo "--tsan builds its fixed concurrency-test set; --target/PJ_BUILD_TARGET is not supported with it" >&2
    exit 2
  fi
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

# The PJ_* variables below come from the options parsed above or, for CI and
# the container wrapper, from the environment. Release/packaging pipelines skip
# the test suite and the scene3D demos (neither ships, and no release flow runs
# ctest). PJ_BUILD_RASTER_HELPER goes the other way: the Linux release turns the
# standalone GPLv2 helper ON so packaging/appimage/build_appimage.sh can stage it.
PJ_FLAG_ARGS=()
# Every selection is passed explicitly on every run. CMake's option() keeps
# the cached value, so an unset -D would let a build/ configured once with
# tests ON keep its widget demos and marketplace app after --no-tests, and a
# tree configured with --minimal keep tests OFF after a plain ./build.sh.
if [[ "$SKIP_TEST" == "1" ]]; then
  PJ_BUILD_TESTS=OFF
  # Documented as following --no-tests; an explicit --no-widget-demos or
  # --no-marketplace-app can only turn them off, never back on.
  PJ_BUILD_WIDGET_DEMOS=OFF
  PJ_BUILD_MARKETPLACE_APP=OFF
fi
PJ_BUILD_TESTS="${PJ_BUILD_TESTS:-ON}"
PJ_BUILD_DEMOS="${PJ_BUILD_DEMOS:-ON}"
PJ_BUILD_WIDGET_DEMOS="${PJ_BUILD_WIDGET_DEMOS:-${PJ_BUILD_TESTS}}"
PJ_BUILD_MARKETPLACE_APP="${PJ_BUILD_MARKETPLACE_APP:-${PJ_BUILD_TESTS}}"
PJ_FLAG_ARGS+=(
  "-DPJ_BUILD_TESTS=${PJ_BUILD_TESTS}"
  "-DPJ_BUILD_DEMOS=${PJ_BUILD_DEMOS}"
  "-DPJ_BUILD_WIDGET_DEMOS=${PJ_BUILD_WIDGET_DEMOS}"
  "-DPJ_BUILD_MARKETPLACE_APP=${PJ_BUILD_MARKETPLACE_APP}"
)
[[ -n "${PJ_BUILD_RASTER_HELPER:-}" ]] && PJ_FLAG_ARGS+=("-DPJ_BUILD_RASTER_HELPER=${PJ_BUILD_RASTER_HELPER}")

# PJ_DEBUG_INFO/PJ_COMPRESS_DEBUG keep Linux dev trees debuggable without
# duplicating DWARF; see cmake/PjDebugInfo.cmake for the policy.
if [[ "$(uname -s)" == Linux ]]; then
  : "${PJ_DEBUG_INFO:=split}"
  : "${PJ_COMPRESS_DEBUG:=ON}"
fi
[[ -n "${PJ_DEBUG_INFO:-}" ]] && PJ_FLAG_ARGS+=("-DPJ_DEBUG_INFO=${PJ_DEBUG_INFO}")
[[ -n "${PJ_COMPRESS_DEBUG:-}" ]] && PJ_FLAG_ARGS+=("-DPJ_COMPRESS_DEBUG=${PJ_COMPRESS_DEBUG}")

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
    -DPJ_SDK_LOCAL_DIR="${SDK_LOCAL_DIR}" \
    "${CMAKE_CCACHE_ARGS[@]+"${CMAKE_CCACHE_ARGS[@]}"}" "${PJ_FLAG_ARGS[@]+"${PJ_FLAG_ARGS[@]}"}" \
    -DPJ_BUILD_TESTS=ON

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
  # Dependencies stay RelWithDebInfo because Release luau built from source is
  # not PIC and fails to link into PIE executables. Revisit when the Conan
  # remote ships Release binaries.
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

# PJ_BUILD_TARGET: packaging builds set it to pj_app so only the shipped closure
# (app + pj-plugin-check, which pj_app depends on) is linked; `all` is the dev default.
cmake --build "$BUILD_DIR" --target "${PJ_BUILD_TARGET:-all}" -j "$(nproc)"
