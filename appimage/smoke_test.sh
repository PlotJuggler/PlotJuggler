#!/bin/sh
#
# Run the packaged AppImage on a machine that did not build it.
#
# The release build's own --selftest-python step runs inside the builder
# container, where every -dev package is installed and a missing runtime
# dependency cannot surface. This script exists to execute the artifact
# somewhere bare instead.
#
# It assumes the runtime libraries the payload needs are already present:
# release CI runs it in the same container right after deb/smoke_test.sh,
# whose `apt install` resolved the .deb's Depends — which doubles as proof
# that the set declared there covers the payload both artifacts share. For a
# standalone run, install those packages (see `Depends:` in the .deb, or
# SONAME_PKG in deb/build_deb.sh) plus xvfb first.
#
#   sh appimage/smoke_test.sh <path/to/PlotJuggler-*.AppImage>
set -e

# This automation deliberately launches the real GUI. Suppress its anonymous
# launch ping even when the caller does not forward CI into the container.
export PJ_DISABLE_TELEMETRY=1

APPIMAGE_PATH="${1:?usage: smoke_test.sh <path/to/PlotJuggler-*.AppImage>}"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT HUP INT TERM

# The artifact arrives via actions/download-artifact, which zips away the
# execute bit, and CI mounts it read-only — stage a runnable copy.
cp "${APPIMAGE_PATH}" "${WORK}/pj4.AppImage"
chmod +x "${WORK}/pj4.AppImage"

# Containers have no FUSE; run from a plain extraction instead of a mount.
export APPIMAGE_EXTRACT_AND_RUN=1

# Display-free by design: main() short-circuits this before any QApplication
# exists, and it proves the AppRun PYTHONHOME override reaches the bundled
# CPython stdlib.
echo "--- selftest-python (AppImage) ---"
"${WORK}/pj4.AppImage" --selftest-python

# The bundled Qt ships only the xcb platform plugin, so constructing a
# QApplication needs a real X server; Xvfb provides one without a GPU.
echo "--- GUI startup under Xvfb (AppImage) ---"
command -v xvfb-run >/dev/null || { echo "FAIL: xvfb-run missing (apt install xvfb)"; exit 1; }
set +e
xvfb-run -a --server-args="-screen 0 1280x800x24" \
  timeout 25 "${WORK}/pj4.AppImage" --test-data --nosplash >"${WORK}/gui.log" 2>&1
rc=$?
set -e
# 124 is timeout(1) reporting that it had to kill a still-healthy process.
if [ "$rc" = "124" ]; then
  echo "OK: ran 25s with a real window under Xvfb"
else
  echo "FAIL: exited early with rc=$rc"
  tail -60 "${WORK}/gui.log"
  exit 1
fi
