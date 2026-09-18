#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Build the PlotJuggler 4 AppImage inside a fully-baked Ubuntu 22.04 container.
#
# The builder image (tagged with the Qt/arch pins from versions.env) bakes Qt,
# the entire Conan dependency closure, and a pinned linuxdeploy into its layers, so running the
# container fetches nothing — except, with --plugins-registry, the published plugin
# zips. Adapts PJ3's AppImage Docker build to PJ4:
#   * no --privileged (linuxdeploy runs extracted via APPIMAGE_EXTRACT_AND_RUN);
#   * Qt + Conan are baked, not installed from system packages each run.
#
# Usage:
#   packaging/appimage/build_in_docker.sh                                # app-only AppImage
#   packaging/appimage/build_in_docker.sh --plugins-registry             # bundle the official set
#   packaging/appimage/build_in_docker.sh --app-dir <path>               # build a different PJ4 app checkout
#   packaging/appimage/build_in_docker.sh --sdk-dir <path>               # trial a local plotjuggler_sdk checkout
#   packaging/appimage/build_in_docker.sh --plugins-dir <path>           # bundle plugins (see below)
#   packaging/appimage/build_in_docker.sh --app-dir <app> --sdk-dir <sdk> --plugins-dir <plugins>
#   packaging/appimage/build_in_docker.sh --fresh ...                    # ignore caches; rebuild plugin deps from scratch
#   packaging/appimage/build_in_docker.sh --with-tests ...               # also compile tests + demos (dev/CI trees only; ~10x larger)
#   packaging/appimage/build_in_docker.sh --sanitize asan ...            # instrumented AppImage with line information
#   PJ_INCLUDE_PLUGINS="<basenames…>" packaging/appimage/build_in_docker.sh --plugins-dir <src>
#     # (source-repo path only) after the in-container compile, keep only the
#     # whitespace-separated top-level entries in /out (e.g. curated .so basenames
#     # plus "ros2-topic-subscriber") — anything else is dropped BEFORE the package
#     # step, so build_appimage.sh emits an AppImage carrying only the requested
#     # curated set. Missing entries fail the build. Unset/empty ships everything.
#   REBUILD_IMAGE=1 packaging/appimage/build_in_docker.sh ...            # force-rebuild the builder image
#
# --app-dir <path> builds a different PJ4 app checkout: the app checkout's own
# build.sh and packaging/appimage/ scripts run, and the AppImage lands under
# <app-dir>/packaging/appimage/. The builder image still comes from THIS repo; it bakes Qt
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
# Verify the result on a clean Ubuntu with: packaging/appimage/run_in_docker.sh
# Output: packaging/appimage/PlotJuggler-<version>-<arch>.AppImage (owned by the host user).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
source "${REPO_ROOT}/versions.env"

# v2 uses the packaging/appimage paths in the baked entrypoint.
IMAGE_TAG="pj4-appimage-builder:jammy-qt${PJ_QT_VERSION}-${PJ_APPIMAGE_ARCH}-v2"
# Persistent caches for the in-container plugin compile (source-repo path). Named
# Docker volumes so the glibc-2.35 Conan packages + ccache survive across runs;
# kept separate from the host ~/.conan2 (which is built against a newer glibc).
# --fresh removes both.
PLUGIN_CONAN_VOL="pj4-appimage-plugin-conan"
PLUGIN_CCACHE_VOL="pj4-appimage-plugin-ccache"
# MemorySanitizer needs two things no other lane does, both persisted so they are
# paid for once: a libc++ built with -fsanitize=memory (the distro one is a normal
# build and defeats the lane), and a Conan cache of its own. The cache MUST be
# separate — sanitizer flags do not enter package_id, so a shared cache would let
# an uninstrumented package be reused here, or an instrumented one leak out.
MSAN_LIBCXX_VOL="pj4-msan-libcxx"
MSAN_CONAN_VOL="pj4-msan-conan"
# The Clang every instrumented lane compiles with, provisioned per run in both the
# plugin and the app container from LLVM's apt repository. Ubuntu 22.04's newest,
# clang-15, cannot compile C++20 lambda captures of structured bindings and its
# shared TSan runtime crashes (see docs/SANITIZERS.md).
PJ_SANITIZER_CLANG_VERSION="${PJ_SANITIZER_CLANG_VERSION:-22}"
# Container-side installer spliced into both container scripts. Single-quoted so it
# expands inside the container, where PJ_SANITIZER_CLANG_VERSION is forwarded.
# libclang-rt-<v>-dev carries the sanitizer runtimes; clang-<v> alone does not.
# llvm-<v> carries llvm-symbolizer: Clang's runtimes, unlike GCC's, cannot name a
# frame without it on PATH, and every suppression matches frames by name.
clang_provision='
      if ! command -v "llvm-symbolizer-${PJ_SANITIZER_CLANG_VERSION}" >/dev/null 2>&1; then
        echo "==> Provisioning Clang ${PJ_SANITIZER_CLANG_VERSION} from apt.llvm.org"
        codename="$(. /etc/os-release && echo "${VERSION_CODENAME}")"
        wget -qO /etc/apt/trusted.gpg.d/apt.llvm.org.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
          && echo "deb http://apt.llvm.org/${codename}/ llvm-toolchain-${codename}-${PJ_SANITIZER_CLANG_VERSION} main" \
               > /etc/apt/sources.list.d/llvm-toolchain.list \
          && apt-get update -qq >/dev/null 2>&1 \
          && apt-get install -y --no-install-recommends \
               "clang-${PJ_SANITIZER_CLANG_VERSION}" "libclang-rt-${PJ_SANITIZER_CLANG_VERSION}-dev" \
               "llvm-${PJ_SANITIZER_CLANG_VERSION}" >/dev/null 2>&1 \
          || { echo "error: could not install clang-${PJ_SANITIZER_CLANG_VERSION} from apt.llvm.org; instrumented lanes compile with it." >&2; exit 3; }
      fi
      ln -sfn "/usr/bin/llvm-symbolizer-${PJ_SANITIZER_CLANG_VERSION}" /usr/local/bin/llvm-symbolizer
'

usage() {
  cat <<'EOF'
Build the PlotJuggler 4 AppImage inside a fully-baked Ubuntu 22.04 container.

Usage:
  packaging/appimage/build_in_docker.sh                      # app-only AppImage
  packaging/appimage/build_in_docker.sh --plugins-registry   # bundle the official set
  packaging/appimage/build_in_docker.sh --app-dir <path>     # build a different PJ4 app
                                                   # checkout; the app's own
                                                   # build.sh + packaging/appimage/
                                                   # scripts run, and the
                                                   # AppImage lands under
                                                   # <app-dir>/packaging/appimage/.
  packaging/appimage/build_in_docker.sh --sdk-dir <path>     # build the app AND (when
                                                   # --plugins-dir is a source
                                                   # repo) the plugins against a
                                                   # LOCAL plotjuggler_sdk
                                                   # checkout, to trial custom
                                                   # SDK changes.
  packaging/appimage/build_in_docker.sh --plugins-dir <path> # bundle plugins:
                                                   #   <path> = a pj-official-plugins
                                                   #   SOURCE repo  -> compiled IN the
                                                   #   container (correct glibc; single
                                                   #   aggregate ./build.sh builds all
                                                   #   plugins incl. toolbox_mosaico,
                                                   #   Arrow built once with Flight)
                                                   #   then bundled;
                                                   #   OR a dir of prebuilt
                                                   #   self-contained plugins -> copied.
  packaging/appimage/build_in_docker.sh --app-dir <app> --sdk-dir <sdk> --plugins-dir <plugins>
  REBUILD_IMAGE=1 packaging/appimage/build_in_docker.sh ...  # force-rebuild the builder image
  packaging/appimage/build_in_docker.sh --fresh ...          # ignore caches; rebuild plugin deps from scratch
  packaging/appimage/build_in_docker.sh --with-tests ...     # also compile the test suite + demos (dev/CI trees only; ~10x larger)
  packaging/appimage/build_in_docker.sh --sanitize asan ...  # ASan/UBSan AppImage (default: none)

--sanitize none|asan|tsan selects the build lane; PJ_SANITIZE is the environment
equivalent, and the option wins. The tsan lane builds the app, the whole test
suite, the SDK and the plugins, runs every test, and produces NO AppImage: an
instrumented binary needs its runtime tuned before it starts, so shipping one
would hand users an artifact that aborts on launch.
ASan defaults PJ_DEBUG_INFO to lines; a caller-set value (even empty) wins.

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
to the pinned SDK. Every other argument passes through to packaging/appimage/build_appimage.sh.
Output: packaging/appimage/PlotJuggler-<version>-<arch>.AppImage, using versions.env plus any PJ_VERSION override.
ASan output: packaging/appimage/PlotJuggler-<version>-asan-<arch>.AppImage.
EOF
}

PJ_SANITIZE="${PJ_SANITIZE-none}"
set_sanitize() {
  case "$1" in
    none|asan|tsan|msan) PJ_SANITIZE="$1" ;;
    *)
      echo "ERROR: --sanitize/PJ_SANITIZE must be none, asan, tsan or msan (got '$1')." >&2
      exit 2 ;;
  esac
}

# Parse args. --plugins-dir <path> may point ANYWHERE on the host. If it is a
# plugin SOURCE repo it is compiled inside the builder (glibc-matched); if it is a
# dir of prebuilt plugins it is bind-mounted and copied. Everything else passes
# through to packaging/appimage/build_appimage.sh unchanged.
FWD_ARGS=()
PLUGINS_MOUNT=()
PLUGIN_SRC=""     # set when --plugins-dir points at a plugin source repo to build
FRESH=0           # --fresh: wipe the persistent plugin-build caches before building
WITH_TESTS=0      # --with-tests: compile the full development tree
SDK_SRC=""        # optional local plotjuggler_sdk checkout for app + source plugins
APP_SRC=""        # optional PJ4 app checkout to mount at /work instead of this repo
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h | --help)
      usage; exit 0 ;;
    --fresh)
      FRESH=1; shift ;;
    --with-tests)
      WITH_TESTS=1; shift ;;
    --sanitize)
      set_sanitize "${2:-}"; shift 2 ;;
    --sanitize=*)
      set_sanitize "${1#--sanitize=}"; shift ;;
    --app-dir)
      [[ $# -ge 2 && -n "${2:-}" ]] || { echo "ERROR: --app-dir needs a path" >&2; exit 1; }
      app_dir="$2"
      [[ -d "${app_dir}" ]] || { echo "ERROR: --app-dir '${app_dir}' is not a directory" >&2; exit 1; }
      app_dir="$(cd "${app_dir}" && pwd)"
      [[ -f "${app_dir}/build.sh" && -d "${app_dir}/pj_app" && -f "${app_dir}/packaging/appimage/build_appimage.sh" ]] || {
        echo "ERROR: --app-dir '${app_dir}' does not look like a PJ4 checkout (needs build.sh, pj_app/, packaging/appimage/build_appimage.sh)" >&2
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
set_sanitize "${PJ_SANITIZE}"

# Physically separate plugin caches per lane. Conan's tools.build:* configuration
# does NOT participate in package_id, so an instrumented plotjuggler_sdk carries the
# same id as the Release one: a shared cache lets one lane serve the other's binaries
# with no error and no warning, producing a silently half-instrumented AppImage.
if [[ "${PJ_SANITIZE}" != none ]]; then
  PLUGIN_CONAN_VOL="${PLUGIN_CONAN_VOL}-${PJ_SANITIZE}"
  PLUGIN_CCACHE_VOL="${PLUGIN_CCACHE_VOL}-${PJ_SANITIZE}"
fi
# Lane-scoped plugin staging, matching the app's build/<lane> layout. Kept under
# build/ so the container entrypoint's recursive chown still hands it back.
PLUGIN_STAGE_REL="build/plugins-built"
[[ "${PJ_SANITIZE}" == none ]] || PLUGIN_STAGE_REL="build/${PJ_SANITIZE}/plugins-built"

# The ThreadSanitizer runtime must disable address-space randomisation to map its
# shadow memory (see build.sh); the personality syscall that needs is denied by
# Docker's default seccomp profile, which makes every TSan binary abort at start
# with "unexpected memory mapping". Relaxed for this lane only — the packaging
# lanes keep the default profile. Defined here because BOTH container runs, the
# plugin build and the app build, need it before either is launched.
seccomp_args=()
if [[ "${PJ_SANITIZE}" == tsan || "${PJ_SANITIZE}" == msan ]]; then
  seccomp_args=(--security-opt seccomp=unconfined)
fi

# In the TSan lane a failing stage means the sanitizer REPORTED something, which
# is the lane's product rather than a build error. Aborting on the first one
# would leave every later stage unexercised — the plugin tests reporting a race
# would mean the app suite never runs, so the lane could never say anything about
# PJ4. Stage failures are recorded here and re-raised once every stage has run.
tsan_stage_rc=0

# The wrapper always forwards PJ_DEBUG_INFO, so choose the lane's default here
# rather than relying on build.sh. '-' (not ':-') preserves an explicitly empty
# caller value as well as nonempty overrides, with or without --with-tests.
if [[ "${PJ_SANITIZE}" == asan || "${PJ_SANITIZE}" == tsan ]]; then
  # Both instrumented lanes are DIAGNOSTIC, so they need source locations. Without
  # this the tsan lane fell through to the packaging default of "none" and compiled
  # PJ4 and the SDK with -g0, so every race and double-lock report named a function
  # and no file:line — the reports were real but not actionable.
  DEBUG_INFO_DEFAULT="lines"
elif [[ "${WITH_TESTS}" == "1" ]]; then
  DEBUG_INFO_DEFAULT="split"
else
  DEBUG_INFO_DEFAULT="none"
fi
PJ_DEBUG_INFO="${PJ_DEBUG_INFO-${DEBUG_INFO_DEFAULT}}"
SANITIZE_INFIX=""
[[ "${PJ_SANITIZE}" == none ]] || SANITIZE_INFIX="-${PJ_SANITIZE}"

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
  # Lane-scoped so an instrumented run can never pick up Release plugin binaries
  # left by a previous build (and vice versa). Still under build/, which the
  # container entrypoint chowns recursively.
  built_bin="${WORK_ROOT}/${PLUGIN_STAGE_REL}"
  echo "==> Compiling plugins from ${PLUGIN_SRC} inside the builder (glibc-matched; first build is slow)"
  rm -rf "${built_bin}"; mkdir -p "${built_bin}"
  docker run --rm --entrypoint bash \
    ${seccomp_args[@]+"${seccomp_args[@]}"} \
    -v "${PLUGIN_SRC}:/plugins-src:ro" \
    -v "${built_bin}:/out" \
    -v "${PLUGIN_CONAN_VOL}:/root/.conan2" \
    -v "${PLUGIN_CCACHE_VOL}:/root/.ccache" \
    "${PLUGIN_SDK_ARGS[@]}" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -e PJ_INCLUDE_PLUGINS="${PJ_INCLUDE_PLUGINS:-}" \
    -e PJ_SANITIZE="${PJ_SANITIZE}" \
    -e PJ_SANITIZER_CLANG_VERSION="${PJ_SANITIZER_CLANG_VERSION}" \
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
      # Instrumented lanes compile the plugins and the SDK with the same Clang as
      # the app: code instrumented for one compiler sanitizer runtime cannot be
      # loaded into a process running another. CC/CXX are exported only AFTER the
      # profile detect above, so the Conan profile stays GCC and the cached
      # dependency binaries keep their package ids; a Clang profile would rebuild
      # Arrow, gRPC and protobuf from source.
      case "${PJ_SANITIZE:-none}" in
        asan|tsan)
          '"${clang_provision}"'
          # A compiler installed after ccache has no /usr/lib/ccache wrapper yet.
          if command -v update-ccache-symlinks >/dev/null 2>&1; then update-ccache-symlinks; fi
          export CC="clang-${PJ_SANITIZER_CLANG_VERSION}" CXX="clang++-${PJ_SANITIZER_CLANG_VERSION}"
          echo "==> Plugins: compiling with ${CXX}"
          ;;
      esac
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
      # The lane has to reach the plugins as well. An ASan app loading UNinstrumented
      # plugins reports nothing at all for a plugin-side use-after-free, and the
      # reverse aborts at startup with "ASan runtime does not come first". Refuse to
      # build a half-instrumented bundle rather than ship one that looks healthy and
      # is blind to the very defects this lane exists to find.
      PLUGIN_BUILD_ARGS=""
      PLUGIN_BUILD_SUBDIR="build"
      if [ "${PJ_SANITIZE:-none}" != none ]; then
        if ! grep -q -- "--${PJ_SANITIZE}" build.sh; then
          echo "error: --sanitize ${PJ_SANITIZE} requires a plugins checkout whose build.sh understands --${PJ_SANITIZE}." >&2
          echo "       This one does not, so the plugins would be built uninstrumented and the" >&2
          echo "       instrumented build would silently miss every plugin-side error." >&2
          exit 3
        fi
        PLUGIN_BUILD_ARGS="--${PJ_SANITIZE}"
        PLUGIN_BUILD_SUBDIR="build/${PJ_SANITIZE}"
        case "${PJ_SANITIZE}" in
          asan) echo "==> Plugins: AddressSanitizer lane (instrumented SDK + plugin targets)" ;;
          tsan) echo "==> Plugins: ThreadSanitizer lane (instrumented SDK + plugin targets, plugin tests run)" ;;
        esac
      fi
      scripts/ensure_core.sh   # build plotjuggler_sdk/<SDK_VERSION> at the container glibc
      # In a sanitizer lane ./build.sh also RUNS the plugin tests, so a reported
      # race returns nonzero. Under `set -e` that skipped everything after it —
      # including the standalone plugin below, which the aggregate deliberately
      # excludes, leaving it entirely untested whenever any other plugin reported.
      # Record the status and carry on; it is re-raised at the end of the stage.
      plugin_rc=0
      ./build.sh ${PLUGIN_BUILD_ARGS} || plugin_rc=$?   # all plugins incl. toolbox_mosaico (Arrow once with Flight)
      cp -a /tmp/psrc/${PLUGIN_BUILD_SUBDIR}/all/Release/bin/. /out/ || plugin_rc=$?
      # toolbox_transform_editor is outside the aggregate add_subdirectory list,
      # so build it standalone.
      echo "==> Building toolbox_transform_editor standalone…"
      ./build.sh ${PLUGIN_BUILD_ARGS} toolbox_transform_editor || plugin_rc=$?
      cp -a /tmp/psrc/${PLUGIN_BUILD_SUBDIR}/toolbox_transform_editor/Release/bin/. /out/ || plugin_rc=$?
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
      # Re-raise whatever the plugin builds/tests reported, now that every stage
      # has run. Sanitizer findings must still fail the lane.
      exit "${plugin_rc}"
    ' || plugin_stage_rc=$?
  if [[ "${plugin_stage_rc:-0}" -ne 0 ]]; then
    if [[ "${PJ_SANITIZE}" == tsan ]]; then
      echo "==> Plugins: ThreadSanitizer stage exited ${plugin_stage_rc}; continuing so the app lane still runs." >&2
      tsan_stage_rc="${plugin_stage_rc}"
    else
      exit "${plugin_stage_rc}"
    fi
  fi
  # Absolute container path: build_appimage.sh cd's into packaging/appimage/ before reading
  # this, so a relative path would resolve against the wrong directory.
  FWD_ARGS+=(--plugins-dir "/work/${PLUGIN_STAGE_REL}")
fi

echo "==> Building AppImage in container (no --privileged; FUSE-less linuxdeploy)"
if [[ "${PJ_SANITIZE}" == tsan || "${PJ_SANITIZE}" == msan ]]; then
  # Both are test lanes, so they always build tests regardless of --with-tests.
  # Demos stay off: they are interactive programs with no ctest entry, so
  # instrumenting them costs build time and proves nothing. The msan lane
  # overrides PJ_BUILD_TARGET itself (build.sh builds its four Qt-free targets
  # by name), so `all` here is not what it ends up building.
  build_env=(-e PJ_BUILD_TESTS=ON -e PJ_BUILD_DEMOS=OFF -e PJ_BUILD_WIDGET_DEMOS=OFF -e PJ_BUILD_TARGET=all)
  build_env+=(-e PJ_COMPRESS_DEBUG="${PJ_COMPRESS_DEBUG:-ON}")
elif [[ "${WITH_TESTS}" == "1" ]]; then
  build_env=(-e PJ_BUILD_TESTS=ON -e PJ_BUILD_DEMOS=ON -e PJ_BUILD_WIDGET_DEMOS=ON -e PJ_BUILD_TARGET=all)
  build_env+=(-e PJ_COMPRESS_DEBUG="${PJ_COMPRESS_DEBUG:-ON}")
else
  build_env=(-e PJ_BUILD_TESTS=OFF -e PJ_BUILD_DEMOS=OFF -e PJ_BUILD_WIDGET_DEMOS=OFF -e PJ_BUILD_TARGET=pj_app)
  build_env+=(-e PJ_COMPRESS_DEBUG="${PJ_COMPRESS_DEBUG:-}")
fi
# Both build.sh and the unconditional build_appimage.sh entrypoint invocation
# must receive the same lane so packaging consumes the selected nested tree.
build_env+=(-e PJ_DEBUG_INFO="${PJ_DEBUG_INFO}" -e PJ_SANITIZE="${PJ_SANITIZE}")
# PJ4 links Qt6::PrintSupport, so linuxdeploy-plugin-qt deploys Qt's cups
# printsupport plugin and then resolves ITS dependencies. The builder image ships
# no libcups, so that resolution fails with "Could not find dependency:
# libcups.so.2" and the packaging step dies. Provision it per-run rather than in
# Dockerfile.build: baking it would force every developer through a REBUILD_IMAGE
# that re-bakes Qt and the entire Conan closure for one small runtime library.
# Guarded by ldconfig, so it is a no-op once the library is present.
msan_mounts=()
sanitizer_bootstrap=''
if [[ "${PJ_SANITIZE}" != none ]]; then
  # Clang is provisioned per run rather than baked, for the same reason libcups
  # is: baking it would force every developer through a REBUILD_IMAGE that
  # re-bakes Qt and the whole Conan closure. Every instrumented lane compiles with
  # it (build.sh names clang/clang++ explicitly). CC/CXX stay unexported outside
  # msan, so Conan still builds any missing dependency with the profile's GCC.
  sanitizer_bootstrap="${clang_provision}"'
      ln -sfn "/usr/bin/clang-${PJ_SANITIZER_CLANG_VERSION}" /usr/local/bin/clang
      ln -sfn "/usr/bin/clang++-${PJ_SANITIZER_CLANG_VERSION}" /usr/local/bin/clang++
'
fi
if [[ "${PJ_SANITIZE}" == msan ]]; then
  docker volume create "${MSAN_LIBCXX_VOL}" >/dev/null
  docker volume create "${MSAN_CONAN_VOL}" >/dev/null
  msan_mounts=(-v "${MSAN_LIBCXX_VOL}:/opt/msan-libcxx" -v "${MSAN_CONAN_VOL}:/root/.conan2-msan")
  # The instrumented libc++ is built once into a volume and skipped thereafter.
  sanitizer_bootstrap+='
      export CC=clang CXX=clang++
      bash /work/scripts/build_msan_libcxx.sh --prefix /opt/msan-libcxx || exit 3
'
fi
run_cmd=(docker run --rm
  -v "${WORK_ROOT}:/work" -w /work
  ${seccomp_args[@]+"${seccomp_args[@]}"}
  ${msan_mounts[@]+"${msan_mounts[@]}"}
  "${PLUGINS_MOUNT[@]}"
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)"
  -e PJ_VERSION="${PJ_VERSION:-}"
  -e PJ_INSTALLATION="${PJ_INSTALLATION:-}"
  -e PJ_SANITIZER_CLANG_VERSION="${PJ_SANITIZER_CLANG_VERSION}"
  "${build_env[@]}"
  --entrypoint bash
  "${IMAGE_TAG}"
  -c 'if ! ldconfig -p | grep -q "libcups\.so\.2"; then
        echo "==> Provisioning libcups2 for linuxdeploy Qt printsupport"
        apt-get update -qq >/dev/null 2>&1 \
          && apt-get install -y --no-install-recommends libcups2 >/dev/null 2>&1 \
          && ldconfig \
          || echo "WARNING: could not install libcups2; Qt printsupport deployment may fail" >&2
      fi
      '"${sanitizer_bootstrap}"'
      exec /usr/local/bin/pj4-build-entry.sh "$@"' _
  "${FWD_ARGS[@]}")
app_stage_rc=0
"${run_cmd[@]}" || app_stage_rc=$?
if [[ "${app_stage_rc}" -ne 0 ]]; then
  if [[ "${PJ_SANITIZE}" == tsan ]]; then
    echo "==> App: ThreadSanitizer stage exited ${app_stage_rc}." >&2
    tsan_stage_rc="${app_stage_rc}"
  else
    exit "${app_stage_rc}"
  fi
fi

echo ""
if [[ "${PJ_SANITIZE}" == tsan || "${PJ_SANITIZE}" == msan ]]; then
  # No artifact to name: both are test lanes. Naming one anyway is worse than
  # saying nothing — it reports a file that was never written. Re-raise the
  # recorded status so a reported finding still fails the command, now that
  # every stage has run.
  if [[ "${tsan_stage_rc}" -ne 0 ]]; then
    echo "==> ${PJ_SANITIZE} lane finished with findings; see the reports above."
  else
    echo "==> ${PJ_SANITIZE} lane finished clean; no AppImage is produced by a test lane."
  fi
  exit "${tsan_stage_rc}"
fi
# Report the real output name from the app tree's versions.env: build_appimage.sh
# names the file from WORK_ROOT/versions.env, which differs from this repo under
# --app-dir (PJ_VERSION, if set, still overrides). Subshell so sourcing it doesn't
# clobber the image-tag vars read from this repo above.
( source "${WORK_ROOT}/versions.env" 2>/dev/null || true
  echo "==> Done: ${WORK_ROOT}/packaging/appimage/PlotJuggler-${PJ_VERSION:-${PJ_APP_VERSION}}${SANITIZE_INFIX}-${PJ_APPIMAGE_ARCH}.AppImage" )
