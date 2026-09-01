#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Build the PlotJuggler 4 AppImage inside a fully-baked Ubuntu 22.04 container.
#
# The builder image (tagged with the Qt/arch pins from versions.env) bakes Qt,
# the entire Conan dependency closure, and a pinned linuxdeploy into its layers, so running the
# container fetches nothing — except, with --plugins-registry, the published plugin
# zips. Mirrors PJ3's appimage/build_in_docker.sh, adapted to PJ4:
#   * no --privileged (linuxdeploy runs extracted via APPIMAGE_EXTRACT_AND_RUN);
#   * Qt + Conan are baked, not installed from system packages each run.
#
# Usage:
#   appimage/build_in_docker.sh                                # app-only AppImage
#   appimage/build_in_docker.sh --plugins-registry             # bundle the official set
#   appimage/build_in_docker.sh --app-dir <path>               # build a different PJ4 app checkout
#   appimage/build_in_docker.sh --sdk-dir <path>               # trial a local plotjuggler_sdk checkout
#   appimage/build_in_docker.sh --plugins-dir <path>           # bundle plugins (see below)
#   appimage/build_in_docker.sh --app-dir <app> --sdk-dir <sdk> --plugins-dir <plugins>
#   appimage/build_in_docker.sh --fresh ...                    # ignore caches; rebuild plugin deps from scratch
#   PJ_INCLUDE_PLUGINS="<basenames…>" appimage/build_in_docker.sh --plugins-dir <src>
#     # (source-repo path only) after the in-container compile, keep only the
#     # whitespace-separated top-level entries in /out (e.g. curated .so basenames
#     # plus "ros2-topic-subscriber") — anything else is dropped BEFORE the package
#     # step, so build_appimage.sh emits an AppImage carrying only the requested
#     # curated set. Missing entries fail the build. Unset/empty ships everything.
#   REBUILD_IMAGE=1 appimage/build_in_docker.sh ...            # force-rebuild the builder image
#
# --app-dir <path> builds a different PJ4 app checkout: the app checkout's own
# build.sh and appimage/ scripts run, and the AppImage lands under
# <app-dir>/appimage/. The builder image still comes from THIS repo; it bakes Qt
# from THIS repo's versions.env, so an --app-dir checkout that pins a different
# Qt in its own versions.env needs REBUILD_IMAGE=1 (or a matching versions.env)
# or the in-container build will not find Qt. The container reuses
# <app-dir>/build; if it holds artifacts from a prior HOST ./build.sh (different
# toolchain/glibc), remove <app-dir>/build first for a clean in-container build.
#
# --plugins-dir <path> accepts EITHER:
#   * a plugin SOURCE repo root (a pj-official-plugins checkout — detected by a
#     SDK_VERSION file + scripts/ensure_core.sh). The sources are copied into the
#     builder and COMPILED there, so the plugins inherit the container's old glibc
#     baseline (2.35) and actually load on the runtime image / older distros.
#     Building host .so against a newer glibc is the #1 reason bundled plugins
#     silently fail to dlopen — so this is the correct, automatic path. The single
#     aggregate ./build.sh now produces all plugins including toolbox_mosaico:
#     Arrow is built once with Flight + gRPC + protobuf, so Mosaico is present
#     without a second standalone build; or
#   * a directory of already-built, self-contained plugins (e.g. unpacked
#     marketplace zips), which is copied in verbatim.
# Either way the host path may live anywhere — it is bind-mounted for you; you
# never stage by hand. Every other argument passes through to build_appimage.sh.
#
# --sdk-dir <path> builds the app AND (when --plugins-dir is a source repo) the
# plugins against a LOCAL plotjuggler_sdk checkout, to trial custom SDK changes.
# For source-repo plugins, the custom SDK is conan create'd into the PERSISTENT
# plugin Conan cache volume, so a later run WITHOUT --sdk-dir keeps using it until
# you pass --fresh to return to the pinned SDK.
#
# Plugin-build caching (source-repo path only): the in-container Conan cache and
# ccache are persisted in two named Docker volumes by DEFAULT, so the heavy
# Arrow + Flight + gRPC + protobuf build (and the plugin objects) compile from
# source ONCE — subsequent runs are cache hits. The volumes hold glibc-2.35
# (jammy) artifacts, kept separate from the host ~/.conan2 on purpose. Pass
# --fresh to delete them and force a from-scratch plugin build (the base builder
# image is unaffected; use REBUILD_IMAGE=1 to rebuild that).
#
# Verify the result on a clean Ubuntu with: appimage/run_in_docker.sh
# Output: appimage/PlotJuggler-<version>-<arch>.AppImage (owned by the host user).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
source "${REPO_ROOT}/versions.env"

IMAGE_TAG="pj4-appimage-builder:jammy-qt${PJ_QT_VERSION}-${PJ_APPIMAGE_ARCH}"
# Persistent caches for the in-container plugin compile (source-repo path). Named
# Docker volumes so the glibc-2.35 Conan packages + ccache survive across runs;
# kept separate from the host ~/.conan2 (which is built against a newer glibc).
# --fresh removes both.
PLUGIN_CONAN_VOL="pj4-appimage-plugin-conan"
PLUGIN_CCACHE_VOL="pj4-appimage-plugin-ccache"

usage() {
  cat <<'EOF'
Build the PlotJuggler 4 AppImage inside a fully-baked Ubuntu 22.04 container.

Usage:
  appimage/build_in_docker.sh                      # app-only AppImage
  appimage/build_in_docker.sh --plugins-registry   # bundle the official set
  appimage/build_in_docker.sh --app-dir <path>     # build a different PJ4 app
                                                   # checkout; the app's own
                                                   # build.sh + appimage/
                                                   # scripts run, and the
                                                   # AppImage lands under
                                                   # <app-dir>/appimage/.
  appimage/build_in_docker.sh --sdk-dir <path>     # build the app AND (when
                                                   # --plugins-dir is a source
                                                   # repo) the plugins against a
                                                   # LOCAL plotjuggler_sdk
                                                   # checkout, to trial custom
                                                   # SDK changes.
  appimage/build_in_docker.sh --plugins-dir <path> # bundle plugins:
                                                   #   <path> = a pj-official-plugins
                                                   #   SOURCE repo  -> compiled IN the
                                                   #   container (correct glibc; single
                                                   #   aggregate ./build.sh builds all
                                                   #   plugins incl. toolbox_mosaico,
                                                   #   Arrow built once with Flight)
                                                   #   then bundled;
                                                   #   OR a dir of prebuilt
                                                   #   self-contained plugins -> copied.
  appimage/build_in_docker.sh --app-dir <app> --sdk-dir <sdk> --plugins-dir <plugins>
  REBUILD_IMAGE=1 appimage/build_in_docker.sh ...  # force-rebuild the builder image
  appimage/build_in_docker.sh --fresh ...          # ignore caches; rebuild plugin deps from scratch

--app-dir builds a different PJ4 app checkout, but the builder image still bakes
Qt from THIS repo's versions.env. If the --app-dir checkout pins a different Qt
in its own versions.env, use REBUILD_IMAGE=1 (or a matching versions.env) or the
in-container build will not find Qt.

--plugins-dir is handled for you (mounted / compiled in-container, no manual
staging). For source-repo plugin builds the in-container Conan cache + ccache
persist in named Docker volumes by default (first build slow, then cached);
--fresh removes them for a clean from-scratch build. With --sdk-dir and source-repo
plugins, the custom SDK is conan create'd into the PERSISTENT plugin Conan cache
volume; a later run WITHOUT --sdk-dir keeps using it until you pass --fresh to return
to the pinned SDK. Every other argument passes through to appimage/build_appimage.sh.
Output: appimage/PlotJuggler-<version>-<arch>.AppImage, using versions.env plus any PJ_VERSION override.
EOF
}

# Parse args. --plugins-dir <path> may point ANYWHERE on the host. If it is a
# plugin SOURCE repo it is compiled inside the builder (glibc-matched); if it is a
# dir of prebuilt plugins it is bind-mounted and copied. Everything else passes
# through to appimage/build_appimage.sh unchanged.
FWD_ARGS=()
PLUGINS_MOUNT=()
PLUGIN_SRC=""     # set when --plugins-dir points at a plugin source repo to build
FRESH=0           # --fresh: wipe the persistent plugin-build caches before building
SDK_SRC=""        # optional local plotjuggler_sdk checkout for app + source plugins
APP_SRC=""        # optional PJ4 app checkout to mount at /work instead of this repo
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h | --help)
      usage; exit 0 ;;
    --fresh)
      FRESH=1; shift ;;
    --app-dir)
      [[ $# -ge 2 && -n "${2:-}" ]] || { echo "ERROR: --app-dir needs a path" >&2; exit 1; }
      app_dir="$2"
      [[ -d "${app_dir}" ]] || { echo "ERROR: --app-dir '${app_dir}' is not a directory" >&2; exit 1; }
      app_dir="$(cd "${app_dir}" && pwd)"
      [[ -f "${app_dir}/build.sh" && -d "${app_dir}/pj_app" && -f "${app_dir}/appimage/build_appimage.sh" ]] || {
        echo "ERROR: --app-dir '${app_dir}' does not look like a PJ4 checkout (needs build.sh, pj_app/, appimage/build_appimage.sh)" >&2
        exit 1
      }
      APP_SRC="${app_dir}"
      shift 2 ;;
    --sdk-dir)
      [[ $# -ge 2 && -n "${2:-}" ]] || { echo "ERROR: --sdk-dir needs a path" >&2; exit 1; }
      sdk_dir="$2"
      [[ -d "${sdk_dir}" ]] || { echo "ERROR: --sdk-dir '${sdk_dir}' is not a directory" >&2; exit 1; }
      sdk_dir="$(cd "${sdk_dir}" && pwd)"
      [[ -f "${sdk_dir}/CMakeLists.txt" && -d "${sdk_dir}/pj_base" ]] || {
        echo "ERROR: --sdk-dir '${sdk_dir}' does not look like a plotjuggler_sdk checkout (needs CMakeLists.txt and pj_base/)" >&2
        exit 1
      }
      SDK_SRC="${sdk_dir}"
      shift 2 ;;
    --plugins-dir)
      host_dir="${2:?--plugins-dir needs a path}"
      [[ -d "${host_dir}" ]] || { echo "ERROR: --plugins-dir '${host_dir}' is not a directory" >&2; exit 1; }
      host_dir="$(cd "${host_dir}" && pwd)"   # absolute path for an unambiguous bind mount
      if [[ -f "${host_dir}/SDK_VERSION" && -f "${host_dir}/scripts/ensure_core.sh" ]]; then
        PLUGIN_SRC="${host_dir}"              # a source repo -> compile it in-container
      else
        PLUGINS_MOUNT=(-v "${host_dir}:/plugins:ro")   # prebuilt -> copy verbatim
        FWD_ARGS+=(--plugins-dir /plugins)
      fi
      shift 2 ;;
    *)
      FWD_ARGS+=("$1"); shift ;;
  esac
done

WORK_ROOT="${APP_SRC:-${REPO_ROOT}}"   # the PJ4 tree that gets built (mounted at /work); defaults to this repo
PLUGIN_SDK_ARGS=()
if [[ -n "${SDK_SRC}" ]]; then
  PLUGIN_SDK_ARGS=(-v "${SDK_SRC}:/custom-sdk:ro" -e PJ_CUSTOM_SDK=/custom-sdk)
  if [[ -z "${PLUGIN_SRC}" ]]; then
    echo "WARNING: --sdk-dir set but no plugin source repo was provided; it only affects plugins built from source (the app resolves the SDK pinned in conanfile.txt)." >&2
  fi
fi

export DOCKER_BUILDKIT=1

if [[ "${REBUILD_IMAGE:-0}" == "1" ]] || ! docker image inspect "${IMAGE_TAG}" >/dev/null 2>&1; then
  echo "==> Building image ${IMAGE_TAG} (baking Qt + Conan closure + linuxdeploy; first build is slow)"
  docker build -t "${IMAGE_TAG}" -f "${SCRIPT_DIR}/Dockerfile.build" \
    --build-arg PJ_QT_VERSION="${PJ_QT_VERSION}" \
    --build-arg PJ_APPIMAGE_ARCH="${PJ_APPIMAGE_ARCH}" \
    "${REPO_ROOT}"
fi

# --fresh: drop the persistent plugin-build caches so the next source-repo build
# recompiles Arrow + its deps and every plugin object from scratch. No-op for the
# prebuilt --plugins-dir path (it compiles nothing) and for the base image, which
# REBUILD_IMAGE=1 rebuilds instead.
if [[ "${FRESH}" == "1" ]]; then
  echo "==> --fresh: removing plugin build caches (Conan + ccache Docker volumes)"
  docker volume rm -f "${PLUGIN_CONAN_VOL}" "${PLUGIN_CCACHE_VOL}" >/dev/null 2>&1 || true
fi

# Compile a plugin source repo INSIDE the builder so the .so inherit glibc 2.35
# (matching the app + runner). The single aggregate ./build.sh produces all
# plugins including toolbox_mosaico, with Arrow built once with Flight; the
# result is left in build/plugins-built (gitignored) for the bundling run below.
#
# PJ_INCLUDE_PLUGINS (optional): whitespace-separated list of top-level entries
# under /out to KEEP (e.g. ".so basenames" + the ros2 bundle dir name). When
# set, everything else at the top level is removed before the package step, so
# the released AppImage carries only the requested curated set instead of the
# full aggregate. Unset/empty preserves the current behaviour (everything the
# aggregate build produced ships).
if [[ -n "${PLUGIN_SRC}" ]]; then
  built_bin="${WORK_ROOT}/build/plugins-built"
  echo "==> Compiling plugins from ${PLUGIN_SRC} inside the builder (glibc-matched; first build is slow)"
  rm -rf "${built_bin}"; mkdir -p "${built_bin}"
  docker run --rm --entrypoint bash \
    -v "${PLUGIN_SRC}:/plugins-src:ro" \
    -v "${built_bin}:/out" \
    -v "${PLUGIN_CONAN_VOL}:/root/.conan2" \
    -v "${PLUGIN_CCACHE_VOL}:/root/.ccache" \
    "${PLUGIN_SDK_ARGS[@]}" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -e PJ_INCLUDE_PLUGINS="${PJ_INCLUDE_PLUGINS:-}" \
    "${IMAGE_TAG}" -c '
      set -euo pipefail
      # Persisted caches: the Conan home (/root/.conan2) and ccache (/root/.ccache)
      # are named Docker volumes that survive across runs, so Arrow + Flight + gRPC
      # + protobuf and the plugin objects build from source only ONCE. On first use
      # Docker SEEDS the new conan volume from the image-baked cache (the app-dep
      # closure + the default profile), so even the first build reuses everything
      # the image already has — only the plugin-specific deps compile cold. A later
      # REBUILD_IMAGE does not re-seed an existing volume; --fresh deletes it so the
      # next run re-seeds from the (possibly newer) image. `conan profile detect` is
      # a harmless safety net. ccache is wired transparently via /usr/lib/ccache.
      export CCACHE_DIR=/root/.ccache
      [ -d /usr/lib/ccache ] && export PATH="/usr/lib/ccache:${PATH}"
      conan profile detect --force >/dev/null 2>&1 || true
      git config --global --add safe.directory "*" || true
      # Copy the sources into the container (excluding the host build/ — which
      # holds host-glibc artifacts — and .git) so nothing host-side is mutated and
      # the in-container build starts clean.
      mkdir -p /tmp/psrc
      ( cd /plugins-src && tar -cf - --exclude=./build --exclude=./.git . ) | ( cd /tmp/psrc && tar -xf - )
      cd /tmp/psrc
      if [ -n "${PJ_CUSTOM_SDK:-}" ]; then
        echo "==> Seeding Conan cache with custom SDK from ${PJ_CUSTOM_SDK} (overrides pinned SDK_VERSION)"
        rm -rf /tmp/custom-sdk; mkdir -p /tmp/custom-sdk
        ( cd "${PJ_CUSTOM_SDK}" && tar -cf - --exclude=./build --exclude=./.git . ) | ( cd /tmp/custom-sdk && tar -xf - )
        conan create /tmp/custom-sdk --version "$(cat SDK_VERSION)" -s build_type=Release -s compiler.cppstd=20 --build=missing
        # Mark the persistent Conan volume so a later run WITHOUT --sdk-dir can warn
        # that the cache still holds this experimental SDK (ensure_core.sh is
        # cache-first, so it would otherwise silently reuse it). --fresh wipes both.
        mkdir -p /root/.conan2; touch /root/.conan2/.pj4-custom-sdk-seeded
      elif [ -f /root/.conan2/.pj4-custom-sdk-seeded ]; then
        echo "WARNING: the plugin Conan cache still holds a custom plotjuggler_sdk from a previous --sdk-dir run; plugins will build against it, NOT the pinned SDK. Pass --fresh to reset." >&2
      fi
      scripts/ensure_core.sh   # build plotjuggler_sdk/<SDK_VERSION> at the container glibc
      ./build.sh               # build all plugins incl. toolbox_mosaico (Arrow once with Flight)
      cp -a /tmp/psrc/build/all/Release/bin/. /out/
      # toolbox_transform_editor is outside the aggregate add_subdirectory list,
      # so build it standalone.
      echo "==> Building toolbox_transform_editor standalone…"
      ./build.sh toolbox_transform_editor
      cp -a /tmp/psrc/build/toolbox_transform_editor/Release/bin/. /out/
      # Fold in the ROS 2 multi-distro bundle when present (proxy + per-distro
      # inners under dist/<distro>/, each built per-distro in its own container).
      if [ -d /tmp/psrc/dist_ros2 ]; then
        echo "==> Bundling ros2 multi-distro (proxy + per-distro inners)"
        rm -rf /out/ros2-topic-subscriber && mkdir -p /out/ros2-topic-subscriber
        cp -a /tmp/psrc/dist_ros2/. /out/ros2-topic-subscriber/
      fi
      # Optional curated-set filter: keep only the whitelisted entries in /out.
      # The list is expected to be whitespace-separated basenames of the top-level
      # entries (e.g. "libcsv_source_plugin.so libtoolbox_mosaico_plugin.so
      # ros2-topic-subscriber"). Fail if any listed entry is missing — the caller
      # asked for a specific set and a silent hole would ship a broken bundle.
      if [ -n "${PJ_INCLUDE_PLUGINS:-}" ]; then
        echo "==> Filtering /out to the curated set: ${PJ_INCLUDE_PLUGINS}"
        missing=""
        for name in ${PJ_INCLUDE_PLUGINS}; do
          if [ ! -e "/out/${name}" ]; then missing="${missing} ${name}"; fi
        done
        if [ -n "${missing}" ]; then
          echo "error: curated entries missing from /out:${missing}" >&2
          echo "current /out contents:" >&2
          ls -1 /out >&2
          exit 7
        fi
        for entry in /out/*; do
          name="$(basename "${entry}")"
          keep=0
          for want in ${PJ_INCLUDE_PLUGINS}; do
            if [ "${name}" = "${want}" ]; then keep=1; break; fi
          done
          if [ "${keep}" -eq 0 ]; then
            echo "  - dropping ${name}"
            rm -rf "${entry}"
          fi
        done
      fi
      chown -R "${HOST_UID}:${HOST_GID}" /out
    '
  # Absolute container path: build_appimage.sh cd's into appimage/ before reading
  # this, so a relative path would resolve against the wrong directory.
  FWD_ARGS+=(--plugins-dir /work/build/plugins-built)
fi

echo "==> Building AppImage in container (no --privileged; FUSE-less linuxdeploy)"
run_cmd=(docker run --rm
  -v "${WORK_ROOT}:/work" -w /work
  "${PLUGINS_MOUNT[@]}"
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)"
  -e PJ_VERSION="${PJ_VERSION:-}"
  "${IMAGE_TAG}"
  "${FWD_ARGS[@]}")
"${run_cmd[@]}"

echo ""
# Report the real output name from the app tree's versions.env: build_appimage.sh
# names the file from WORK_ROOT/versions.env, which differs from this repo under
# --app-dir (PJ_VERSION, if set, still overrides). Subshell so sourcing it doesn't
# clobber the image-tag vars read from this repo above.
( source "${WORK_ROOT}/versions.env" 2>/dev/null || true
  echo "==> Done: ${WORK_ROOT}/appimage/PlotJuggler-${PJ_VERSION:-${PJ_APP_VERSION}}-${PJ_APPIMAGE_ARCH}.AppImage" )
