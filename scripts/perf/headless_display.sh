# SPDX-License-Identifier: MPL-2.0
# Sourced helper: a private headless GNOME Shell + rootful Xwayland, so a run
# owns a display no other window can cover.
#
#   source scripts/perf/headless_display.sh
#   start_display <display-num> <run-dir> [WxH]   # e.g. 96 /tmp/pjx96; default 1920x1080
#   ... DISPLAY=:<display-num> ...
#   stop_display <run-dir>
#
# start_display returns 0 when it started the display, 3 when a live display
# (the pid in /tmp/.X<n>-lock is alive) already owns the number and was left
# alone, and 1 (message on stderr) when Xwayland never appears. A socket whose
# lock owner is dead is stale: it is removed and the display started. It records the compositor and
# Xwayland pids in <run-dir>/shell.pid and xw.pid; stop_display kills those and
# every other process whose environment has XDG_RUNTIME_DIR=<run-dir> (gnome-shell
# leaves ~30 session helpers behind).

start_display() {
  local num=$1 r=$2 size=${3:-1920x1080} sock="/tmp/.X11-unix/X$1" wl="pjperf$1"
  if [[ -S $sock ]]; then
    local pid; pid=$(tr -d ' ' <"/tmp/.X$1-lock" 2>/dev/null)
    [[ -n $pid ]] && kill -0 "$pid" 2>/dev/null && return 3
    rm -f "$sock" "/tmp/.X$1-lock"  # stale: nothing serves it
  fi
  mkdir -p "$r"; chmod 700 "$r"
  (cd /tmp && XDG_RUNTIME_DIR=$r setsid dbus-run-session -- gnome-shell --headless --wayland \
    --virtual-monitor "$size" --wayland-display "$wl" >"$r/shell.log" 2>&1 </dev/null & echo $! >"$r/shell.pid")
  for _ in $(seq 60); do [[ -S $r/$wl ]] && break; sleep 0.5; done
  sleep 3
  (cd /tmp && env -u DISPLAY XDG_RUNTIME_DIR="$r" WAYLAND_DISPLAY="$wl" setsid Xwayland ":$num" \
    -geometry "$size" -noreset >"$r/xwayland.log" 2>&1 </dev/null & echo $! >"$r/xw.pid")
  for _ in $(seq 40); do [[ -S $sock ]] && break; sleep 0.5; done
  [[ -S $sock ]] || { echo "Xwayland did not start (see $r)" >&2; return 1; }
  sleep 2
}

stop_display() {
  local r=$1 e p
  kill "$(cat "$r/xw.pid" 2>/dev/null)" "$(cat "$r/shell.pid" 2>/dev/null)" 2>/dev/null || true
  for e in /proc/[0-9]*/environ; do
    if tr '\0' '\n' 2>/dev/null <"$e" | grep -qx "XDG_RUNTIME_DIR=$r"; then
      p=${e#/proc/}; kill "${p%/environ}" 2>/dev/null || true
    fi
  done
  return 0
}
