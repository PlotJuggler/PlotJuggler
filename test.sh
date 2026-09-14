#!/usr/bin/env bash
# Runs the full ctest suite headless. Many widget tests create a QApplication
# without forcing a platform plugin, so a plain `ctest` pops their windows on
# the desktop; `offscreen` keeps them invisible (CI gets the same via xvfb).
# --sanitize selects the build tree and runtime defaults; other arguments go to
# ctest. An explicit --test-dir takes precedence over the lane's build tree.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/versions.env"

PJ_SANITIZE="${PJ_SANITIZE-none}"
TEST_DIR=""
CTEST_ARGS=()

bad_value() { echo "$1" >&2; exit 2; }
set_sanitize() {
  [[ "$1" =~ ^(none|asan|tsan)$ ]] || bad_value "--sanitize/PJ_SANITIZE must be none, asan or tsan (got '$1')"
  PJ_SANITIZE="$1"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --sanitize) set_sanitize "${2-}"; shift ;;
    --sanitize=*) set_sanitize "${1#--sanitize=}" ;;
    --test-dir)
      [[ $# -ge 2 && -n "$2" && "$2" != -* ]] || bad_value "--test-dir needs a directory"
      TEST_DIR="$2"
      shift
      ;;
    --test-dir=*)
      TEST_DIR="${1#--test-dir=}"
      [[ -n "$TEST_DIR" ]] || bad_value "--test-dir needs a directory"
      ;;
    *) CTEST_ARGS+=("$1") ;;
  esac
  shift
done
set_sanitize "$PJ_SANITIZE"

BUILD_DIR="${SCRIPT_DIR}/build"
[[ "$PJ_SANITIZE" == none ]] || BUILD_DIR+="/${PJ_SANITIZE}"
if [[ -z "$TEST_DIR" ]]; then
  TEST_DIR="$BUILD_DIR"
elif [[ "$(realpath -m -- "$TEST_DIR")" != "$(realpath -m -- "$BUILD_DIR")" ]]; then
  printf 'Warning: sanitizer lane "%s" selects "%s", but --test-dir "%s" wins; runtime options follow lane "%s".\n' \
    "$PJ_SANITIZE" "$BUILD_DIR" "$TEST_DIR" "$PJ_SANITIZE" >&2
fi

if [[ "$PJ_SANITIZE" != none ]]; then
  SUPPRESSION_FILE="${SCRIPT_DIR}/cmake/sanitizers/lsan.supp"
  # An explicitly empty caller value is intentional, too: use '-' rather than ':-'.
  # The leak suppression file goes to LSAN_OPTIONS only. ASan parses a
  # suppressions= file with its own grammar, rejects leak: entries, and aborts
  # every instrumented process with "failed to parse suppressions".
  export ASAN_OPTIONS="${ASAN_OPTIONS-abort_on_error=1:print_stacktrace=1:detect_leaks=1}"
  export LSAN_OPTIONS="${LSAN_OPTIONS-suppressions=${SUPPRESSION_FILE}}"
  export UBSAN_OPTIONS="${UBSAN_OPTIONS-print_stacktrace=1}"
  # Prebuilt Qt/GLib/FFmpeg are uninstrumented, so TSan cannot see their
  # lock-free ordering and reports races they do not have; without these the
  # PJ4 suite emits hundreds of reports, almost none in code we compile.
  export TSAN_OPTIONS="${TSAN_OPTIONS-halt_on_error=1 history_size=4 suppressions=${SCRIPT_DIR}/cmake/sanitizers/tsan.supp}"
  # CPython's obmalloc pools hide individual allocations from ASan.
  export PYTHONMALLOC="${PYTHONMALLOC-malloc}"
fi

export QT_QPA_PLATFORM=offscreen
export QT_IM_MODULE=""
export QT_PLUGIN_PATH="${SCRIPT_DIR}/.qt/${PJ_QT_VERSION}/gcc_64/plugins"

# GCC's ThreadSanitizer aborts with "unexpected memory mapping" before running any
# test when the kernel's mmap randomisation is wider than its fixed shadow ranges
# (vm.mmap_rnd_bits=32 on current kernels). Disabling randomisation for the test
# process avoids a host sysctl change. The personality syscall it needs is denied
# by Docker's default seccomp profile, so if the call is refused, run unwrapped and
# let the runtime report rather than pretending the lane ran.
TSAN_LAUNCH=()
if [[ "$PJ_SANITIZE" == tsan ]] && setarch -R true >/dev/null 2>&1; then
  TSAN_LAUNCH=(setarch -R)
fi

# Emit --test-dir once; the last caller-supplied directory wins if repeated.
exec ${TSAN_LAUNCH[@]+"${TSAN_LAUNCH[@]}"} \
  ctest --test-dir "$TEST_DIR" --output-on-failure --no-tests=error "${CTEST_ARGS[@]}"
