#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# The full local pre-push gate of this checkout, in parallel.
#
#   scripts/perf/gate_par.sh OUT [--wasm] [--pw-workers N] [--baseline DUMPDIR]
#       [--no-build] [--check]
#
# Compiles run once, under the build slot (build_slot.sh); then every test step
# runs at the same time OUTSIDE the slot, each display step on its own private
# headless GNOME Shell + rootful Xwayland (:61 and :62 by default), so covered windows
# never starve each other of frame callbacks:
#   offscreen  ./test.sh
#   d1         -L qrhi-opengl (incl. dpr2), then the opengl + opengl_dpr2 pixel dumps
#   d2         -L qrhi-vulkan, then the vulkan pixel dump
#   wasm       Playwright on the packaged browser build (--wasm), the timing- and
#              hover-sensitive specs serially after everything else
# The pixel dump (harness, point-cloud goldens, benchmark final frames) lands in
# OUT/dump; with --baseline it is compared by pxcmp.py, and OUT/dump is the next
# run's baseline. Summary: OUT/summary.txt; every step's exit status lands in
# OUT/status_<job> and the script exits non-zero (after the summary) if any step
# failed. --check resolves and verifies every input path, prints them and exits.
#
# Trees: build/ (the default ./build.sh tree) and build-wasm-probe/ (configured
# here, --wasm).
# Environment:
#   PJ_GATE_DISPLAY_BASE  first private display number (default 61; the second is
#                    base+1). A live display this script did not start is never
#                    reused: pick another base.
#   PJ_BUILD_JOBS    compile parallelism (default: nproc)
#   PJ_BUILD_LOCK    the build-slot lock file (see build_slot.sh)
#   PJ_EMSDK_DIR     the emsdk checkout (default: $EMSDK, else .emsdk, as wasm-ci.yml)
#   PJ_QT_WASM_ROOT  the directory holding <PJ_QT_VERSION>/wasm_multithread and
#                    <PJ_QT_VERSION>/gcc_64 (default: .qt-wasm, as wasm-ci.yml)
# --wasm needs tests/wasm/node_modules: `npm ci` there, or a worktree borrows the
# primary checkout's by symlink for the run.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/scripts/perf"
source "$ROOT/versions.env"
[[ $# -ge 1 && $1 != -* ]] || { sed -n '3,36p' "$0"; exit 2; }
OUT=$1; shift
WASM=0 PW=4 BASE="" BUILD=1 CHECK=0
while [[ $# -gt 0 ]]; do
  case $1 in
    --wasm) WASM=1; shift ;; --pw-workers) PW=$2; shift 2 ;;
    --baseline) BASE=$(realpath "$2"); shift 2 ;; --no-build) BUILD=0; shift ;;
    --check) CHECK=1; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
JOBS=${PJ_BUILD_JOBS:-$(nproc)}
EMSDK_DIR=${PJ_EMSDK_DIR:-${EMSDK:-$ROOT/.emsdk}}
QT_WASM_ROOT=${PJ_QT_WASM_ROOT:-$ROOT/.qt-wasm}
QT_WASM=$QT_WASM_ROOT/$PJ_QT_VERSION/wasm_multithread
QT_HOST=$QT_WASM_ROOT/$PJ_QT_VERSION/gcc_64
MAIN_REPO="$(dirname "$(cd "$ROOT" && cd "$(git rev-parse --git-common-dir)" && pwd)")"
NODE_MODULES=$ROOT/tests/wasm/node_modules
[[ -e $NODE_MODULES ]] || NODE_MODULES=$MAIN_REPO/tests/wasm/node_modules

# --- preflight: fail now, not twenty minutes in ---------------------------------
missing=()
need() { [[ -e $1 ]] || missing+=("$1 ($2)"); }
need "$ROOT/build/CTestTestfile.cmake" "configure build/ with ./build.sh"
[[ -n $BASE ]] && need "$BASE" "--baseline"
if [[ $WASM == 1 ]]; then
  need "$EMSDK_DIR/emsdk_env.sh" "PJ_EMSDK_DIR"
  need "$QT_WASM/lib/cmake/Qt6/qt.toolchain.cmake" "PJ_QT_WASM_ROOT"
  need "$QT_HOST/bin" "PJ_QT_WASM_ROOT host kit"
  need "$NODE_MODULES" "npm ci in tests/wasm"
fi
CCACHE_ARGS=""  # optional: the wasm configure uses it as compiler launcher when present
if command -v ccache >/dev/null; then
  CCACHE_ARGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
fi
if [[ $CHECK == 1 || ${#missing[@]} -gt 0 ]]; then
  printf '%s\n' "root: $ROOT" "out: $OUT" "jobs: $JOBS" "build lock: ${PJ_BUILD_LOCK:-${XDG_RUNTIME_DIR:-/tmp}/pj_build.lock}" \
    "emsdk: $EMSDK_DIR" "qt wasm: $QT_WASM" "qt host: $QT_HOST" "node_modules: $NODE_MODULES"
  [[ ${#missing[@]} -eq 0 ]] || { printf 'missing: %s\n' "${missing[@]}" >&2; exit 2; }
  echo "all inputs present"; exit 0
fi

mkdir -p "$OUT"; OUT=$(realpath "$OUT"); cd "$ROOT"

# --- compiles: one slot hold ---------------------------------------------------
if [[ $BUILD == 1 ]]; then
  cat > "$OUT/build.sh" <<EOF
#!/bin/bash
cd "$ROOT"
cmake --build build -j$JOBS > "$OUT/build_rhi.log" 2>&1 || { echo "build FAILED"; exit 1; }
if [[ $WASM == 1 ]]; then
  source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1
  embuilder build zlib > "$OUT/wasm_embuilder.log" 2>&1 || { echo "embuilder FAILED"; exit 1; }
  cmake -S . -B build-wasm-probe -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$QT_WASM/lib/cmake/Qt6/qt.toolchain.cmake" \
    -DQT_HOST_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=Release \
    $CCACHE_ARGS \
    -DPJ_BUILD_TESTS=OFF -DPJ_BUILD_DEMOS=OFF -DPJ_WASM_ENABLE_INGRESS_PROBE=ON \
    -DPJ_WASM_SCENE3D_CAPABILITY_PROBE=ON -DPJ_WASM_WITH_SCENE2D=ON -DPJ_WASM_WITH_SCENE3D=ON > "$OUT/wasm_configure.log" 2>&1 || { echo "wasm configure FAILED"; exit 1; }
  cmake --build build-wasm-probe --target pj_app pj_scene3d_wasm_capability_probe --parallel $JOBS > "$OUT/wasm_build.log" 2>&1 || { echo "wasm build FAILED"; exit 1; }
  rm -rf build-wasm-probe/deploy
  node scripts/package_wasm.mjs --source build-wasm-probe/pj_app --output build-wasm-probe/deploy > "$OUT/wasm_package.log" 2>&1 || { echo "wasm package FAILED"; exit 1; }
fi
EOF
  chmod +x "$OUT/build.sh"
  t0=$(date +%s)
  "$HERE/build_slot.sh" "$OUT/build.sh" || exit 1
  echo "builds: $(( $(date +%s) - t0 )) s" > "$OUT/summary.txt"
else
  : > "$OUT/summary.txt"
fi

# --- displays -------------------------------------------------------------------
DBASE=${PJ_GATE_DISPLAY_BASE:-61}
declare -A XNUM=([d1]=$DBASE [d2]=$((DBASE + 1)))
source "$HERE/headless_display.sh"
names=(d1 d2)
STARTED=() SERVE_PID="" LINKED_NODE_MODULES=0
cleanup() {
  local n
  for n in "${STARTED[@]}"; do stop_display "/tmp/pjx${XNUM[$n]}"; done
  [[ -n $SERVE_PID ]] && kill "$SERVE_PID" 2>/dev/null
  [[ $LINKED_NODE_MODULES == 1 ]] && rm -f "$ROOT/tests/wasm/node_modules"
  return 0
}
trap cleanup EXIT
for n in "${names[@]}"; do
  rc=0; start_display "${XNUM[$n]}" "/tmp/pjx${XNUM[$n]}" || rc=$?
  if [[ $rc == 3 ]]; then
    echo "display :${XNUM[$n]} is live and was not started by this run; refusing to reuse it (set PJ_GATE_DISPLAY_BASE to another number)" >&2
    exit 2
  fi
  STARTED+=("$n")  # ours from here on, even when it failed half-way
  [[ $rc == 0 ]] || exit 1
done
X() { env -u WAYLAND_DISPLAY DISPLAY=":${XNUM[$1]}" QT_QPA_PLATFORM=xcb PJ_REQUIRE_GPU_TESTS=1 "${@:2}"; }
# The qrhi lanes log each view's backend (pj.graphics, shown by -V) for check_qrhi_backend.sh.
XL() { X "$1" QT_LOGGING_RULES=pj.graphics.info=true QT_FORCE_STDERR_LOGGING=1 "${@:2}"; }
st_() { echo "$1 $2" >> "$st"; }  # st_ <step> <exit>: one line of the running job's status file
res() { grep -hE "tests passed|\*\*\*Failed|Failed  |Timeout|Not Run|Exception" "$1" | grep -v "^ *Start" | head -20; }

dump() {  # dump <display> <backend> <dir>: harness, goldens and benchmark final frames
  local d=$3/$2; mkdir -p "$d/harness" "$d/golden" "$d/bench"
  X "$1" PJ_RHI_TEST_DUMP_DIR="$d/harness" ctest --test-dir build -j1 --timeout 300 \
    -R "^(scene3d_rhi_render_test|scene3d_rhi_point_test|plot_rhi_render_test)\.$2\." > "$d/harness.log" 2>&1
  st_ "dump_harness_$2" $?
  [[ $2 == opengl_dpr2 ]] && return
  X "$1" PJ_POINTCLOUD_GOLDEN_OUT="$d/golden" PJ_POINTCLOUD_GOLDEN_DUMP_ALL=1 ctest --test-dir build -j1 --timeout 300 \
    -R "^pointcloud_golden_test\.$2\." > "$d/golden.log" 2>&1
  st_ "dump_golden_$2" $?
  X "$1" PJ_S3D_BENCH_DUMP_DIR="$d/bench" ctest --test-dir build -j1 --timeout 300 \
    -R "^scene3d_frame_benchmark\.$2\." > "$d/bench.log" 2>&1
  st_ "dump_bench_$2" $?
}

t1=$(date +%s)
rm -rf "$OUT/dump" "$OUT"/status_*
# gtest discovery runs lazily inside ctest; parallel ctest jobs on freshly rebuilt
# runners race on it and write every test twice. Discover once, alone, first.
ctest --test-dir build -N > /dev/null 2>&1
( st=$OUT/status_offscreen
  ./test.sh -j8 --timeout 600 > "$OUT/offscreen.log" 2>&1; st_ ctest_offscreen $?
  echo "offscreen done $(( $(date +%s) - t1 )) s" ) > "$OUT/j_offscreen.out" 2>&1 &
JOBS_PIDS=($!)
( st=$OUT/status_d1
  XL d1 ctest --test-dir build -j1 --timeout 600 -L '^qrhi-opengl$' -V > "$OUT/qrhi_opengl.log" 2>&1; st_ ctest_opengl $?
  dump d1 opengl "$OUT/dump"; dump d1 opengl_dpr2 "$OUT/dump"
  echo "d1 done $(( $(date +%s) - t1 )) s" ) > "$OUT/j_d1.out" 2>&1 &
JOBS_PIDS+=($!)
( st=$OUT/status_d2
  XL d2 ctest --test-dir build -j1 --timeout 600 -L '^qrhi-vulkan$' -V > "$OUT/qrhi_vulkan.log" 2>&1; st_ ctest_vulkan $?
  dump d2 vulkan "$OUT/dump"
  echo "d2 done $(( $(date +%s) - t1 )) s" ) > "$OUT/j_d2.out" 2>&1 &
JOBS_PIDS+=($!)
# Playwright: specs with main-thread timing or hover-pixel checks flake when they
# share the machine, so they run serially after everything else has finished.
PW_SERIAL="editors.spec.js plotting.spec.js"
if [[ $WASM == 1 ]]; then
  port=$((8700 + RANDOM % 90))
  if [[ ! -e tests/wasm/node_modules ]]; then LINKED_NODE_MODULES=1; ln -s "$NODE_MODULES" tests/wasm/node_modules; fi
  node scripts/serve_wasm.mjs --root build-wasm-probe/deploy --port $port > "$OUT/wasm_serve.log" 2>&1 &
  SERVE_PID=$!
  for _ in $(seq 50); do curl -fsS -o /dev/null "http://127.0.0.1:$port/" 2>/dev/null && break; sleep 0.2; done
  specs=$(cd tests/wasm && node -p "require('./playwright.config.js').testMatch.join(' ')")
  PW_PARALLEL=""; for f in $specs; do [[ " $PW_SERIAL " == *" $f "* ]] || PW_PARALLEL+=" $f"; done
  # shellcheck disable=SC2086  # the spec lists are word lists
  ( st=$OUT/status_wasm
    cd tests/wasm && PJ_WASM_URL="http://127.0.0.1:$port/" PJ_WASM_BUILD_DIR=build-wasm-probe \
      npx playwright test $PW_PARALLEL --project=chromium --reporter=line --workers="$PW" > "$OUT/playwright.log" 2>&1
    rc=$?; st_ playwright_parallel $rc; echo "playwright (parallel) exit $rc"
    echo "wasm parallel done $(( $(date +%s) - t1 )) s" ) > "$OUT/j_wasm.out" 2>&1 &
  JOBS_PIDS+=($!)
fi
wait "${JOBS_PIDS[@]}"
if [[ $WASM == 1 ]]; then
  # shellcheck disable=SC2086
  ( st=$OUT/status_wasm
    cd tests/wasm && PJ_WASM_URL="http://127.0.0.1:$port/" PJ_WASM_BUILD_DIR=build-wasm-probe \
      npx playwright test $PW_SERIAL --project=chromium --reporter=line --workers=1 > "$OUT/playwright_serial.log" 2>&1
    rc=$?; st_ playwright_serial $rc; echo "playwright (serial: $PW_SERIAL) exit $rc" ) >> "$OUT/j_wasm.out" 2>&1
fi
st=$OUT/status_summary
# the pixel comparison runs first so its status is not lost behind the summary pipes
# the perf HUD image shows live CPU/GPU timings, so it differs on every run
if [[ -n $BASE ]]; then python3 "$HERE/pxcmp.py" "$BASE" "$OUT/dump" --ignore '*PerfHud_hud.png' > "$OUT/pxcmp.txt" 2>&1; st_ pxcmp $?; fi

{
  echo "tests (parallel): $(( $(date +%s) - t1 )) s"
  cat "$OUT"/j_*.out
  for f in offscreen qrhi_opengl qrhi_vulkan; do
    [[ -f $OUT/$f.log ]] && { echo "== $f"; res "$OUT/$f.log"; }
  done
  for lane in opengl:OpenGL vulkan:Vulkan; do  # a lane that fell back to another backend proves nothing
    bash scripts/ci/check_qrhi_backend.sh "$OUT/qrhi_${lane%:*}.log" "${lane#*:}, " > "$OUT/backend_${lane%:*}.txt" 2>&1 \
      && { echo "backend ${lane%:*}: ok (${lane#*:})"; st_ "backend_${lane%:*}" 0; } || { st_ "backend_${lane%:*}" 1; echo "backend ${lane%:*}: FAILED, see $OUT/backend_${lane%:*}.txt"; tail -n 3 "$OUT/backend_${lane%:*}.txt"; }
  done
  echo "dpr2 passed: $(grep -cE 'opengl_dpr2\..*Passed' "$OUT/qrhi_opengl.log") failed: $(grep -cE 'opengl_dpr2\..*(Failed|Timeout)' "$OUT/qrhi_opengl.log")"
  for f in playwright playwright_serial; do
    [[ -f $OUT/$f.log ]] && { echo "== $f"; grep -E "passed|failed|flaky" "$OUT/$f.log" | tail -3; }
  done
  echo "== pixel dump: $(find "$OUT/dump" -name '*.png' | wc -l) images"
  if [[ -n $BASE ]]; then grep -v "^NEW" "$OUT/pxcmp.txt" | tail -40; fi
  FAILED=$(grep -hE ' [1-9][0-9]*$' "$OUT"/status_* 2>/dev/null)
  if [[ -n $FAILED ]]; then echo "== FAILED STEPS"; echo "$FAILED"; else echo "== all steps exited 0"; fi
} >> "$OUT/summary.txt"
cat "$OUT/summary.txt"
grep -qhE ' [1-9][0-9]*$' "$OUT"/status_* 2>/dev/null && exit 1
exit 0
