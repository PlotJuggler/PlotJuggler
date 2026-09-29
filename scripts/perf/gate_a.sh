#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# Gate A performance run (pj_scene3D/docs/TECHNICAL_NOTES.md §9): the frame
# benchmarks of a candidate tree against a frozen baseline, B's method. Builds
# nothing.
#
#   scripts/perf/gate_a.sh --candidate build [--baseline TREE | --baseline-run DIR]
#       [--out DIR] [--rounds 5] [--frames 30] [--display :96] [--only REGEX]
#       [--no-vulkan] [--keep-display] [--first-round K]
#
# --candidate: the tree under test, with scene3d_frame_benchmark,
#   plot_canvas_benchmark and mixed_layout_benchmark built (PJ_BUILD_DEMOS=ON);
#   measured on OpenGL (config rhi_gl, the verdicts) and Vulkan (rhi_vk).
# --baseline: a baseline tree with the same three benchmarks, measured on OpenGL
#   (config base) interleaved with the candidate in every round: the least noisy
#   comparison.
# --baseline-run: instead, an earlier gate_a.sh run directory whose rhi_gl runs
#   are the frozen baseline (e.g. the final P6 run); only the candidate runs.
# Neither: the report compares nothing and gates only the absolute limits.
#
# Method: a private headless GNOME Shell with a rootful Xwayland on --display
# (xcb: the Wayland QPA pins every frame to the compositor's 60 Hz callback),
# vsync off (vblank_mode=0, MESA_VK_WSI_PRESENT_MODE=immediate), every case in
# its own process, the configurations interleaved within each of --rounds
# rounds. Every run's output lands in DIR/raw/<round>/<config>__<case>.log,
# ending with its exit status (GATE_RUN_EXIT=<n>);
# gate_a_report.py DIR [--baseline RUN] then writes DIR/gate_a.md (median
# [min–max] of the rounds, Gate A verdicts against the baseline). --only keeps
# the cases whose "<config>__<case>" matches the regex; --no-vulkan drops rhi_vk.
# --first-round K numbers the rounds from K, so one --out can collect rounds run
# one at a time (each under the build lock, letting builds run in between).
#
# Hold the machine's build lock for the whole run (no compile may add noise),
# e.g. `scripts/perf/build_slot.sh scripts/perf/gate_a.sh ...`. Needs gnome-shell,
# Xwayland and dbus-run-session; the GPU is whatever Mesa picks (check the
# report's device column).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CAND_TREE="" BASE_TREE="" BASE_RUN="" OUT="" FIRST_ROUND=1 ROUNDS=5 FRAMES=30 DISPLAY_ID=":96" ONLY="" VULKAN=1 KEEP_DISPLAY=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --candidate) CAND_TREE="$(realpath "$2")"; shift 2 ;;
    --baseline) BASE_TREE="$(realpath "$2")"; shift 2 ;;
    --baseline-run) BASE_RUN="$(realpath "$2")"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --rounds) ROUNDS="$2"; shift 2 ;;
    --first-round) FIRST_ROUND="$2"; shift 2 ;;
    --frames) FRAMES="$2"; shift 2 ;;
    --display) DISPLAY_ID="$2"; shift 2 ;;
    --only) ONLY="$2"; shift 2 ;;
    --no-vulkan) VULKAN=0; shift ;;
    --keep-display) KEEP_DISPLAY=1; shift ;;
    -h|--help) sed -n '2,36p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$CAND_TREE" ]] || { echo "--candidate is required" >&2; exit 2; }
[[ -z "$BASE_TREE" || -z "$BASE_RUN" ]] || { echo "--baseline and --baseline-run are exclusive" >&2; exit 2; }
[[ -z "$BASE_RUN" || -d "$BASE_RUN/raw" ]] || { echo "not a gate_a.sh run directory: $BASE_RUN" >&2; exit 2; }
OUT="${OUT:-$ROOT/build/gate_a_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$OUT/raw"

bin() {  # bin <tree> <name>: the built executable, or empty
  find "$1" -type f -name "$2" -perm -u+x 2>/dev/null | head -n 1
}
declare -A S3D PLOT MIXED  # per config: the executables of its tree
configs=(rhi_gl)
[[ "$VULKAN" == 1 ]] && configs+=(rhi_vk)
[[ -n "$BASE_TREE" ]] && configs=(base "${configs[@]}")
for cfg in "${configs[@]}"; do
  tree="$CAND_TREE"; [[ "$cfg" == base ]] && tree="$BASE_TREE"
  S3D[$cfg]="$(bin "$tree" scene3d_frame_benchmark)"
  PLOT[$cfg]="$(bin "$tree" plot_canvas_benchmark)"
  MIXED[$cfg]="$(bin "$tree" mixed_layout_benchmark)"
  for exe in "${S3D[$cfg]}" "${PLOT[$cfg]}" "${MIXED[$cfg]}"; do
    [[ -n "$exe" ]] || { echo "$tree lacks a benchmark (build them with PJ_BUILD_DEMOS=ON)" >&2; exit 2; }
  done
done

# --- the display: headless GNOME Shell + rootful Xwayland ----------------------
DISPLAY_NUM="${DISPLAY_ID#:}"
RUN_DIR="/tmp/pjx${DISPLAY_NUM}"
source "$ROOT/scripts/perf/headless_display.sh"
if [[ "$KEEP_DISPLAY" == 0 ]]; then
  trap 'stop_display "$RUN_DIR"' EXIT
fi
start_display "$DISPLAY_NUM" "$RUN_DIR" || [[ $? == 3 ]] || exit 1  # 3 = reused, on purpose

# --- the cases -------------------------------------------------------------------
# One "<config> <case> <command...>" per line; configs: base (the baseline tree on
# OpenGL), rhi_gl and rhi_vk (the candidate on OpenGL / Vulkan).
CASES=()
add() { CASES+=("$*"); }
for c in CloudStatic CloudStream CubesAndLinesOrbit MarkersPanFollow RobotTfAnimated Layers10 Layers100 \
         Layers1000 DockMovesCloud DockMovesRobot FirstShow; do
  for cfg in "${configs[@]}"; do
    add "$cfg" "$c" "${S3D[$cfg]}" "--gtest_filter=Scene3dFrameBenchmark.$c"
  done
done
for cfg in "${configs[@]}"; do  # the 10M cloud's GPU memory at DPR 2
  add "$cfg" CloudStaticDpr2 env QT_SCALE_FACTOR=2 "${S3D[$cfg]}" --gtest_filter=Scene3dFrameBenchmark.CloudStatic
done
for p in dashed many_curves tracker_only streaming static_1m static_1m_dpr2; do
  for cfg in "${configs[@]}"; do
    if [[ "$p" == static_1m_dpr2 ]]; then
      add "$cfg" "plot_$p" env QT_SCALE_FACTOR=2 "${PLOT[$cfg]}" --canvas rhi --case static_1m --frames "$FRAMES"
    else
      add "$cfg" "plot_$p" "${PLOT[$cfg]}" --canvas rhi --case "$p" --frames "$FRAMES"
    fi
  done
done
for cfg in "${configs[@]}"; do
  add "$cfg" mixed "${MIXED[$cfg]}" --frames "$FRAMES"
  # An ADS dock-area tab switch of a 9.9M-point cloud (215^3) and a plot.
  add "$cfg" tab_switch "${MIXED[$cfg]}" --plots 1 --cloud-side 215 --tab-switch 10
done

run_env=(env -u WAYLAND_DISPLAY DISPLAY="$DISPLAY_ID" QT_QPA_PLATFORM=xcb vblank_mode=0
         MESA_VK_WSI_PRESENT_MODE=immediate PJ_S3D_BENCH_FULL=1 PJ_S3D_BENCH_FRAMES="$FRAMES"
         PJ_REQUIRE_GPU_TESTS=1)

{
  echo "date: $(date -Is)"
  echo "host: $(uname -r); $(nproc) CPUs; governor $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
  echo "power: $(cat /sys/class/power_supply/AC*/online 2>/dev/null | head -n 1 || echo n/a) (1 = AC)"
  echo "load: $(cat /proc/loadavg)"
  echo "candidate tree: $CAND_TREE ($(git -C "$CAND_TREE/.." rev-parse --short HEAD 2>/dev/null || echo ?))"
  if [[ -n "$BASE_TREE" ]]; then
    echo "baseline tree: $BASE_TREE ($(git -C "$BASE_TREE/.." rev-parse --short HEAD 2>/dev/null || echo ?))"
  elif [[ -n "$BASE_RUN" ]]; then
    echo "baseline run: $BASE_RUN"
  else
    echo "baseline: none"
  fi
  echo "display: $DISPLAY_ID; rounds $ROUNDS; frames $FRAMES"
} >>"$OUT/meta.txt"
tail -n 8 "$OUT/meta.txt"

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

python3 "$ROOT/scripts/perf/gate_a_report.py" "$OUT" ${BASE_RUN:+--baseline "$BASE_RUN"}
