#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Render the packaged application through native Wayland, without XWayland.
# Usage: wayland_check.sh <app command...>  (the same arguments as render_check.sh)
set -eu
command -v weston >/dev/null || { echo "ERROR: install weston for the Wayland smoke check" >&2; exit 1; }
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=$(mktemp -d)
weston_pid=
cleanup() {
  rc=$?
  trap - EXIT HUP INT TERM
  if [ "$rc" -ne 0 ]; then cat "$work/weston.log" >&2 2>/dev/null || true; fi
  if [ -n "$weston_pid" ]; then
    kill "$weston_pid" 2>/dev/null || true
    wait "$weston_pid" 2>/dev/null || true
  fi
  rm -rf "$work"
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
export XDG_RUNTIME_DIR="$work/runtime" WAYLAND_DISPLAY=pj4-smoke
mkdir -m 700 "$XDG_RUNTIME_DIR"
weston --backend=headless-backend.so --use-gl --shell=kiosk-shell.so \
  --socket="$WAYLAND_DISPLAY" --idle-time=0 --no-config --width=1280 --height=800 \
  >"$work/weston.log" 2>&1 &
weston_pid=$!
attempt=0
while [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; do
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 100 ] || ! kill -0 "$weston_pid" 2>/dev/null; then
    echo "ERROR: headless Wayland compositor failed to start" >&2
    exit 1
  fi
  sleep 0.1
done
unset DISPLAY
export QT_QPA_PLATFORM=wayland
sh "$script_dir/render_check.sh" "$@"
