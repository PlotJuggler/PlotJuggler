#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# Gate A performance run (pj_scene3D/docs/TECHNICAL_NOTES.md §9): the frame
# benchmarks of a QRhi tree and a legacy GL tree, B's method. Builds nothing.
#
#   scripts/perf/gate_a.sh --rhi build --gl build-gl [--out DIR] [--rounds 5]
#       [--frames 30] [--display :96] [--only REGEX] [--no-vulkan] [--keep-display]
#       [--first-round K]
#
# --rhi: a QRhi tree (default configuration) with scene3d_frame_benchmark,
#   plot_canvas_benchmark and mixed_layout_benchmark built;
# --gl: a GL rollback tree (-DPJ_SCENE3D_RHI=OFF -DPJ_BUILD_DEMOS=ON) with
#   scene3d_frame_benchmark and mixed_layout_benchmark built.
#
# Method: a private headless GNOME Shell with a rootful Xwayland on --display
# (xcb: the Wayland QPA pins every frame to the compositor's 60 Hz callback),
# vsync off (vblank_mode=0, MESA_VK_WSI_PRESENT_MODE=immediate), every case in
# its own process, the configurations interleaved within each of --rounds
# rounds. Every run's output lands in DIR/raw/<round>/<config>__<case>.log,
# ending with its exit status (GATE_RUN_EXIT=<n>);
# gate_a_report.py DIR then writes DIR/gate_a.md (median [min–max] of the
# rounds, Gate A verdicts). --only keeps the cases whose "<config>__<case>"
# matches the regex; --no-vulkan drops the RHI Vulkan configuration.
# --first-round K numbers the rounds from K, so one --out can collect rounds run
# one at a time (each under the build lock, letting builds run in between).
#
# Hold the machine's build lock for the whole run (no compile may add noise),
# e.g. `flock /path/build.lock scripts/perf/gate_a.sh ...`. Needs gnome-shell,
# Xwayland and dbus-run-session; the GPU is whatever Mesa picks (check the
# report's device column).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RHI_TREE="" GL_TREE="" OUT="" FIRST_ROUND=1 ROUNDS=5 FRAMES=30 DISPLAY_ID=":96" ONLY="" VULKAN=1 KEEP_DISPLAY=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --rhi) RHI_TREE="$(realpath "$2")"; shift 2 ;;
    --gl) GL_TREE="$(realpath "$2")"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --rounds) ROUNDS="$2"; shift 2 ;;
    --first-round) FIRST_ROUND="$2"; shift 2 ;;
    --frames) FRAMES="$2"; shift 2 ;;
    --display) DISPLAY_ID="$2"; shift 2 ;;
    --only) ONLY="$2"; shift 2 ;;
    --no-vulkan) VULKAN=0; shift ;;
    --keep-display) KEEP_DISPLAY=1; shift ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$RHI_TREE" && -n "$GL_TREE" ]] || { echo "--rhi and --gl are required" >&2; exit 2; }
OUT="${OUT:-$ROOT/build/gate_a_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$OUT/raw"

bin() {  # bin <tree> <name>: the built executable, or empty
  find "$1" -type f -name "$2" -perm -u+x 2>/dev/null | head -n 1
}
RHI_S3D="$(bin "$RHI_TREE" scene3d_frame_benchmark)"
RHI_PLOT="$(bin "$RHI_TREE" plot_canvas_benchmark)"
RHI_MIXED="$(bin "$RHI_TREE" mixed_layout_benchmark)"
GL_S3D="$(bin "$GL_TREE" scene3d_frame_benchmark)"
GL_MIXED="$(bin "$GL_TREE" mixed_layout_benchmark)"
for exe in RHI_S3D RHI_PLOT RHI_MIXED GL_S3D GL_MIXED; do
  [[ -n "${!exe}" ]] || { echo "missing executable: $exe (build it first)" >&2; exit 2; }
done

# --- the display: headless GNOME Shell + rootful Xwayland ----------------------
DISPLAY_NUM="${DISPLAY_ID#:}"
RUN_DIR="/tmp/pjx${DISPLAY_NUM}"
SHELL_PID="" XWAYLAND_PID=""
stop_display() {
  [[ -n "$XWAYLAND_PID" ]] && kill "$XWAYLAND_PID" 2>/dev/null || true
  [[ -n "$SHELL_PID" ]] && kill "$SHELL_PID" 2>/dev/null || true
  # gnome-shell leaves ~30 session helpers behind: kill every process of this runtime dir.
  for environ in /proc/[0-9]*/environ; do
    if tr '\0' '\n' 2>/dev/null <"$environ" | grep -qx "XDG_RUNTIME_DIR=$RUN_DIR"; then
      pid="${environ#/proc/}"; kill "${pid%/environ}" 2>/dev/null || true
    fi
  done
}
if [[ "$KEEP_DISPLAY" == 0 ]]; then
  trap stop_display EXIT
fi
if [[ ! -S "/tmp/.X11-unix/X${DISPLAY_NUM}" ]]; then
  mkdir -p "$RUN_DIR"; chmod 700 "$RUN_DIR"
  XDG_RUNTIME_DIR="$RUN_DIR" dbus-run-session -- gnome-shell --headless --wayland \
    --virtual-monitor 1920x1080 --wayland-display "pjperf${DISPLAY_NUM}" >"$RUN_DIR/shell.log" 2>&1 &
  SHELL_PID=$!
  for _ in $(seq 60); do [[ -S "$RUN_DIR/pjperf${DISPLAY_NUM}" ]] && break; sleep 0.5; done
  sleep 3
  env -u DISPLAY XDG_RUNTIME_DIR="$RUN_DIR" WAYLAND_DISPLAY="pjperf${DISPLAY_NUM}" \
    Xwayland "$DISPLAY_ID" -geometry 1920x1080 -noreset >"$RUN_DIR/xwayland.log" 2>&1 &
  XWAYLAND_PID=$!
  for _ in $(seq 40); do [[ -S "/tmp/.X11-unix/X${DISPLAY_NUM}" ]] && break; sleep 0.5; done
  [[ -S "/tmp/.X11-unix/X${DISPLAY_NUM}" ]] || { echo "Xwayland did not start (see $RUN_DIR)" >&2; exit 1; }
  sleep 2
fi

# --- the cases -------------------------------------------------------------------
# One "<config> <case> <command...>" per line; configs: gl (GL tree), rhi_gl and
# rhi_vk (QRhi tree on OpenGL / Vulkan). Plot rows compare the RHI canvas with
# Qwt's GL canvas (config gl), both from the QRhi tree.
CASES=()
add() { CASES+=("$*"); }
scene_configs=(gl rhi_gl)
[[ "$VULKAN" == 1 ]] && scene_configs+=(rhi_vk)
for c in CloudStatic CloudStream CubesAndLinesOrbit MarkersPanFollow RobotTfAnimated Layers10 Layers100 \
         Layers1000 DockMovesCloud DockMovesRobot FirstShow; do
  for cfg in "${scene_configs[@]}"; do
    exe="$RHI_S3D"; [[ "$cfg" == gl ]] && exe="$GL_S3D"
    add "$cfg" "$c" "$exe" "--gtest_filter=Scene3dFrameBenchmark.$c"
  done
done
for cfg in "${scene_configs[@]}"; do  # the 10M cloud's GPU memory at DPR 2
  exe="$RHI_S3D"; [[ "$cfg" == gl ]] && exe="$GL_S3D"
  add "$cfg" CloudStaticDpr2 env QT_SCALE_FACTOR=2 "$exe" --gtest_filter=Scene3dFrameBenchmark.CloudStatic
done
for p in dashed many_curves tracker_only streaming static_1m static_1m_dpr2; do
  for cfg in "${scene_configs[@]}"; do
    canvas=rhi; [[ "$cfg" == gl ]] && canvas=gl
    if [[ "$p" == static_1m_dpr2 ]]; then
      add "$cfg" "plot_$p" env QT_SCALE_FACTOR=2 "$RHI_PLOT" --canvas "$canvas" --case static_1m --frames "$FRAMES"
    else
      add "$cfg" "plot_$p" "$RHI_PLOT" --canvas "$canvas" --case "$p" --frames "$FRAMES"
    fi
  done
done
for cfg in "${scene_configs[@]}"; do
  exe="$RHI_MIXED"; [[ "$cfg" == gl ]] && exe="$GL_MIXED"
  add "$cfg" mixed "$exe" --frames "$FRAMES"
  # An ADS dock-area tab switch of a 9.9M-point cloud (215^3) and a plot.
  add "$cfg" tab_switch "$exe" --plots 1 --cloud-side 215 --tab-switch 10
done

run_env=(env -u WAYLAND_DISPLAY DISPLAY="$DISPLAY_ID" QT_QPA_PLATFORM=xcb vblank_mode=0
         MESA_VK_WSI_PRESENT_MODE=immediate PJ_S3D_BENCH_FULL=1 PJ_S3D_BENCH_FRAMES="$FRAMES"
         PJ_REQUIRE_GPU_TESTS=1)

{
  echo "date: $(date -Is)"
  echo "host: $(uname -r); $(nproc) CPUs; governor $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
  echo "power: $(cat /sys/class/power_supply/AC*/online 2>/dev/null | head -n 1 || echo n/a) (1 = AC)"
  echo "load: $(cat /proc/loadavg)"
  echo "rhi tree: $RHI_TREE ($(git -C "$RHI_TREE/.." rev-parse --short HEAD 2>/dev/null || echo ?))"
  echo "gl tree: $GL_TREE"
  echo "display: $DISPLAY_ID; rounds $ROUNDS; frames $FRAMES"
} >>"$OUT/meta.txt"
tail -n 7 "$OUT/meta.txt"

total=0
for round in $(seq "$FIRST_ROUND" $((FIRST_ROUND + ROUNDS - 1))); do
  mkdir -p "$OUT/raw/$round"
  for line in "${CASES[@]}"; do
    read -r cfg name cmd <<<"$line"
    [[ -z "$ONLY" || "${cfg}__${name}" =~ $ONLY ]] || continue
    api=opengl; [[ "$cfg" == rhi_vk ]] && api=vulkan
    log="$OUT/raw/$round/${cfg}__${name}.log"
    # shellcheck disable=SC2086  # $cmd is a word list built above
    status=0
    timeout 600 "${run_env[@]}" PJ_GRAPHICS_API="$api" $cmd >"$log" 2>&1 || status=$?
    echo "GATE_RUN_EXIT=$status" >>"$log"  # gate_a_report.py: a non-zero run is a failed measurement
    if [[ "$status" != 0 ]]; then
      echo "round $round: $cfg $name FAILED (exit $status, see $log)"
    fi
    total=$((total + 1))
  done
  echo "round $round done ($total runs, load $(cut -d' ' -f1-3 /proc/loadavg))"
done

python3 "$ROOT/scripts/perf/gate_a_report.py" "$OUT"
