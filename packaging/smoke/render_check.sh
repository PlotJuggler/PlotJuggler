#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
#
# Proves that a QRhi build of PlotJuggler 4 renders: it opens the fixture layout
# next to this script (a plot, a Scene3D view and a media viewer) on --test-data,
# grabs the 3D view with --screenshot, and checks what every view logged.
#
#   render_check.sh <plotjuggler4> [<extra app args>...]
#
# The command may carry a wrapper, e.g. `env PATH=/c/Windows/System32 app.exe`
# to run with a bare PATH. It needs a display: xvfb-run, a compositor, or a
# Windows desktop session. POSIX sh, so it also runs under dash (the .deb smoke
# container) and Git Bash (Windows).
#
# Environment:
#   PJ_RENDER_BACKEND  the QRhi backend every view must report (QRhi::backendName()).
#                      Default: D3D11 on Windows, Metal on macOS, OpenGL elsewhere.
#   PJ_RENDER_TIMEOUT  seconds before a run counts as hung (default 90).
#   PJ_RENDER_SKIP_WITHOUT_DISPLAY=1  exit 77 (ctest's skip) instead of failing
#                      when there is no display to render on (QT_QPA_PLATFORM
#                      offscreen/minimal, or no DISPLAY/WAYLAND_DISPLAY off
#                      Windows and macOS), unless PJ_REQUIRE_GPU_TESTS=1. The
#                      build tree's ctest sets it; the packaging smokes do not.
#
# It fails unless:
#   - the app exits 0 (--screenshot exits non-zero on a missing or blank grab);
#   - the grab reports <backend>, and the Scene3D view and the media viewer each
#     log "<objectName>: <backend>, QRhiDriverInfo(...)", with no view on another
#     backend;
#   - every plot of the fixture logs "PJ_WASM_PLOT_RHI_READY backend=<backend>".
set -eu

if [ "$#" -lt 1 ]; then
  echo "usage: render_check.sh <plotjuggler4> [<extra app args>...]" >&2
  exit 2
fi

if [ "${PJ_RENDER_SKIP_WITHOUT_DISPLAY:-}" = 1 ] && [ "${PJ_REQUIRE_GPU_TESTS:-}" != 1 ]; then
  no_display=""
  case "${QT_QPA_PLATFORM:-}" in offscreen* | minimal*) no_display=1 ;; esac
  case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN* | Darwin) ;;
    *) [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || no_display=1 ;;
  esac
  if [ -n "${no_display}" ]; then
    echo "SKIP: no display to render on (QT_QPA_PLATFORM='${QT_QPA_PLATFORM:-}')"
    exit 77
  fi
fi

case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*) default_backend=D3D11 ;;
  Darwin) default_backend=Metal ;;
  *) default_backend=OpenGL ;;
esac
backend="${PJ_RENDER_BACKEND:-${default_backend}}"
fixture="$(cd "$(dirname "$0")" && pwd)/render_fixture.pj4.xml"
test -f "${fixture}" || { echo "FAIL: fixture ${fixture} missing" >&2; exit 1; }
# One ready line per plot area in the fixture.
plots="$(grep -c '<plot ' "${fixture}")"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT HUP INT TERM
log="${work}/app.log"

# pj.graphics carries each view's backend line. A GUI-subsystem Windows exe
# otherwise sends qInfo to OutputDebugString, which no pipe sees.
export QT_LOGGING_RULES="${QT_LOGGING_RULES:+${QT_LOGGING_RULES};}pj.graphics.info=true"
export QT_FORCE_STDERR_LOGGING=1
export PJ_DISABLE_TELEMETRY=1
# Fresh settings, so a developer's saved preferences (the GPU plot canvas can be
# switched off there) cannot change what is checked. XDG only: Windows keeps
# QSettings in the registry, which a CI runner starts without.
export XDG_CONFIG_HOME="${work}/config" XDG_DATA_HOME="${work}/data" XDG_CACHE_HOME="${work}/cache"

timeout_cmd=""
command -v timeout >/dev/null 2>&1 && timeout_cmd="timeout ${PJ_RENDER_TIMEOUT:-90}"

echo "--- render check: ${backend}, ${plots} plot(s), $*"
rc=0
# shellcheck disable=SC2086  # timeout_cmd is empty or a command and its argument
${timeout_cmd} "$@" --nosplash --test-data --layout "${fixture}" --screenshot "${work}/shot.png" \
  >"${log}" 2>&1 || rc=$?

fail() {
  echo "FAIL: $*"
  echo "--- app log (tail) ---"
  tail -80 "${log}"
  exit 1
}

test "${rc}" -eq 0 || fail "the app exited ${rc} (124: killed by the timeout)"
grep -q "^\[screenshot\] graphics backend: ${backend}" "${log}" || fail "the grab did not report ${backend}"
for view in scene3dRhiCanvas scene2dMediaViewer; do
  grep -Eq "${view}\"?: ${backend}, QRhiDriverInfo\(" "${log}" || fail "${view} did not log a ${backend} QRhi"
done
if grep 'QRhiDriverInfo(' "${log}" | grep -v ": ${backend}, QRhiDriverInfo("; then
  fail "a view ran on something other than ${backend} (lines above)"
fi
ready="$(grep -c "PJ_WASM_PLOT_RHI_READY backend=${backend} " "${log}" || true)"
test "${ready}" -eq "${plots}" || fail "${ready} of ${plots} plot(s) reported a ${backend} QRhi canvas"
if grep 'PJ_WASM_PLOT_RHI_READY' "${log}" | grep -v "backend=${backend} "; then
  fail "a plot canvas ran on something other than ${backend} (lines above)"
fi

grep 'QRhiDriverInfo(\|PJ_WASM_PLOT_RHI_READY\|^\[screenshot\]' "${log}"
echo "OK: every view rendered on ${backend}"
