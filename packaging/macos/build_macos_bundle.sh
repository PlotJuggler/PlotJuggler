#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Repackages a built PlotJuggler 4 tree into PlotJuggler.app and a .dmg (macOS
# arm64). Same role as packaging/appimage/build_appimage.sh: it compiles nothing.
#
#   build_macos_bundle.sh --qt-dir <qt kit> [--build-dir build] [--out-dir dist]
#                         [--version X.Y.Z] [--plugins-registry] [--sign <identity>]
#                         [--notarize]
#
#   --qt-dir            the Qt kit the tree was built with (.qt/<PJ_QT_VERSION>/macos)
#   --plugins-registry  bundle the curated official plugins from pj-plugin-registry
#   --sign <identity>   a "Developer ID Application" identity in the keychain;
#                       default "-" (ad-hoc: runs, but Gatekeeper blocks a download
#                       until the user picks Open Anyway)
#   --notarize          submit the signed .dmg to Apple and staple the ticket; needs
#                       APPLE_ID, APPLE_TEAM_ID and APPLE_APP_PASSWORD
#
# Layout: Contents/MacOS holds plotjuggler4 and pj-plugin-check, Frameworks the Qt
# and Conan libraries (macdeployqt), PlugIns the Qt plugins, Resources/plugins the
# bundled PlotJuggler plugins (ExtensionCatalogService's seed source; codesign
# refuses their manifest.json under PlugIns) and Resources/python the embedded
# CPython stdlib (main.cpp points PYTHONHOME there). The raster helper is left
# out.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BUILD="${ROOT}/build"
QT_DIR=""
OUT_DIR="${ROOT}/dist"
VERSION=""
BUNDLE_PLUGINS=0
SIGN_IDENTITY="-"
NOTARIZE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD="$(cd "$2" && pwd)"; shift 2 ;;
    --qt-dir) QT_DIR="$(cd "$2" && pwd)"; shift 2 ;;
    --out-dir) OUT_DIR="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --plugins-registry) BUNDLE_PLUGINS=1; shift ;;
    --sign) SIGN_IDENTITY="$2"; shift 2 ;;
    --notarize) NOTARIZE=1; shift ;;
    -h | --help) sed -n '3,24p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

die() { echo "ERROR: $*" >&2; exit 1; }

[[ "$(uname -s)" == Darwin ]] || die "macOS only"
[[ -n "${QT_DIR}" ]] || die "--qt-dir is required"
[[ -x "${QT_DIR}/bin/macdeployqt" ]] || die "no macdeployqt in ${QT_DIR}/bin"
[[ -x "${BUILD}/pj_app/plotjuggler4" ]] || die "no ${BUILD}/pj_app/plotjuggler4: build the pj_app target first"
if [[ "${NOTARIZE}" == 1 ]]; then
  [[ "${SIGN_IDENTITY}" != "-" ]] || die "--notarize needs --sign with a Developer ID identity"
  for var in APPLE_ID APPLE_TEAM_ID APPLE_APP_PASSWORD; do
    [[ -n "${!var:-}" ]] || die "--notarize needs ${var}"
  done
fi
if [[ -z "${VERSION}" ]]; then
  # shellcheck disable=SC1091
  VERSION="$(source "${ROOT}/versions.env" && echo "${PJ_APP_VERSION}")"
fi

# Same curated set as the Windows installer ($PluginIds in
# packaging/installer/build_windows_installer.ps1): ros2-topic-subscriber is
# Linux-only, and toolbox-assistant-agent ships on Linux only for now.
BUNDLE_IDS=(
  csv-loader mcap-loader parquet-loader ulog-loader mp4-loader pointcloud-3d-loader
  dummy-streamer foxglove-bridge plotjuggler-bridge webrtc-client
  ros-parser protobuf-parser json-parser data-tamer-parser
  toolbox-quaternion toolbox-transform-editor toolbox-mosaico arrow-parser
)
PLATFORM="macos-arm64"
REGISTRY_URL="https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry/refs/heads/development/registry.json"

STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT
APP="${STAGE}/PlotJuggler.app"
CONTENTS="${APP}/Contents"
mkdir -p "${CONTENTS}/MacOS" "${CONTENTS}/Resources/plugins"

# ---------------------------------------------------------------------------
# 1. Executables, icon, Python stdlib.
# ---------------------------------------------------------------------------
install -m 755 "${BUILD}/pj_app/plotjuggler4" "${CONTENTS}/MacOS/plotjuggler4"
[[ -x "${BUILD}/pj_app/pj-plugin-check" ]] || die "no pj-plugin-check next to the app: the marketplace fails closed without it"
install -m 755 "${BUILD}/pj_app/pj-plugin-check" "${CONTENTS}/MacOS/pj-plugin-check"

iconset="${STAGE}/plotjuggler.iconset"
mkdir -p "${iconset}"
icon_png="${ROOT}/resources/svg/plotjuggler4.png"   # 256 px: the largest raster the repo has
for size in 16 32 128 256; do
  sips -z "${size}" "${size}" "${icon_png}" --out "${iconset}/icon_${size}x${size}.png" >/dev/null
done
for size in 16 32 128; do
  sips -z $((size * 2)) $((size * 2)) "${icon_png}" --out "${iconset}/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "${iconset}" -o "${CONTENTS}/Resources/plotjuggler.icns"

# PYTHONHOME layout: <home>/lib/python3.X. pj_scripting/CMakeLists.txt records
# the Conan cpython prefix the build linked.
py_prefix="$(head -n1 "${BUILD}/pj_python_home.txt" 2>/dev/null || true)"
py_stdlib="$(ls -d "${py_prefix}"/lib/python3.* 2>/dev/null | head -n1 || true)"
[[ -n "${py_stdlib}" ]] || die "no Python stdlib under '${py_prefix}/lib/python3.*' (${BUILD}/pj_python_home.txt)"
mkdir -p "${CONTENTS}/Resources/python/lib"
cp -R "${py_stdlib}" "${CONTENTS}/Resources/python/lib/"
find "${CONTENTS}/Resources/python" -name __pycache__ -type d -prune -exec rm -rf {} +
echo "Python: bundled $(basename "${py_stdlib}")"

# macdeployqt finds the binary through CFBundleExecutable, so the plist comes
# first; step 4 sets LSMinimumSystemVersion.
cat > "${CONTENTS}/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>plotjuggler4</string>
  <key>CFBundleIdentifier</key><string>io.plotjuggler.application</string>
  <key>CFBundleName</key><string>PlotJuggler</string>
  <key>CFBundleDisplayName</key><string>PlotJuggler</string>
  <key>CFBundleIconFile</key><string>plotjuggler</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${VERSION}</string>
  <key>CFBundleVersion</key><string>${VERSION}</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.developer-tools</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
PLIST
plutil -lint "${CONTENTS}/Info.plist" >/dev/null

# ---------------------------------------------------------------------------
# 2. Qt and Conan libraries into Frameworks, install names rewritten.
#    The stdlib's extension modules are deployed too: they link Conan's
#    OpenSSL, libffi, ... by absolute or @rpath names.
# ---------------------------------------------------------------------------
deploy_args=("-executable=${CONTENTS}/MacOS/pj-plugin-check")
while IFS= read -r -d '' ext; do
  deploy_args+=("-executable=${ext}")
done < <(find "${CONTENTS}/Resources/python" -name '*.so' -print0)
"${QT_DIR}/bin/macdeployqt" "${APP}" "${deploy_args[@]}" -verbose=1

is_macho() { file -b "$1" | grep -q 'Mach-O'; }

# Every library a bundled Mach-O loads must resolve inside the bundle or the OS.
missing=0
while IFS= read -r -d '' bin; do
  is_macho "${bin}" || continue
  # Universal (fat) Mach-O files print one "<file> (architecture X):" header per
  # slice; dependencies and the install name are the indented / non-header lines.
  own_id="$(otool -D "${bin}" | grep -v ':$' | head -n1 || true)"
  while IFS= read -r dep; do
    [[ "${dep}" == "${own_id}" ]] && continue
    case "${dep}" in
      /System/* | /usr/lib/* | @executable_path/* | @loader_path/* | @rpath/*) ;;
      *) echo "  ${bin#"${APP}/"} -> ${dep}" >&2; missing=1 ;;
    esac
  done < <(otool -L "${bin}" | awk '/^\t/{print $1}')
done < <(find "${APP}" -type f -print0)
[[ "${missing}" == 0 ]] || die "the bundle links libraries outside itself (listed above)"

# ---------------------------------------------------------------------------
# 3. Plugins: verified downloads from the registry, after macdeployqt so it
#    never rewrites them (they are self-contained).
# ---------------------------------------------------------------------------
if [[ "${BUNDLE_PLUGINS}" == 1 ]]; then
  registry_json="${STAGE}/registry.json"
  curl -fsSL "${REGISTRY_URL}" -o "${registry_json}" || die "failed to download ${REGISTRY_URL}"
  for id in "${BUNDLE_IDS[@]}"; do
    url="" checksum=""
    read -r url checksum < <(python3 - "${registry_json}" "${id}" "${PLATFORM}" <<'PY'
import json, sys
reg, want_id, plat = sys.argv[1:4]
for ext in json.load(open(reg)).get("extensions", []):
    if ext.get("id") == want_id:
        p = ext.get("platforms", {}).get(plat) or {}
        print(p.get("url", ""), p.get("checksum", ""))
        break
PY
    ) || true
    [[ -n "${url:-}" ]] || die "the registry has no '${id}' for ${PLATFORM}"
    zip="${STAGE}/${id}.zip"
    curl -fsSL "${url}" -o "${zip}" || die "download failed for '${id}'"
    want="${checksum#sha256:}"
    got="$(shasum -a 256 "${zip}" | awk '{print $1}')"
    [[ -z "${want}" || "${got}" == "${want}" ]] || die "checksum mismatch for '${id}' (want ${want}, got ${got})"
    unpack="${STAGE}/unpack-${id}"
    mkdir -p "${unpack}"
    unzip -oq "${zip}" -d "${unpack}"
    # Official archives hold one top-level <id>/ directory: normalize to <id>/{manifest.json,...}.
    payload="${unpack}"
    entries=("${unpack}"/*)
    if [[ ${#entries[@]} -eq 1 && -d "${entries[0]}" ]]; then payload="${entries[0]}"; fi
    mkdir -p "${CONTENTS}/Resources/plugins/${id}"
    cp -R "${payload}/." "${CONTENTS}/Resources/plugins/${id}/"
    rm -rf "${unpack}" "${zip}"
    echo "  plugin ${id}"
  done
  echo "Plugins: bundled ${#BUNDLE_IDS[@]} extension(s) for ${PLATFORM}"
fi

# ---------------------------------------------------------------------------
# 4. LSMinimumSystemVersion: the highest deployment target of
#    any bundled Mach-O (Qt's floor, unless a Conan library asks for more).
# ---------------------------------------------------------------------------
min_macos="11.0"
while IFS= read -r -d '' bin; do
  is_macho "${bin}" || continue
  while IFS= read -r v; do
    if [[ "$(printf '%s\n%s\n' "${min_macos}" "${v}" | sort -V | tail -n1)" == "${v}" ]]; then min_macos="${v}"; fi
  done < <(otool -l "${bin}" | awk '/LC_BUILD_VERSION/{b=1} b && $1=="minos"{print $2; b=0}')
done < <(find "${APP}" -type f -print0)
echo "Minimum macOS: ${min_macos}"

plutil -replace LSMinimumSystemVersion -string "${min_macos}" "${CONTENTS}/Info.plist"

# ---------------------------------------------------------------------------
# 5. Sign inside out (no --deep): every nested Mach-O, the frameworks, the
#    helper, then the app. Hardened runtime throughout; the app and the helper
#    load plugins signed by other teams (or ad-hoc), hence the entitlement.
# ---------------------------------------------------------------------------
entitlements="${SCRIPT_DIR}/entitlements.plist"
sign_args=(--force --options runtime --sign "${SIGN_IDENTITY}")
if [[ "${SIGN_IDENTITY}" == "-" ]]; then sign_args+=(--timestamp=none); else sign_args+=(--timestamp); fi

while IFS= read -r -d '' bin; do
  is_macho "${bin}" || continue
  case "${bin}" in "${CONTENTS}/MacOS/"*) continue ;; esac
  codesign "${sign_args[@]}" "${bin}"
done < <(find "${APP}" -type f -print0)
while IFS= read -r -d '' framework; do
  codesign "${sign_args[@]}" "${framework}"
done < <(find "${CONTENTS}/Frameworks" -name '*.framework' -type d -prune -print0)
codesign "${sign_args[@]}" --entitlements "${entitlements}" "${CONTENTS}/MacOS/pj-plugin-check"
codesign "${sign_args[@]}" --entitlements "${entitlements}" "${APP}"
codesign --verify --deep --strict --verbose=2 "${APP}"

# ---------------------------------------------------------------------------
# 6. Disk image (app + Applications link), signed and notarized when asked.
# ---------------------------------------------------------------------------
mkdir -p "${OUT_DIR}"
dmg="${OUT_DIR}/PlotJuggler-${VERSION}-macOS-arm64.dmg"
dmg_root="${STAGE}/dmg"
mkdir -p "${dmg_root}"
mv "${APP}" "${dmg_root}/"
ln -s /Applications "${dmg_root}/Applications"
rm -f "${dmg}"
hdiutil create -volname "PlotJuggler ${VERSION}" -srcfolder "${dmg_root}" -fs HFS+ -format UDZO -ov "${dmg}"
if [[ "${SIGN_IDENTITY}" != "-" ]]; then
  codesign --force --timestamp --sign "${SIGN_IDENTITY}" "${dmg}"
fi
if [[ "${NOTARIZE}" == 1 ]]; then
  xcrun notarytool submit "${dmg}" --apple-id "${APPLE_ID}" --team-id "${APPLE_TEAM_ID}" \
    --password "${APPLE_APP_PASSWORD}" --wait --timeout 30m
  xcrun stapler staple "${dmg}"
  spctl --assess --type open --context context:primary-signature --verbose=2 "${dmg}"
fi
echo "Built ${dmg}"
