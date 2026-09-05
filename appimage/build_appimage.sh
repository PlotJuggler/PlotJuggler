#!/usr/bin/env bash
#
# Build a relocatable PlotJuggler 4 AppImage.
#
# Bundles the plotjuggler4 shell and its Qt 6 + Conan runtime into a single
# self-contained AppImage. Unlike PJ3 (system Qt 5), PJ4 links Qt from ./.qt and
# most external deps from Conan, so linuxdeploy must be pointed at both — that is
# the bulk of the env setup here.
#
# Prerequisites (run these first, they are NOT done here):
#   ./install_qt6.sh      # Qt into ./.qt/<ver>/gcc_64
#   ./build.sh            # builds build/pj_app/plotjuggler4 + Conan env
#
# Plugins are NOT part of this repo — they are built and published separately by
# pj-official-plugins (per-extension marketplace zips) and indexed by the
# pj-plugin-registry. There are two ways to bundle them (and a no-plugin default):
#
#   appimage/build_appimage.sh                       # app-only AppImage
#   appimage/build_appimage.sh --plugins-dir <path>  # LOCAL: copy a folder you
#                                                    #   curated, verbatim, into
#                                                    #   usr/lib/plotjuggler/plugins
#   appimage/build_appimage.sh --plugins-registry [url]
#                                                    # REGISTRY: download the
#                                                    #   curated set (BUNDLE_IDS)
#                                                    #   from the plugin registry,
#                                                    #   verify checksums, unpack
#
# --commit-hash <hash> appends .<hash> to the output filename (PlotJuggler-
# <version>-<arch>.<hash>.AppImage) — used by release CI on workflow_dispatch
# (non-tag) builds, where <version> alone would collide across builds. Tag
# builds omit it, matching the Windows installer's -CleanReleaseName.
#
# Bundled plugins land at usr/lib/plotjuggler/plugins. The app never scans that
# dir directly — at startup it seeds its contents into the writable per-user
# extensions dir (copying new ids, refreshing ones whose bundled version is
# newer) and loads everything from there, so marketplace installs/uninstalls
# work normally and the read-only bundle stays a seed source.
#
# --retro-wad <path> bundles the standalone GPLv2 pj-raster-helper (build it
# with `PJ_BUILD_RASTER_HELPER=ON ./build.sh`) together with <path> as its game
# data and the license texts from 3rdparty/retro/. Omit it and no retro
# payload ships.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
source "${ROOT}/versions.env"

ARCH="${PJ_APPIMAGE_ARCH}"
QT_VERSION="${PJ_QT_VERSION}"
PLATFORM="linux-${ARCH}"   # registry artifact key (registry.json platforms.<key>)

BUILD="${ROOT}/build"
QT_DIR="${ROOT}/.qt/${QT_VERSION}/gcc_64"
APPDIR="${BUILD}/AppDir"
VERSION="${PJ_VERSION:-${PJ_APP_VERSION}}"

# Bundled plugins go where the installed app expects its seed source: <prefix>/
# lib/plotjuggler/plugins, which plotjuggler4 resolves relative to itself
# (usr/bin -> ../lib/plotjuggler/plugins). Kept OUT of usr/plugins on purpose —
# linuxdeploy-plugin-qt deploys Qt's own platform/imageformat plugins there, and
# the seed's plugin scan must never try to dlopen those as PlotJuggler plugins.
PJ_PLUGINS_REL="usr/lib/plotjuggler/plugins"

# The plugin registry to resolve --plugins-registry against. `development` is
# the registry repo's default (and only long-lived) branch — the same ref the
# Windows installer resolves against; override with the optional
# --plugins-registry <url> argument to pin a specific ref.
REGISTRY_URL="https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry/refs/heads/development/registry.json"

# Curated set of registry extension ids bundled by --plugins-registry. The
# registry lists every official extension; this is the subset that ships in the
# AppImage. Keep in lockstep with $PluginIds in
# installer/build_windows_installer.ps1 (same set, plus the Linux-only
# ros2-topic-subscriber). Excluded by request: toolbox-colormap,
# toolbox-reactive-scripts-editor.
BUNDLE_IDS=(
  csv-loader
  mcap-loader
  parquet-loader
  ulog-loader
  mp4-loader
  pointcloud-3d-loader
  dummy-streamer
  foxglove-bridge
  plotjuggler-bridge
  webrtc-client
  ros-parser
  protobuf-parser
  json-parser
  data-tamer-parser
  toolbox-quaternion
  toolbox-transform-editor
  toolbox-mosaico
  # Multi-distro ROS 2 subscriber: the single linux-x86_64 zip carries the
  # distro-agnostic proxy + per-distro inners under dist/<distro>/;
  # registry-mode unpacks it verbatim, no special handling needed.
  ros2-topic-subscriber
)

PLUGINS_MODE="none"        # none | local | registry
PLUGINS_LOCAL_DIR=""
COMMIT_HASH=""
RETRO_WAD=""

# Prints the header comment block, stopping at the first line of real code, so
# it cannot drift out of sync with a fixed line range.
usage() { awk 'NR>1 { if (!/^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --plugins-dir)
      PLUGINS_MODE="local"
      # Canonicalize to an absolute path against the invocation CWD *now*: step 1
      # below cd's into SCRIPT_DIR (appimage/) before collect_plugins_local reads
      # this, so a relative value would otherwise resolve against appimage/ rather
      # than where the user ran the command, and silently miss.
      PLUGINS_LOCAL_DIR="$(realpath -m -- "${2:?--plugins-dir needs a path}")"; shift 2 ;;
    --plugins-registry)
      PLUGINS_MODE="registry"
      # Optional URL argument (anything not starting with '-').
      if [[ -n "${2:-}" && "${2}" != -* ]]; then REGISTRY_URL="$2"; shift; fi
      shift ;;
    --commit-hash)
      COMMIT_HASH="${2:?--commit-hash needs a value}"; shift 2 ;;
    --retro-wad)
      # Canonicalized now for the same reason --plugins-dir is: step 1 cd's into
      # appimage/ before this value is read.
      RETRO_WAD="$(realpath -m -- "${2:?--retro-wad needs a path}")"; shift 2 ;;
    -h | --help)
      usage; exit 0 ;;
    *)
      echo "build_appimage: unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

# ---------------------------------------------------------------------------
# 0. Sanity checks
# ---------------------------------------------------------------------------
[[ -d "${QT_DIR}" ]]                    || { echo "Qt not found at ${QT_DIR}. Run ./install_qt6.sh"; exit 1; }
[[ -x "${BUILD}/pj_app/plotjuggler4" ]] || { echo "plotjuggler4 not built. Run ./build.sh"; exit 1; }
[[ -f "${BUILD}/conanrun.sh" ]]         || { echo "build/conanrun.sh missing. Run ./build.sh"; exit 1; }
command -v wget >/dev/null              || { echo "wget required"; exit 1; }

# ---------------------------------------------------------------------------
# 1. linuxdeploy + its Qt plugin (themselves AppImages, fetched once)
# ---------------------------------------------------------------------------
cd "${SCRIPT_DIR}"
LD="linuxdeploy-${ARCH}.AppImage"
LDQT="linuxdeploy-plugin-qt-${ARCH}.AppImage"
# appimagetool packages the finished AppDir into the AppImage. We invoke it
# directly (instead of linuxdeploy's --output appimage) so plugins can be copied
# in AFTER linuxdeploy has deployed the app's dependency closure — see step 5.
AT="appimagetool-${ARCH}.AppImage"
[[ -f "${LD}" ]]   || wget -q "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/${LD}"
[[ -f "${LDQT}" ]] || wget -q "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/${LDQT}"
[[ -f "${AT}" ]]   || wget -q "https://github.com/AppImage/appimagetool/releases/download/continuous/${AT}"
chmod +x "${LD}" "${LDQT}" "${AT}"

# ---------------------------------------------------------------------------
# 2. Assemble the AppDir skeleton
# ---------------------------------------------------------------------------
rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr/bin" "${APPDIR}/${PJ_PLUGINS_REL}"
# plotjuggler4 itself is installed by linuxdeploy via --executable below (it copies
# the binary into usr/bin and deploys its Qt + Conan dependency closure).

# Icon: committed 256x256 raster (resources/svg/plotjuggler4.png), rendered once
# from resources/svg/plotjuggler.svg with the pinned Qt6Svg. No host rasterizer
# (ImageMagick/rsvg/inkscape) is required at build time.
ICON_PNG="${ROOT}/resources/svg/plotjuggler4.png"
[[ -f "${ICON_PNG}" ]] || { echo "ERROR: committed icon ${ICON_PNG} is missing"; exit 1; }

# ---------------------------------------------------------------------------
# 3. Plugin-bundling helpers (per the selected mode), copying into per-id subdirs
#    of ${PJ_PLUGINS_REL}. The app scans this tree recursively. Published plugins
#    are self-contained (heavy deps static-linked), so no dependency deployment
#    is needed — a plain copy/unpack is the install. These run AFTER linuxdeploy
#    (step 5), never before: linuxdeploy would otherwise try to resolve every
#    bundled .so's dependency closure and abort on a plugin whose deps are
#    intentionally external — e.g. the ROS 2 subscriber's per-distro inner under
#    dist/<distro>/, which binds to the user's *sourced* system ROS and must NOT
#    be self-contained.
# ---------------------------------------------------------------------------
collect_plugins_local() {
  [[ -d "${PLUGINS_LOCAL_DIR}" ]] || { echo "ERROR: --plugins-dir '${PLUGINS_LOCAL_DIR}' is not a directory"; exit 1; }
  echo "Plugins: copying local folder '${PLUGINS_LOCAL_DIR}' -> ${PJ_PLUGINS_REL}"
  cp -a "${PLUGINS_LOCAL_DIR}/." "${APPDIR}/${PJ_PLUGINS_REL}/"
}

collect_plugins_registry() {
  command -v python3   >/dev/null || { echo "python3 required for --plugins-registry"; exit 1; }
  command -v unzip     >/dev/null || { echo "unzip required for --plugins-registry"; exit 1; }
  command -v sha256sum >/dev/null || { echo "sha256sum required for --plugins-registry"; exit 1; }

  local registry_json; registry_json="$(mktemp)"
  echo "Plugins: fetching registry ${REGISTRY_URL}"
  wget -qO "${registry_json}" "${REGISTRY_URL}" || { echo "ERROR: failed to download registry"; exit 1; }

  local id url checksum want got zip dest
  for id in "${BUNDLE_IDS[@]}"; do
    # Resolve this extension's url + checksum for our platform from the registry.
    read -r url checksum < <(python3 - "${registry_json}" "${id}" "${PLATFORM}" <<'PY'
import json, sys
reg, want_id, plat = sys.argv[1], sys.argv[2], sys.argv[3]
data = json.load(open(reg))
for ext in data.get("extensions", []):
    if ext.get("id") == want_id:
        p = ext.get("platforms", {}).get(plat)
        if p:
            print(p.get("url", ""), p.get("checksum", ""))
        break
PY
)
    [[ -n "${url}" ]] || { echo "ERROR: registry has no '${id}' for ${PLATFORM}"; exit 1; }

    zip="$(mktemp --suffix=.zip)"
    echo "  ${id}: ${url}"
    wget -qO "${zip}" "${url}" || { echo "ERROR: download failed for '${id}'"; exit 1; }

    want="${checksum#sha256:}"   # registry value is 'sha256:<hex>'
    if [[ -n "${want}" ]]; then
      got="$(sha256sum "${zip}" | awk '{print $1}')"
      [[ "${got}" == "${want}" ]] || { echo "ERROR: checksum mismatch for '${id}' (want ${want}, got ${got})"; exit 1; }
    fi

    dest="${APPDIR}/${PJ_PLUGINS_REL}/${id}"
    mkdir -p "${dest}"
    unpack="$(mktemp -d)"
    unzip -oq "${zip}" -d "${unpack}"
    # Official archives carry one top-level <id>/ directory. Normalize it away
    # (same as the Windows installer) so the bundled layout is always
    # plugins/<id>/{manifest.json,...} — the layout the release CI's glibc
    # audit expects when it exempts ros2-topic-subscriber/dist/<distro>/.
    payload="${unpack}"
    entries=( "${unpack}"/* )
    if [[ ${#entries[@]} -eq 1 && -d "${entries[0]}" ]]; then
      payload="${entries[0]}"
    fi
    cp -a "${payload}/." "${dest}/"
    rm -rf "${unpack}" "${zip}"
  done
  rm -f "${registry_json}"
  echo "Plugins: bundled ${#BUNDLE_IDS[@]} extension(s) from the registry for ${PLATFORM}"
}

# ---------------------------------------------------------------------------
# 4. Runtime env so linuxdeploy resolves Qt (from ./.qt) and Conan deps.
#    conanrun.sh exports LD_LIBRARY_PATH covering every Conan package this build
#    links (FFmpeg, cloudini, …); without it linuxdeploy drops those libs.
# ---------------------------------------------------------------------------
# shellcheck disable=SC1091
source "${BUILD}/conanrun.sh"
export QMAKE="${QT_DIR}/bin/qmake6"
export PATH="${QT_DIR}/bin:${PATH}"
export LD_LIBRARY_PATH="${QT_DIR}/lib:${LD_LIBRARY_PATH:-}"

# ---------------------------------------------------------------------------
# 5. Deploy, then bundle plugins, then package — in that order.
#
#    linuxdeploy populates the AppDir with the app binary plus its Qt + Conan
#    dependency closure (and installs the custom AppRun / desktop / icon), but is
#    NOT given --output appimage. Plugins are copied in only AFTER that deploy, so
#    linuxdeploy never walks their .so closures: self-contained plugins need no
#    deployment, and a plugin with intentionally-external deps (the ROS 2 inner)
#    would make linuxdeploy abort trying to resolve ROS libs that must come from
#    the user's sourced system ROS at runtime. appimagetool then packages the
#    finished AppDir verbatim without re-scanning dependencies.
# ---------------------------------------------------------------------------
cd "${SCRIPT_DIR}"
"./${LD}" \
  --appdir "${APPDIR}" \
  --executable "${BUILD}/pj_app/plotjuggler4" \
  --desktop-file "${SCRIPT_DIR}/plotjuggler4.desktop" \
  --icon-file "${ICON_PNG}" \
  --custom-apprun "${SCRIPT_DIR}/AppRun.sh" \
  --plugin qt

# Scalable icon next to the 256px raster, so HiDPI docks and app grids render
# the logo crisp instead of upscaling the PNG. Named after the desktop file's
# Icon= key like the PNG.
install -Dm644 "${ROOT}/resources/svg/plotjuggler.svg" \
  "${APPDIR}/usr/share/icons/hicolor/scalable/apps/plotjuggler4.svg"

case "${PLUGINS_MODE}" in
  local)    collect_plugins_local ;;
  registry) collect_plugins_registry ;;
  none)     echo "Plugins: none (app-only AppImage; pass --plugins-dir or --plugins-registry to bundle)" ;;
esac

# ---------------------------------------------------------------------------
# 5a-bis. Plugin-admission helper.
#     The marketplace runs the first dlopen of a downloaded plugin inside
#     pj-plugin-check instead of the application process. ExtensionManager fails
#     closed when the helper cannot be started, so a bundle without it rejects
#     every marketplace install. It has to sit next to the app binary, which is
#     where PluginCheckRunner looks (applicationDirPath()).
#
#     Staged after linuxdeploy on purpose: the AppDir is packaged verbatim from
#     here on. That is safe because the helper links no Qt — only libstdc++,
#     libgcc_s, libm and libc, all already in the closure linuxdeploy resolved
#     for the app.
# ---------------------------------------------------------------------------
PLUGIN_CHECK="${BUILD}/pj_app/pj-plugin-check"
[[ -x "${PLUGIN_CHECK}" ]] || { echo "ERROR: ${PLUGIN_CHECK} missing — build the pj-plugin-check target; without it the bundle rejects every marketplace install"; exit 1; }
install -m 0755 "${PLUGIN_CHECK}" "${APPDIR}/usr/bin/pj-plugin-check"
echo "Plugin-admission helper: bundled pj-plugin-check"

# ---------------------------------------------------------------------------
# 5b. Bundle the embedded CPython stdlib for the Python Data Processor backend.
#     The app bakes PYTHONHOME to the BUILD host's Conan cpython path
#     (PJ_PYTHON_HOME), which does not exist on any other machine, so CPython
#     fails to find its stdlib (encodings, …) and Python filters die with
#     "Failed to init CPython". AppRun overrides PYTHONHOME to <AppDir>/usr, so
#     ship the stdlib at usr/lib/pythonX.Y. pj_scripting/CMakeLists.txt records
#     the prefix in build/pj_python_home.txt.
#
#     Only the stdlib needs this explicit copy. libpython3.12.so.1.0 itself
#     arrives on its own: the app declares it DT_NEEDED, so linuxdeploy pulls it
#     in with the rest of the shared-library closure. Do not "simplify" the copy
#     below by assuming linuxdeploy covers the stdlib too — those are plain data
#     files that no ELF references, and dropping them breaks every Python filter.
# ---------------------------------------------------------------------------
PY_HOME_FILE="${BUILD}/pj_python_home.txt"
if [[ -f "${PY_HOME_FILE}" ]]; then
  py_prefix="$(head -n1 "${PY_HOME_FILE}")"
  py_stdlib="$(ls -d "${py_prefix}"/lib/python3.* 2>/dev/null | head -n1)"
  if [[ -n "${py_stdlib}" && -d "${py_stdlib}" ]]; then
    py_ver="$(basename "${py_stdlib}")"   # e.g. python3.12
    echo "Python: bundling stdlib ${py_stdlib} -> usr/lib/${py_ver}"
    mkdir -p "${APPDIR}/usr/lib"
    cp -a "${py_stdlib}" "${APPDIR}/usr/lib/${py_ver}"
  else
    echo "WARNING: Python stdlib not found under '${py_prefix}/lib/python3.*' — Python Data Processors will fail at runtime" >&2
  fi
else
  echo "WARNING: ${PY_HOME_FILE} missing — cannot bundle Python stdlib (Python Data Processors will fail)" >&2
fi

# ---------------------------------------------------------------------------
# 5c. Retro payload (only with --retro-wad): the standalone GPLv2
#     pj-raster-helper, its game data, and the license texts that must travel
#     with them. PlotJuggler links none of it — the helper is launched as a
#     child process, so it ships as plain files under the app binary's own
#     directory, which is where MainWindow::openEmbeddedConsole looks
#     (applicationDirPath()/3rdparty/retro/). Landing it under usr/bin also
#     means deb/build_deb.sh, which copies usr/bin wholesale, inherits it.
#
#     Staged AFTER linuxdeploy for the same reason plugins are: the helper's
#     entire dependency closure (Qt Core + Network) is already deployed for the
#     app, so there is nothing for linuxdeploy to add by walking it again.
# ---------------------------------------------------------------------------
if [[ -n "${RETRO_WAD}" ]]; then
  RETRO_HELPER="${BUILD}/raster_helper/pj-raster-helper"
  [[ -x "${RETRO_HELPER}" ]] || { echo "ERROR: ${RETRO_HELPER} missing — rebuild with PJ_BUILD_RASTER_HELPER=ON ./build.sh"; exit 1; }
  [[ -f "${RETRO_WAD}" ]]    || { echo "ERROR: --retro-wad '${RETRO_WAD}' is not a file"; exit 1; }

  RETRO_DIR="${APPDIR}/usr/bin/3rdparty/retro"
  mkdir -p "${RETRO_DIR}"
  install -m 0755 "${RETRO_HELPER}" "${RETRO_DIR}/pj-raster-helper"
  install -m 0644 "${RETRO_WAD}" "${RETRO_DIR}/base.wad"
  # GPLv2 section 3 (written offer) and the shareware terms are conditions of
  # shipping these two files at all. Only the trigger is hidden; the licenses
  # are not — see 3rdparty/retro/README.md.
  install -m 0644 "${ROOT}/3rdparty/retro/COPYING" \
                  "${ROOT}/3rdparty/retro/SOURCE-OFFER.txt" \
                  "${ROOT}/3rdparty/retro/SHAREWARE-LICENSE.txt" \
                  "${ROOT}/3rdparty/retro/README.md" \
                  "${RETRO_DIR}/"
  # The build-tree binary points at this host's Qt and Conan trees. Repoint it
  # at the deployed closure so it resolves Qt on its own; the launcher's
  # LD_LIBRARY_PATH (which the child inherits) then stops being the only thing
  # keeping it alive.
  if command -v patchelf >/dev/null; then
    patchelf --set-rpath '$ORIGIN/../../../lib' "${RETRO_DIR}/pj-raster-helper"
  else
    echo "WARNING: patchelf not found — pj-raster-helper keeps its build-tree RUNPATH" >&2
  fi
  # linuxdeploy strips everything it deploys; match it rather than shipping
  # debug info for a payload nobody debugs from a release build.
  if command -v strip >/dev/null; then
    strip "${RETRO_DIR}/pj-raster-helper"
  fi
  echo "Retro: staged pj-raster-helper + $(basename "${RETRO_WAD}") -> usr/bin/3rdparty/retro"
fi

if [[ -n "${COMMIT_HASH}" ]]; then
  OUTPUT="${SCRIPT_DIR}/PlotJuggler-${VERSION}-${ARCH}.${COMMIT_HASH}.AppImage"
else
  OUTPUT="${SCRIPT_DIR}/PlotJuggler-${VERSION}-${ARCH}.AppImage"
fi
ARCH="${ARCH}" "./${AT}" "${APPDIR}" "${OUTPUT}"

echo ""
echo "Done: ${OUTPUT}"
