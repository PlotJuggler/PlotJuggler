#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Build the PlotJuggler4 AppImage inside an Ubuntu 22.04 container, matching
# the glibc/libstdc++ baseline release.yml already publishes releases from —
# independent of whatever the host machine runs. See Dockerfile for why.
#
# Usage:
#   packaging/appimage/build_appimage_in_docker.sh                     # app-only AppImage
#   packaging/appimage/build_appimage_in_docker.sh --plugins-registry  # bundle official plugins
#   packaging/appimage/build_appimage_in_docker.sh --plugins-dir DIR   # bundle a local folder
#
# Arguments are forwarded to packaging/appimage/build_appimage.sh. A --plugins-dir path may
# be relative — it is resolved here against your current directory (and must live
# inside the repo tree, since only the repo is mounted into the container).
#
# This is a packaging build: it configures with PJ_BUILD_TESTS=OFF and
# PJ_BUILD_DEMOS=OFF, so the ~17 GB of static test binaries and the scene3D dev
# demos (neither ships in the AppImage) are never compiled.
#
# The container reuses the host's ./.qt (Qt's official binaries are portable
# across glibc baselines) and packaging/appimage/ (cached linuxdeploy tool downloads,
# final .AppImage output), but builds into its own build-appimage-docker/ tree
# rather than ./build/, so container-toolchain objects never mix with the
# host's own dev build. Conan/ccache caches persist across runs in their own
# host-side directories (bind-mounted, not named Docker volumes: a freshly
# created named volume is root-owned, which --user below can't write into), so
# repeat builds are incremental.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
IMAGE="pj4-appimage-builder:ubuntu22.04"
CONTAINER_BUILD_DIR="${ROOT}/build-appimage-docker"
CONAN_CACHE_DIR="${ROOT}/.conan2-appimage-docker"
CCACHE_DIR="${ROOT}/.ccache-appimage-docker"

mkdir -p "${CONTAINER_BUILD_DIR}" "${CONAN_CACHE_DIR}" "${CCACHE_DIR}"

# Validate/rewrite a --plugins-dir argument on the host BEFORE the (long) build,
# and canonicalize it to an absolute path. build_appimage.sh cd's into packaging/appimage/
# before it consumes this path, so a relative value must be resolved here against
# the host CWD (where the user ran this). Only ${ROOT} is bind-mounted into the
# container, so a folder outside the repo tree is rejected up front — it would be
# invisible inside the container and fail after a full build otherwise.
ARGS=("$@")
for ((i = 0; i < ${#ARGS[@]}; i++)); do
  [[ "${ARGS[i]}" == "--plugins-dir" ]] || continue
  plugins_dir="$(realpath -m -- "${ARGS[i + 1]:?--plugins-dir needs a path}")"
  [[ -d "${plugins_dir}" ]] || { echo "ERROR: --plugins-dir '${ARGS[i + 1]}' (=${plugins_dir}) is not a directory" >&2; exit 1; }
  if [[ "${plugins_dir}/" != "${ROOT}/"* ]]; then
    echo "ERROR: --plugins-dir '${plugins_dir}' is outside the repo tree (${ROOT})." >&2
    echo "       Only the repo is mounted into the build container, so it would be invisible there." >&2
    echo "       Copy the plugins under the repo (e.g. ${ROOT}/appimage_plugins) and retry." >&2
    exit 1
  fi
  ARGS[i + 1]="${plugins_dir}"
  break
done

docker build -t "${IMAGE}" "${SCRIPT_DIR}/docker"

# APPIMAGE_EXTRACT_AND_RUN: linuxdeploy/linuxdeploy-plugin-qt are themselves
# AppImages; containers have no FUSE by default, so run them extracted instead
# (same trick release.yml uses for the same reason on ubuntu-22.04 runners).
docker run --rm \
  --user "$(id -u):$(id -g)" \
  -v "${ROOT}:${ROOT}" \
  -v "${CONTAINER_BUILD_DIR}:${ROOT}/build" \
  -v "${CONAN_CACHE_DIR}:/home/build/.conan2" \
  -v "${CCACHE_DIR}:/home/build/.cache/ccache" \
  -w "${ROOT}" \
  -e APPIMAGE_EXTRACT_AND_RUN=1 \
  -e PJ_BUILD_TESTS=OFF \
  -e PJ_BUILD_DEMOS=OFF \
  "${IMAGE}" \
  bash -c 'conan profile detect --force && ./scripts/install_qt6.sh && ./build.sh && ./packaging/appimage/build_appimage.sh "$@"' bash "${ARGS[@]+"${ARGS[@]}"}"
