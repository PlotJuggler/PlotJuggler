#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/versions.env"

# Disable the IBus platform input context: it's loaded from the system Qt
# install (often an older major version) and can segfault under the pinned Qt.
export QT_IM_MODULE=""

# Qt-Advanced-Docking 5.1 supports native Wayland (drag, drop overlays, floating windows).
# Uncomment the next line to fall back to XWayland if a regression appears.
# export QT_QPA_PLATFORM=xcb

# Point Qt plugin discovery at the Qt version pinned in versions.env only. If the user's shell
# has QT_PLUGIN_PATH set to a stale Qt (e.g. /home/.../qt/6.4.2/plugins), Qt
# scans it first, picks up the cert-only TLS backend there, then fails to load
# its OpenSSL sibling (symbol mismatch against the newer libstdc++) and all
# HTTPS traffic breaks — including the marketplace registry fetch.
export QT_PLUGIN_PATH="${SCRIPT_DIR}/.qt/${PJ_QT_VERSION}/gcc_64/plugins"

BIN="${SCRIPT_DIR}/build/pj_app/plotjuggler4"

# --apitrace: launch under apitrace to capture a GL call trace (a smoke-test for
# redundant per-frame GL work — shader recompiles, full-cloud re-uploads, etc.).
# The flag is consumed here; all other arguments are forwarded to pj_app. If
# apitrace isn't installed we log and launch normally rather than failing.
#
#   ./run.sh --apitrace
#     → load a topic, interact briefly, quit. Keep it SHORT: apitrace records
#       buffer payloads, so traces grow fast.
#   Analyse (frames vs one-time GL setup that's wrongly per-frame):
#     t=./pj_app.trace
#     echo "frames:   $(apitrace dump "$t" | grep -c glXSwapBuffers)"
#     echo "compiles: $(apitrace dump "$t" | grep -c glCompileShader)"
#     echo "uploads:  $(apitrace dump "$t" | grep -c glBufferData)"
#   Healthy → compiles a small constant (≈ #programs) regardless of frames.
#   Env overrides: PJ_TRACE_API (gl|egl; try egl if the trace is empty),
#                  PJ_TRACE_OUT (output path; default ./pj_app.trace).
#
# --heaptrack: launch under heaptrack to profile heap allocations (peak RSS,
# leaks, top allocators). The flag is consumed here; all other arguments are
# forwarded to pj_app. If heaptrack isn't installed we log and launch normally.
#
#   ./run.sh --heaptrack
#     → load data, exercise the suspect path, quit cleanly (heaptrack finalizes
#       and compresses its trace on a normal exit — don't kill -9 it).
#   Output: $PWD/heaptrack.pj_app.<pid>.zst (override the path with
#           PJ_HEAPTRACK_OUT=/some/prefix).
#   Analyse: heaptrack_gui <file>   (or headless: heaptrack --analyze <file>)
use_apitrace=0
use_heaptrack=0
app_args=()
for arg in "$@"; do
  case "$arg" in
    --apitrace) use_apitrace=1 ;;
    --heaptrack) use_heaptrack=1 ;;
    *) app_args+=("$arg") ;;
  esac
done

if [ "$use_apitrace" -eq 1 ]; then
  if command -v apitrace >/dev/null 2>&1; then
    out="${PJ_TRACE_OUT:-${SCRIPT_DIR}/pj_app.trace}"
    echo "run.sh: tracing GL with apitrace -> ${out}" >&2
    exec apitrace trace --api "${PJ_TRACE_API:-gl}" --output "${out}" \
         "${BIN}" ${app_args[@]+"${app_args[@]}"}
  fi
  echo "run.sh: --apitrace requested but 'apitrace' is not installed; launching normally." >&2
fi

if [ "$use_heaptrack" -eq 1 ]; then
  if command -v heaptrack >/dev/null 2>&1; then
    ht_opts=()
    if [ -n "${PJ_HEAPTRACK_OUT:-}" ]; then
      ht_opts+=("-o" "${PJ_HEAPTRACK_OUT}")
      echo "run.sh: profiling heap with heaptrack -> ${PJ_HEAPTRACK_OUT}.zst" >&2
    else
      echo "run.sh: profiling heap with heaptrack -> \$PWD/heaptrack.pj_app.<pid>.zst" >&2
    fi
    echo "run.sh: analyse the result with 'heaptrack_gui <file>' (or 'heaptrack --analyze <file>')." >&2
    exec heaptrack ${ht_opts[@]+"${ht_opts[@]}"} \
         "${BIN}" ${app_args[@]+"${app_args[@]}"}
  fi
  echo "run.sh: --heaptrack requested but 'heaptrack' is not installed; launching normally." >&2
fi

exec "${BIN}" ${app_args[@]+"${app_args[@]}"}
