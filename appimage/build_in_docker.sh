#!/usr/bin/env bash
# Build the PlotJuggler AppImage inside an Ubuntu 22.04 container so the
# embedded Python ABI (3.10) and Qt5 versions match the GitHub release CI.
#
# Usage:
#   ./appimage/build_in_docker.sh                # full build
#   REBUILD_IMAGE=1 ./appimage/build_in_docker.sh # rebuild the docker image
#
# Output:
#   ./PlotJuggler-<arch>.AppImage at the repo root.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_TAG="plotjuggler-appimage-builder:jammy"

cd "$REPO_ROOT"

if [[ "${REBUILD_IMAGE:-0}" == "1" ]] || ! docker image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
    echo "==> Building docker image $IMAGE_TAG"
    docker build -t "$IMAGE_TAG" -f appimage/Dockerfile appimage/
fi

echo "==> Running build inside container"
# linuxdeploy needs FUSE; --privileged is the simplest way to grant it.
# --user keeps the build outputs (build-appimage/, AppDir/, the AppImage) owned
# by the host user instead of root, so subsequent host commands and `git status`
# behave normally. HOME is redirected to the (writable) ccache dir so any tools
# that touch ~/.cache during the build don't try to write into /root.
docker run --rm --privileged \
    --user "$(id -u):$(id -g)" \
    -v "$REPO_ROOT:/work" \
    -w /work \
    -e HOME=/work/.ccache-docker \
    -e CCACHE_DIR=/work/.ccache-docker \
    "$IMAGE_TAG" \
    bash appimage/build_appimage.sh
