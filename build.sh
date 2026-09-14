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
    "  --debug-info LEVEL   none|lines|full|split (Linux default: split, asan: lines)" \
    "  --no-compress-debug  Keep DWARF uncompressed (default on Linux: zlib-compressed)" \
    "  --sanitize LANE      none|asan|tsan|msan (default: none; trees: build/, build/asan, build/tsan, build/msan)" \
    "  --tsan               Deprecated alias for --sanitize tsan: build and run concurrency tests" \
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
    "PJ_BUILD_WIDGET_DEMOS, PJ_BUILD_MARKETPLACE_APP, PJ_BUILD_TARGET, PJ_DEBUG_INFO," \
    "PJ_COMPRESS_DEBUG and PJ_SANITIZE environment variables (how CI and the" \
    "container wrapper drive this script); an option always wins over the variable." \
    "PJ_DRY_RUN=1 prints the lane, build directory and CMake arguments without building."
}

# `./build.sh --tsan` builds + runs the Qt-free foundation concurrency tests under
# ThreadSanitizer in a separate build/tsan tree.
# It guards the datastore worker-thread race regressions; the Linux CI `tsan` job
# runs the same target set. TSan is wired via -DPJ_ENABLE_TSAN=ON on a normal
# RelWithDebInfo configure, so the Conan dependency closure is reused as-is (no
# Debug rebuild), except for the SDK, which must be built from source as well.
PJ_SANITIZE="${PJ_SANITIZE-none}"
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
set_sanitize() {
  [[ "$1" =~ ^(none|asan|tsan|msan)$ ]] || bad_value "--sanitize/PJ_SANITIZE must be none, asan, tsan or msan (got '$1')"
  PJ_SANITIZE="$1"
}
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
    --sanitize) set_sanitize "${2:-}"; shift ;;
    --sanitize=*) set_sanitize "${1#--sanitize=}" ;;
    --tsan) set_sanitize tsan ;;
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
set_sanitize "${PJ_SANITIZE}"
if [[ "${PJ_SANITIZE}" == tsan ]]; then
  TSAN=1
fi
MSAN=0
if [[ "${PJ_SANITIZE}" == msan ]]; then
  MSAN=1
fi
# The baked container entrypoint chowns /work/build recursively; sibling trees
# would leave instrumented artifacts root-owned on the host.
BUILD_REL=build
[[ "${PJ_SANITIZE}" == none ]] || BUILD_REL+="/${PJ_SANITIZE}"
BUILD_DIR="${SCRIPT_DIR}/${BUILD_REL}"

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
    echo "--tsan/--sanitize tsan needs the test suite: it cannot be combined with --no-tests/--minimal or PJ_BUILD_TESTS=OFF" >&2
    exit 2
  fi
  # PJ_BUILD_TARGET is honoured here (it defaults to `all`), so a caller can
  # narrow a TSan run to one target while iterating. It is deliberately NOT
  # restricted: the lane's whole point is that nothing curates the target set.
fi

# MemorySanitizer covers only targets whose ENTIRE in-process closure can be
# instrumented. That is not a preference: MSan reports reads of memory it never
# saw written, so one uninstrumented writer turns correct code into a stream of
# false reports, and MSan has no runtime suppressions to mute them. Qt is
# consumed prebuilt and the GL stack dlopens a GPU driver, so any target reaching
# either is excluded permanently rather than pending work.
#
# These four link only pj_datastore, pj_base, nanoarrow and GoogleTest. Every
# other runner selects a Qt test main (cmake/PjTests.cmake) or pulls CPython.
# sanitizer_msan_selftest is FIRST and always built: it deliberately reads
# uninitialised memory, so if the lane stops reporting it, the other three
# passing proves nothing.
MSAN_TARGETS=(sanitizer_msan_selftest pj_datastore_tests chunk_test engine_concurrency_test engine_thread_safety_test)
CONAN_LANE_ARGS=()
if [[ "$MSAN" == "1" ]]; then
  if [[ "$SKIP_TEST" == "1" ]] || { [[ -n "${PJ_BUILD_TESTS:-}" && ! "${PJ_BUILD_TESTS^^}" =~ ^(ON|1|TRUE|YES|Y)$ ]]; }; then
    echo "--sanitize msan needs the test suite: it cannot be combined with --no-tests/--minimal or PJ_BUILD_TESTS=OFF" >&2
    exit 2
  fi
  command -v clang++ >/dev/null 2>&1 || {
    echo "--sanitize msan requires Clang: MemorySanitizer does not exist in GCC, which rejects" >&2
    echo "                                -fsanitize=memory outright. Install clang in the builder." >&2
    exit 2
  }
  PJ_MSAN_LIBCXX_ROOT="${PJ_MSAN_LIBCXX_ROOT:-/opt/msan-libcxx}"
  if [[ ! -e "${PJ_MSAN_LIBCXX_ROOT}/lib/libc++.so" ]]; then
    echo "--sanitize msan needs an instrumented libc++ at ${PJ_MSAN_LIBCXX_ROOT}." >&2
    echo "  The distro libc++ is a normal build; linking it makes every std:: read" >&2
    echo "  report as uninitialised. Build one with:" >&2
    echo "    scripts/build_msan_libcxx.sh --prefix ${PJ_MSAN_LIBCXX_ROOT}" >&2
    exit 2
  fi
  # A DEDICATED Conan cache, because the settings above do not isolate this lane.
  # The sanitizer flags travel in tools.build:*, which does NOT participate in
  # package_id, and compiler.libcxx is dropped for C-only packages such as
  # nanoarrow — so an ordinary Clang build and this instrumented one resolve to
  # the SAME id. With a shared cache, --build=missing would reuse an
  # uninstrumented nanoarrow here, and an instrumented one would contaminate
  # later ordinary builds. Neither is detectable from the final executable: it
  # still carries __msan_ symbols from our own code, so the lane would report a
  # confident, meaningless clean run.
  #
  # This is the same defect that made ensure_core.sh fetch a prebuilt
  # uninstrumented SDK and call it instrumented. Separate caches make it
  # impossible rather than unlikely.
  export CONAN_HOME="${PJ_MSAN_CONAN_HOME:-${HOME}/.conan2-msan}"
  # A dedicated cache starts empty, so it has no default profile and Conan
  # refuses to resolve anything. Detect one on first use. The detected compiler
  # does not matter: every setting this lane depends on is passed explicitly
  # below, and compiler_executables decides what actually runs.
  if [[ ! -f "${CONAN_HOME}/profiles/default" ]]; then
    echo "msan lane: initialising dedicated Conan cache at ${CONAN_HOME}"
    conan profile detect --exist-ok >/dev/null
  fi
  # And build the closure from source regardless of what that cache holds: a
  # cache is only trustworthy if nothing ever wrote an uninstrumented package
  # into it, which is an invariant no build can verify after the fact.
  CONAN_LANE_ARGS+=(--build="nanoarrow/*" --build="gtest/*")
  CONAN_LANE_ARGS+=(
    -s compiler=clang
    -s "compiler.version=${PJ_SANITIZER_CLANG_VERSION:-22}"
    -s compiler.libcxx=libc++
    # compiler_executables, not just the settings above. `-s compiler=clang`
    # only labels the package_id and the generated toolchain; Conan still
    # invokes the default cc/c++, so the dependency builds ran GCC with a
    # Clang-only flag and died on "unrecognized argument to -fsanitize=memory".
    # The setting states intent; this states what actually runs.
    -c 'tools.build:compiler_executables={"c":"clang","cpp":"clang++"}'
    -c "tools.build:cxxflags=['-fsanitize=memory','-fsanitize-memory-track-origins=2','-fno-omit-frame-pointer','-stdlib=libc++','-nostdinc++','-isystem${PJ_MSAN_LIBCXX_ROOT}/include/c++/v1']"
    -c "tools.build:cflags=['-fsanitize=memory','-fno-omit-frame-pointer']"
    -c "tools.build:sharedlinkflags=['-fsanitize=memory','-L${PJ_MSAN_LIBCXX_ROOT}/lib']"
    -c "tools.build:exelinkflags=['-fsanitize=memory','-L${PJ_MSAN_LIBCXX_ROOT}/lib']"
  )
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
# Every instrumented lane compiles with Clang; ordinary builds keep the default
# GCC. GCC 11's ThreadSanitizer cannot see condition_variable timed waits (GCC PR
# 101978) and reports a false double lock at every wait_for; MemorySanitizer
# exists only in Clang; and one compiler for all instrumentation keeps the
# sanitizer runtimes of the app, the SDK and the plugins consistent. Named
# explicitly because a cached GCC would otherwise be kept. The Conan profile stays
# GCC: Clang compiles against the same GCC 11 libstdc++, so the prebuilt
# dependency binaries link unchanged. See docs/SANITIZERS.md.
SANITIZER_COMPILER_ARGS=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
case "${PJ_SANITIZE}" in
  none) PJ_FLAG_ARGS+=(-DPJ_ENABLE_SANITIZERS=OFF -DPJ_ENABLE_TSAN=OFF -DPJ_SDK_FORCE_SOURCE=OFF) ;;
  asan) PJ_FLAG_ARGS+=(-DPJ_ENABLE_SANITIZERS=ON -DPJ_ENABLE_TSAN=OFF -DPJ_SDK_FORCE_SOURCE=ON "${SANITIZER_COMPILER_ARGS[@]}") ;;
  tsan) PJ_FLAG_ARGS+=(-DPJ_ENABLE_SANITIZERS=OFF -DPJ_ENABLE_TSAN=ON -DPJ_SDK_FORCE_SOURCE=ON "${SANITIZER_COMPILER_ARGS[@]}") ;;
  msan)
    PJ_FLAG_ARGS+=(
      -DPJ_ENABLE_SANITIZERS=OFF -DPJ_ENABLE_TSAN=OFF -DPJ_ENABLE_MSAN=ON
      -DPJ_SDK_FORCE_SOURCE=ON
      "${SANITIZER_COMPILER_ARGS[@]}"
      "-DPJ_MSAN_LIBCXX_ROOT=${PJ_MSAN_LIBCXX_ROOT}"
      -DPJ4_BUILD_APP=OFF -DPJ_BUILD_TESTS=ON
    ) ;;
esac
# Passed on every configure rather than left to its option() default. CMake's
# option() does not overwrite an existing cache entry, so a build tree that was
# once configured with container annotations ON keeps them forever — silently
# re-enabling the libstdc++ vector annotations and reproducing the Luau ODR
# false positive that PJ_SANITIZE_CONTAINERS=OFF exists to avoid. Naming it here
# makes the lane, not the stale cache, decide.
PJ_FLAG_ARGS+=("-DPJ_SANITIZE_CONTAINERS=${PJ_SANITIZE_CONTAINERS:-OFF}")

# PJ_DEBUG_INFO/PJ_COMPRESS_DEBUG keep Linux dev trees debuggable without
# duplicating DWARF; see cmake/PjDebugInfo.cmake for the policy.
if [[ "$(uname -s)" == Linux ]]; then
  if [[ "${PJ_SANITIZE}" == asan ]]; then
    : "${PJ_DEBUG_INFO:=lines}"
  else
    : "${PJ_DEBUG_INFO:=split}"
  fi
  : "${PJ_COMPRESS_DEBUG:=ON}"
fi
[[ -n "${PJ_DEBUG_INFO:-}" ]] && PJ_FLAG_ARGS+=("-DPJ_DEBUG_INFO=${PJ_DEBUG_INFO}")
[[ -n "${PJ_COMPRESS_DEBUG:-}" ]] && PJ_FLAG_ARGS+=("-DPJ_COMPRESS_DEBUG=${PJ_COMPRESS_DEBUG}")

CMAKE_ARGS=(
  -S "$SCRIPT_DIR" -B "$BUILD_DIR"
  "-DCMAKE_TOOLCHAIN_FILE=$BUILD_DIR/conan_toolchain.cmake"
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  "-DCMAKE_PREFIX_PATH=${QT_DIR}"
  "-DPJ_SDK_LOCAL_DIR=${SDK_LOCAL_DIR}"
  "${CMAKE_CCACHE_ARGS[@]+"${CMAKE_CCACHE_ARGS[@]}"}" "${PJ_FLAG_ARGS[@]}"
)
if [[ "$TSAN" == "1" ]]; then
  # The app is built like everything else. It used to be skipped only because the
  # container entrypoint unconditionally packages and a TSan tree has nothing to
  # package; that is a packaging constraint, not a ThreadSanitizer one, and
  # build_in_docker.sh now runs the lane without invoking the packager.
  CMAKE_ARGS+=(-DPJ4_BUILD_APP=ON -DPJ_BUILD_TESTS=ON)
else
  CMAKE_ARGS+=("-DPJ_VERSION=${PJ_VERSION:-${PJ_APP_VERSION}}" "-DPJ_INSTALLATION=${PJ_INSTALLATION:-source}")
fi

# Share the actual configure arguments with the preview, before requiring Qt,
# creating build artifacts, or contacting Conan/remotes.
if [[ "${PJ_DRY_RUN:-0}" == "1" ]]; then
  printf 'Sanitizer lane: %s\nBuild directory: %s\n' "${PJ_SANITIZE}" "${BUILD_DIR}"
  printf 'cmake'
  printf ' %q' "${CMAKE_ARGS[@]}"
  printf '\n'
  exit 0
fi

# Checked after the dry-run preview so the preview works without a toolchain.
if [[ "${PJ_SANITIZE}" != none ]] && ! command -v clang++ >/dev/null 2>&1; then
  echo "--sanitize ${PJ_SANITIZE} compiles with Clang (see docs/SANITIZERS.md), but clang++ is not on PATH." >&2
  echo "  Install clang-${PJ_SANITIZER_CLANG_VERSION:-22} from apt.llvm.org and link it as clang/clang++, or run the lane through" >&2
  echo "  packaging/appimage/build_in_docker.sh, which provisions it." >&2
  exit 2
fi

if [[ ! -d "$QT_DIR" ]]; then
  echo "Qt ${PJ_QT_VERSION} not found at ${QT_DIR}."
  echo "Install it with: ./scripts/install_qt6.sh"
  exit 1
fi

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

# The ThreadSanitizer runtime reserves fixed shadow-memory ranges and aborts with
# "FATAL: ThreadSanitizer: unexpected memory mapping" before running a single test
# when the kernel randomises mmap over a wider range than it expects. Current
# kernels default vm.mmap_rnd_bits to 32, so on an untuned host EVERY TSan binary
# dies instantly — which looks like a build problem, not a configuration one.
# Disabling randomisation for the test process is the fix that needs no host sysctl
# change. It requires the personality syscall, which Docker's default seccomp
# profile denies, so the container wrapper relaxes seccomp for this lane; when the
# syscall is refused anyway, run unwrapped and let the runtime speak for itself
# rather than skipping the lane and reporting success.
SAN_LAUNCH=()
if [[ "$TSAN" == "1" || "$MSAN" == "1" ]]; then
  if setarch -R true >/dev/null 2>&1; then
    SAN_LAUNCH=(setarch -R)
  else
    echo "warning: 'setarch -R' unavailable; TSan aborts with 'unexpected memory mapping' and MSan with" >&2
    echo "         'can not mmap the shadow memory' — both before running a single test." >&2
    echo "         Run the lane through packaging/appimage/build_in_docker.sh --sanitize tsan, which relaxes seccomp." >&2
  fi
fi

# Pin resolution to the explicit remote list selected above. A developer machine
# may have unrelated private remotes that host forked recipes under a user
# channel; those must never shadow PJ4's intended dependency graph.
if [[ "$SKIP_CONAN_INSTALL" == "0" ]]; then
  # Dependencies stay RelWithDebInfo because Release luau built from source is
  # not PIC and fails to link into PIE executables. Revisit when the Conan
  # remote ships Release binaries.
  # The MSan lane resolves its OWN minimal requirement list. The app's conanfile
  # would drag FFmpeg, CPython and the rest into a from-source rebuild under
  # Clang + instrumented libc++, to instrument code its four Qt-free targets
  # never load. The lockfile is skipped with it: it pins the app graph, not this
  # one.
  conan_recipe="$SCRIPT_DIR"
  conan_lock_args=(${CONAN_LOCKFILE_ARGS[@]+"${CONAN_LOCKFILE_ARGS[@]}"})
  if [[ "$MSAN" == "1" ]]; then
    conan_recipe="$SCRIPT_DIR/cmake/msan"
    conan_lock_args=()
  fi
  conan install "$conan_recipe" --output-folder="$BUILD_DIR" --build=missing \
    ${conan_lock_args[@]+"${conan_lock_args[@]}"} \
    -s build_type=RelWithDebInfo -s compiler.cppstd=20 \
    ${CONAN_LANE_ARGS[@]+"${CONAN_LANE_ARGS[@]}"} "${CONAN_REMOTE_ARGS[@]}"
elif [[ ! -f "$BUILD_DIR/conan_toolchain.cmake" ]]; then
  echo "Missing $BUILD_DIR/conan_toolchain.cmake; run Conan install first." >&2
  exit 1
fi

cmake "${CMAKE_ARGS[@]}"

# Surface the compile DB (CMAKE_EXPORT_COMPILE_COMMANDS writes it in the selected tree) at
# the repo root so clangd/editors resolve includes without extra config. The root
# path is gitignored; the relative target survives a worktree move.
ln -sf "${BUILD_REL}/compile_commands.json" "$SCRIPT_DIR/compile_commands.json"

if [[ "$MSAN" == "1" ]]; then
  cmake --build "$BUILD_DIR" --target "${MSAN_TARGETS[@]}" -j "$(nproc)"

  # An uninstrumented dependency slipping into the closure is the failure this
  # lane cannot survive, and it is silent: the build succeeds and the reports
  # look like real bugs in our code. Check the linked binaries carry MSan
  # instrumentation before trusting anything they say.
  for t in "${MSAN_TARGETS[@]}"; do
    bin="$(find "$BUILD_DIR" -type f -executable -name "$t" -print -quit)"
    if [[ -z "$bin" ]]; then
      echo "error: ${t} was not produced under ${BUILD_DIR}; cannot report on it." >&2
      exit 4
    fi
    # Both symbol tables: the MSan runtime is linked statically into the
    # executable, so which table carries __msan_ depends on how the link was
    # done. Checking only the dynamic table gives a false "uninstrumented".
    msan_syms="$( { nm -D "$bin" 2>/dev/null; nm "$bin" 2>/dev/null; } | grep -c '__msan_' || true)"
    if [[ "${msan_syms}" -eq 0 ]]; then
      echo "error: ${t} is not MemorySanitizer-instrumented; refusing to report on it." >&2
      echo "       binary: ${bin}" >&2
      echo "       (a clean run here means MSan reports would be meaningless, not that the code is clean)" >&2
      exit 4
    fi
  done

  # exitcode=77 distinguishes an MSan report from an ordinary test failure.
  # halt_on_error stops at the first, because an uninitialised value propagates
  # and every later report is likely an echo of the first.
  MSAN_OPTIONS="halt_on_error=1 exitcode=77 ${MSAN_OPTIONS:-}" \
    ${SAN_LAUNCH[@]+"${SAN_LAUNCH[@]}"} \
    ctest --test-dir "$BUILD_DIR" --output-on-failure --no-tests=error --timeout 120 \
      -R '^(sanitizer_msan_selftest$|pj_datastore_tests\.|chunk_test$|engine_concurrency_test$|engine_thread_safety_test$)'
  exit 0
fi

if [[ "$TSAN" == "1" ]]; then
  cmake --build "$BUILD_DIR" --target "${PJ_BUILD_TARGET:-all}" -j "$(nproc)"

  # Run under ctest so the per-test CMake TIMEOUT catches a deadlock regression,
  # and so TSan's non-zero exit (it dies on the first report under halt_on_error)
  # fails the test. The whole configured suite runs: a curated target list stops
  # covering new threaded code the moment someone forgets to extend it, and PJ4
  # spawns threads in seven modules, not the three the old list named.
  # suppressions: Qt, GLib and FFmpeg are prebuilt and uninstrumented, so TSan
  # cannot see the lock-free ordering inside them and invents races there. See
  # cmake/sanitizers/tsan.supp for the measurements behind each entry.
  # A narrowed build must narrow the RUN too. PJ_BUILD_TARGET is honoured above,
  # so an unfiltered ctest would try to execute every configured test and fail on
  # the executables that were deliberately not built — reporting the lane broken
  # when the selected target passed. Target and ctest names differ for some
  # runners, so --no-tests=error keeps an empty selection from passing.
  tsan_filter=()
  if [[ -n "${PJ_BUILD_TARGET:-}" && "${PJ_BUILD_TARGET}" != all ]]; then
    tsan_filter=(-R "^(${PJ_BUILD_TARGET}([.$]|$)|sanitizer_race_selftest$)")
  fi
  QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" \
  TSAN_OPTIONS="halt_on_error=1 history_size=4 suppressions=${SCRIPT_DIR}/cmake/sanitizers/tsan.supp ${TSAN_OPTIONS:-}" \
    ${SAN_LAUNCH[@]+"${SAN_LAUNCH[@]}"} \
    ctest --test-dir "$BUILD_DIR" --output-on-failure --no-tests=error --timeout 120 \
      ${tsan_filter[@]+"${tsan_filter[@]}"}
  exit 0
fi

# PJ_BUILD_TARGET: packaging builds set it to pj_app so only the shipped closure
# (app + pj-plugin-check, which pj_app depends on) is linked; `all` is the dev default.
cmake --build "$BUILD_DIR" --target "${PJ_BUILD_TARGET:-all}" -j "$(nproc)"
