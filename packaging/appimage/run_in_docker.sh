#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Verify the PlotJuggler 4 AppImage runs on a clean Ubuntu 22.04 (no Qt, no build
# tools, no app deps) using only the libraries the AppImage bundles plus the
# universal X11/GL runtime libs in packaging/appimage/Dockerfile.run. The host X server is
# shared via xhost so the GUI appears on your screen.
#
# Usage:
#   packaging/appimage/run_in_docker.sh [APP_ARGS...]   # launch the bundled GUI (args go to the app)
#   PJ_APPIMAGE=<path> packaging/appimage/run_in_docker.sh ...   # run a specific AppImage
#   REBUILD_IMAGE=1 packaging/appimage/run_in_docker.sh ...      # force-rebuild the runner image
#   packaging/appimage/run_in_docker.sh -h | --help     # show this help and exit
#
# All arguments are forwarded verbatim to the AppImage. Requires an X server on the
# host ($DISPLAY) and the `xhost` tool.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
source "${REPO_ROOT}/versions.env"
IMAGE_TAG="pj4-appimage-runner:jammy"

usage() {
  cat <<'EOF'
Run the PlotJuggler 4 AppImage on a clean Ubuntu 22.04 runtime image (only base
X11/GL libs — no Qt, build tools, or app deps) with the host X server shared via
xhost, so the GUI appears on your display.

Usage:
  packaging/appimage/run_in_docker.sh [APP_ARGS...]            # launch the GUI (args go to the app)
  PJ_APPIMAGE=<path> packaging/appimage/run_in_docker.sh ...   # run a specific AppImage
  REBUILD_IMAGE=1 packaging/appimage/run_in_docker.sh ...      # force-rebuild the runner image
  packaging/appimage/run_in_docker.sh -h | --help              # show this help and exit

Requires an X server on the host ($DISPLAY) and the `xhost` tool.
EOF
}

# Explicit help: handle -h/--help before resolving the AppImage or touching docker.
for arg in "$@"; do
  case "${arg}" in -h | --help) usage; exit 0 ;; esac
done

# Resolve the AppImage to test: PJ_APPIMAGE override, else the newest built one.
APPIMAGE="${PJ_APPIMAGE:-}"
if [[ -z "${APPIMAGE}" ]]; then
  APPIMAGE="$(ls -t "${SCRIPT_DIR}"/PlotJuggler-*-"${PJ_APPIMAGE_ARCH}".AppImage 2>/dev/null | head -1 || true)"
fi
[[ -n "${APPIMAGE}" && -f "${APPIMAGE}" ]] || {
  echo "ERROR: no AppImage found. Build one first: packaging/appimage/build_in_docker.sh"; exit 1; }
APPIMAGE="$(cd "$(dirname "${APPIMAGE}")" && pwd)/$(basename "${APPIMAGE}")"  # absolute

command -v docker >/dev/null || { echo "docker required"; exit 1; }
command -v xhost  >/dev/null || { echo "xhost required (x11-xserver-utils)"; exit 1; }
: "${DISPLAY:?DISPLAY is not set — need a running X server on the host}"

export DOCKER_BUILDKIT=1
# Build the clean runner image (no build context needed: Dockerfile.run COPYs
# nothing, so feed it on stdin and send an empty context).
if [[ "${REBUILD_IMAGE:-0}" == "1" ]] || ! docker image inspect "${IMAGE_TAG}" >/dev/null 2>&1; then
  echo "==> Building clean runner image ${IMAGE_TAG}"
  docker build -t "${IMAGE_TAG}" - < "${SCRIPT_DIR}/Dockerfile.run"
fi

# Share the host X server with the container's root user, and revoke on exit.
xhost +SI:localuser:root >/dev/null
cleanup() { xhost -SI:localuser:root >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "==> Running ${APPIMAGE##*/} on clean ${IMAGE_TAG} (DISPLAY=${DISPLAY})"
docker run --rm \
  -e DISPLAY="${DISPLAY}" \
  -e QT_X11_NO_MITSHM=1 \
  -e QT_IM_MODULE= \
  -v /tmp/.X11-unix:/tmp/.X11-unix:ro \
  -v "${APPIMAGE}:/opt/app.AppImage:ro" \
  "${IMAGE_TAG}" \
  /opt/app.AppImage --appimage-extract-and-run "$@"
