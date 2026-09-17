#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Build a PlotJuggler 4 release AppImage with the curated plugin set EMBEDDED BY
# COMPILATION (not downloaded from the registry). Ties together the two build
# flows that are otherwise manual:
#
#   * ros2 multi-distro  -> data_stream_ros2/docker/run-local.sh --bundle
#                           (distro-agnostic proxy in Ubuntu 22.04 + one .so per
#                           supported ROS 2 distro, each in its own Docker image)
#   * app + non-ros2     -> DEFAULT: packaging/appimage/build_in_docker.sh (Ubuntu 22.04 /
#     plugins + package    glibc 2.35), which compiles both the app and the
#                          aggregate plugin set inside the builder and picks up
#                          the ros2 bundle produced by the step above. This
#                          orchestrator then extracts the produced AppImage,
#                          drops non-MUST .so from the plugins dir, and repacks
#                          so the released AppImage carries exactly the curated
#                          20 (matching --host-build).
#                          With --host-build: ./build.sh + pj_ported_plugins/
#                          build.sh on the host, curated 20-plugin filter, then
#                          packaging/appimage/build_appimage.sh --plugins-dir.
#
# DEFAULT = docker (portable). The AppImage inherits the container's glibc 2.35
# floor and runs on any distro with glibc >= 2.35 (Ubuntu 22.04 and newer, and
# equivalents on other families). Ros2 is ALWAYS Docker-per-distro either way.
# Both modes produce the same curated 20-plugin bundle.
#
# --host-build (opt-in) = host build (fast, this machine). The resulting AppImage
# inherits the HOST glibc floor (e.g. Ubuntu 24.04 => glibc 2.38), so it only
# runs on hosts whose glibc is >= the build host's. Kept for local iteration.
#
# Usage:
#   build_release_appimage.sh [--pj4 <PJ4 root>] [--plugins-dir <dir>]
#                             [--sdk-dir <dir>] [--out <file.AppImage>]
#                             [--host-build] [--fresh]
#                             [--skip-ros2] [--skip-app] [--skip-plugins]
#                             [--ros2-distros "humble iron jazzy rolling"]
#
#   --host-build                            opt into the legacy host-build flow
#                                           (curated 20 plugins, non-portable).
#   --fresh                                 (docker mode only) drop the
#                                           persistent Conan + ccache Docker
#                                           volumes so app+plugins compile from
#                                           scratch; the builder image is
#                                           untouched (use REBUILD_IMAGE=1 for
#                                           that).
#   --skip-app/--skip-plugins/--skip-ros2   reuse a prior build of that piece
#                                           (skip its (re)compile; must already
#                                           exist). In docker mode --skip-app
#                                           and --skip-plugins are collapsed:
#                                           either both apply or neither, since
#                                           the container run compiles them
#                                           together.
set -euo pipefail

# ---- locate repos ----------------------------------------------------------
PJ4_ROOT=""
PLUGINS_REPO=""
SDK_LOCAL=""
OUT=""
SKIP_APP=0; SKIP_PLUGINS=0; SKIP_ROS2=0
ROS2_DISTROS="humble iron jazzy rolling"
USE_DOCKER=1     # default: glibc-matched via packaging/appimage/build_in_docker.sh
FRESH=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pj4)          PJ4_ROOT="${2:?}"; shift 2 ;;
    --plugins-dir)  PLUGINS_REPO="${2:?}"; shift 2 ;;
    --sdk-dir)      SDK_LOCAL="${2:?}"; shift 2 ;;
    --out)          OUT="${2:?}"; shift 2 ;;
    --host-build)   USE_DOCKER=0; shift ;;
    --fresh)        FRESH=1; shift ;;
    --skip-app)     SKIP_APP=1; shift ;;
    --skip-plugins) SKIP_PLUGINS=1; shift ;;
    --skip-ros2)    SKIP_ROS2=1; shift ;;
    --ros2-distros) ROS2_DISTROS="${2:?}"; shift 2 ;;
    -h|--help)      grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

# Default PJ4 root = two parent directories above packaging/appimage/, else cwd.
if [[ -z "${PJ4_ROOT}" ]]; then
  if [[ -f "$(dirname "$0")/../../build.sh" ]]; then
    PJ4_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
  else
    PJ4_ROOT="$(pwd)"
  fi
fi
# The plugins and the SDK are SIBLING repositories, not nested checkouts. Take
# each from its flag, then its environment variable, then a sibling of PJ4_ROOT.
: "${PLUGINS_REPO:=${PJ_PLUGINS_DIR:-}}"
: "${SDK_LOCAL:=${PJ_SDK_DIR:-}}"
[[ -n "${PLUGINS_REPO}" ]] || PLUGINS_REPO="$(cd "${PJ4_ROOT}/.." && pwd)/pj-official-plugins"
[[ -n "${SDK_LOCAL}"    ]] || SDK_LOCAL="$(cd "${PJ4_ROOT}/.." && pwd)/plotjuggler_sdk"

for d in "${PLUGINS_REPO}:--plugins-dir (or PJ_PLUGINS_DIR)" \
         "${SDK_LOCAL}:--sdk-dir (or PJ_SDK_DIR)"; do
  path="${d%%:*}"; flag="${d#*:}"
  [[ -d "${path}" ]] || {
    echo "ERROR: not a directory: ${path}" >&2
    echo "       pass ${flag}; the plugins and SDK repos are siblings of PJ4, not nested in it." >&2
    exit 1
  }
done
PLUGINS_REPO="$(cd "${PLUGINS_REPO}" && pwd)"
SDK_LOCAL="$(cd "${SDK_LOCAL}" && pwd)"
ROS2_DIR="${PLUGINS_REPO}/data_stream_ros2"

for p in "${PJ4_ROOT}/build.sh" "${PLUGINS_REPO}/build.sh" \
         "${ROS2_DIR}/docker/run-local.sh" "${PJ4_ROOT}/packaging/appimage/build_appimage.sh" \
         "${PJ4_ROOT}/packaging/appimage/build_in_docker.sh"; do
  [[ -f "$p" ]] || { echo "ERROR: expected script not found: $p" >&2; exit 1; }
done

# Stamp the output AppImage name with the PJ4 build commit + a UTC timestamp so
# two AppImages built from the same base version but different source snapshots
# don't clobber each other in a shared drop folder (Dropbox, Nextcloud, CI
# artifacts). The stamp goes into PJ_VERSION, which build_appimage.sh substitutes
# into `PlotJuggler-<version>-<arch>.AppImage`, so honour a caller-provided
# PJ_VERSION verbatim (release tags, external CI overrides).
if [[ -z "${PJ_VERSION:-}" ]]; then
  source "${PJ4_ROOT}/versions.env"      # PJ_APP_VERSION
  stamp="$(date -u +%Y%m%d-%H%M)"        # UTC — matches CI logs
  sha="$(git -C "${PJ4_ROOT}" rev-parse --short=10 HEAD 2>/dev/null || echo unknown)"
  dirty=""
  git -C "${PJ4_ROOT}" diff --quiet HEAD 2>/dev/null || dirty="-dirty"
  export PJ_VERSION="${PJ_APP_VERSION}-${stamp}-${sha}${dirty}"
  echo "==> AppImage version stamp: ${PJ_VERSION}"
fi

# SDK_LOCAL (resolved above) is the ros2 `run-local.sh --core` mount: bind-mounted
# as /core in every distro container, with `conan export /core` run on it. It must
# therefore be the SDK repo whose `conanfile.py` declares `name = "plotjuggler_sdk"`
# — the plugins repo root is an aggregate with no `name` and would fail `conan export`.

# The release MUST-set, by built .so basename. Keep in lockstep with
# BUNDLE_IDS in packaging/appimage/build_appimage.sh (the registry-mode equivalent).
# Anything the aggregate build produces that is NOT in this list (mqtt, zmq,
# udp, lerobot, fft, colormap, reactive_script, ...) is deliberately left out
# of the released bundle in BOTH modes (docker mode filters the packaged
# AppImage post-build; host mode stages only these files before invoking
# build_appimage.sh).
RELEASE_FLAT_SOS=(
  libcsv_source_plugin.so
  libmcap_source_plugin.so
  libparquet_source_plugin.so
  libulog_source_plugin.so
  libmp4_source_plugin.so
  libdata_load_3d_plugin.so
  libdummy_stream_plugin.so
  libfoxglove_source_plugin.so
  libpj_bridge_source_plugin.so
  libwebrtc_source_plugin.so
  libparser_ros_plugin.so
  libparser_protobuf_plugin.so
  libparser_json_plugin.so
  libparser_data_tamer_plugin.so
  libparser_arrow_plugin.so
  libtoolbox_quaternion_plugin.so
  libtoolbox_transform_editor_plugin.so
  libtoolbox_mosaico_plugin.so
  # Linux-only in the bundle (POSIX-only CLI backends); see BUNDLE_IDS.
  libtoolbox_assistant_agent_plugin.so
)
ROS2_EXTENSION_DIR="ros2-topic-subscriber"  # bundle dir name inside the plugin tree

log() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

# ---- 1) ros2 proxy + all distros (Docker) — same in both modes -------------
if [[ "${SKIP_ROS2}" == 0 ]]; then
  log "Building ros2 proxy + distros [${ROS2_DISTROS}] via Docker"
  # run-local.sh --bundle builds proxy + every distro in distros.env. Needs the
  # docker group; if the caller is not in an active docker group, re-exec under sg.
  ros2_cmd=( "${ROS2_DIR}/docker/run-local.sh" --bundle --core "${SDK_LOCAL}" )
  if docker info >/dev/null 2>&1; then
    "${ros2_cmd[@]}"
  else
    sg docker -c "$(printf '%q ' "${ros2_cmd[@]}")"
  fi
fi

# ---- 2) app + non-ros2 plugins + package -----------------------------------
if [[ "${USE_DOCKER}" == 1 ]]; then
  # Docker mode: single container run compiles app + aggregate plugins + folds
  # in dist_ros2/ (from step 1) + packages the AppImage. glibc 2.35, portable.
  # --skip-app/--skip-plugins are collapsed here: the container run is atomic.
  if [[ "${SKIP_APP}" == 1 || "${SKIP_PLUGINS}" == 1 ]]; then
    echo "warning: --skip-app / --skip-plugins are ignored in docker mode (compiled together in one container run). Use --host-build to skip individually." >&2
  fi
  log "Building app + non-ros2 plugins in the Ubuntu 22.04 builder (portable, glibc 2.35)"
  # Hand the curated MUST-set to build_in_docker.sh via PJ_INCLUDE_PLUGINS so it
  # filters /out INSIDE the container BEFORE build_appimage.sh runs. That way the
  # packaged AppImage carries only the curated entries directly — no post-build
  # extract + repack step.
  docker_cmd=( "${PJ4_ROOT}/packaging/appimage/build_in_docker.sh" --plugins-dir "${PLUGINS_REPO}" )
  [[ "${FRESH}" == 1 ]] && docker_cmd+=( --fresh )
  PJ_INCLUDE_PLUGINS="${RELEASE_FLAT_SOS[*]} ${ROS2_EXTENSION_DIR}" \
    "${docker_cmd[@]}"
else
  # Host mode (opt-in): compile on the host, then stage only the MUST-set and
  # call build_appimage.sh --plugins-dir. AppImage's glibc floor equals the host's.
  if [[ "${SKIP_APP}" == 0 ]]; then
    log "Building PJ4 app (host)"
    ( cd "${PJ4_ROOT}" && ./build.sh )
  fi
  [[ -x "${PJ4_ROOT}/build/pj_app/plotjuggler4" ]] || \
    { echo "ERROR: app binary missing (build/pj_app/plotjuggler4) — drop --skip-app" >&2; exit 1; }

  # toolbox_mosaico, toolbox_transform_editor and parser_arrow are all part of
  # the aggregate's `else()` add_subdirectory list (pj_ported_plugins/CMakeLists.txt
  # in standalone mode) — a plain `./build.sh` already produces every .so below,
  # no per-plugin standalone build needed.
  if [[ "${SKIP_PLUGINS}" == 0 ]]; then
    log "Building the aggregate plugin set (host)"
    ( cd "${PLUGINS_REPO}"
      [[ -x scripts/ensure_core.sh ]] && scripts/ensure_core.sh
      ./build.sh
    )
  fi

  STAGING="$(mktemp -d)"; trap 'rm -rf "${STAGING}"' EXIT
  log "Collecting release plugins into staging: ${STAGING}"
  find_so() {  # $1 = basename; search the plugins build trees, newest match wins
    find "${PLUGINS_REPO}/build" -name "$1" -path '*Release/bin/*' 2>/dev/null \
      | grep -vE '/test|/lib/' | head -1
  }
  missing=0
  for so in "${RELEASE_FLAT_SOS[@]}"; do
    f="$(find_so "${so}")"
    if [[ -n "${f}" ]]; then cp "${f}" "${STAGING}/"; echo "  + ${so}"
    else echo "  ! MISSING ${so}"; missing=1; fi
  done
  # ros2: copy the assembled bundle tree verbatim into a ros2-topic-subscriber/
  # subdir so the proxy finds its sibling dist/<distro>/ inners at runtime
  # (self-describing load). The name mirrors what build_in_docker.sh produces.
  ROS2_BUNDLE="${ROS2_DIR}/dist_ros2"
  if [[ -d "${ROS2_BUNDLE}" ]]; then
    mkdir -p "${STAGING}/${ROS2_EXTENSION_DIR}"
    cp -a "${ROS2_BUNDLE}/." "${STAGING}/${ROS2_EXTENSION_DIR}/"
    echo "  + ${ROS2_EXTENSION_DIR}/ (proxy + $(ls "${STAGING}/${ROS2_EXTENSION_DIR}/dist" 2>/dev/null | tr '\n' ' '))"
  else
    echo "  ! MISSING ros2 bundle (${ROS2_BUNDLE}) — drop --skip-ros2 or run run-local.sh --bundle"; missing=1
  fi
  [[ "${missing}" == 0 ]] || { echo "ERROR: some release plugins are missing (see ! above)" >&2; exit 1; }

  log "Packaging AppImage"
  ( cd "${PJ4_ROOT}" && APPIMAGE_EXTRACT_AND_RUN=1 \
      ./packaging/appimage/build_appimage.sh --plugins-dir "${STAGING}" )
fi

BUILT="$(ls -t "${PJ4_ROOT}/packaging/appimage/"PlotJuggler-*-x86_64.AppImage 2>/dev/null | head -1)"
if [[ -n "${OUT}" && -n "${BUILT}" ]]; then cp "${BUILT}" "${OUT}"; BUILT="${OUT}"; fi
log "Done: ${BUILT}"
if [[ "${USE_DOCKER}" == 1 ]]; then
  echo "Mode: docker (Ubuntu 22.04, glibc 2.35). Curated $(( ${#RELEASE_FLAT_SOS[@]} + 1 )) plugins embedded + ros2(${ROS2_DISTROS})."
else
  echo "Mode: host build. Curated $(( ${#RELEASE_FLAT_SOS[@]} + 1 )) plugins embedded + ros2(${ROS2_DISTROS})."
fi
