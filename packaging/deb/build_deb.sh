#!/usr/bin/env bash
#
# Build a Debian/Ubuntu package (.deb) for PlotJuggler 4.
#
# This does NOT compile anything. It repackages the AppDir that
# packaging/appimage/build_appimage.sh already produced (build/AppDir): a complete,
# relocatable tree carrying the app, Qt 6, the Conan closure, the embedded
# CPython stdlib and the bundled plugins. Run that first:
#
#   ./build.sh
#   packaging/appimage/build_appimage.sh --plugins-registry
#   packaging/deb/build_deb.sh
#
# The package is a BUNDLED ("vendor") .deb in the Chrome / VS Code mould —
# everything under /opt/plotjuggler4 plus a wrapper in /usr/bin. A distro-native
# package is impossible: PJ4 pins Qt 6.11.1 (no Ubuntu ships it) and much of the
# Conan graph is not packaged by Debian at all. See
# docs/plans/2026-08-03-debian-packaging.md.
#
# Options:
#   --appdir <path>       AppDir to package (default: build/AppDir)
#   --binary <path>       Replace the AppDir's plotjuggler4 with this one, then
#                         strip it and restore the deployed RUNPATH, matching
#                         what linuxdeploy did to the copy it replaces. Used by
#                         release CI to ship a binary stamped
#                         PJ_INSTALLATION=deb rather than the AppImage-stamped
#                         one. Requires patchelf and strip.
#   --output-dir <dir>    Where to write the .deb (default: packaging/deb/)
#   --commit-hash <hash>  Mark a non-tag build: version becomes <ver>~<hash>,
#                         which sorts BELOW the plain version.
set -euo pipefail

# The staged tree's modes become the installed modes, and mkdir/redirects
# inherit the caller's umask — under umask 077 the app would install 0700 and
# be unusable by non-root users. Pin the 0755/0644 policy here.
umask 022

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
source "${ROOT}/versions.env"

APPDIR="${ROOT}/build/AppDir"
BINARY_OVERRIDE=""
OUTPUT_DIR="${SCRIPT_DIR}"
COMMIT_HASH=""

PACKAGE="plotjuggler4"
# Install prefix inside the package. Bundled libraries make /opt the correct
# home per FHS; /usr/bin gets a wrapper only.
PREFIX="/opt/${PACKAGE}"

# Prints the header comment block, stopping at the first line of real code, so
# it cannot drift out of sync with a fixed line range.
usage() { awk 'NR>1 { if (!/^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --appdir)      APPDIR="$(realpath -m -- "${2:?--appdir needs a path}")"; shift 2 ;;
    --binary)      BINARY_OVERRIDE="$(realpath -m -- "${2:?--binary needs a path}")"; shift 2 ;;
    --output-dir)  OUTPUT_DIR="$(realpath -m -- "${2:?--output-dir needs a path}")"; shift 2 ;;
    --commit-hash) COMMIT_HASH="${2:?--commit-hash needs a value}"; shift 2 ;;
    -h | --help)   usage; exit 0 ;;
    *) echo "build_deb: unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

# ---------------------------------------------------------------------------
# 0. Sanity checks
# ---------------------------------------------------------------------------
[[ -d "${APPDIR}" ]]                     || { echo "ERROR: AppDir not found at ${APPDIR}. Run packaging/appimage/build_appimage.sh first." >&2; exit 1; }
[[ -x "${APPDIR}/usr/bin/plotjuggler4" ]] || { echo "ERROR: ${APPDIR}/usr/bin/plotjuggler4 missing — is this a finished AppDir?" >&2; exit 1; }
command -v dpkg-deb >/dev/null           || { echo "ERROR: dpkg-deb required (apt install dpkg)" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Version -> Debian version
#    Tag builds pass the tag through PJ_VERSION; otherwise versions.env wins.
#    A dispatch build appends ~<hash>: '~' sorts BELOW the bare version, so a
#    pre-release artifact never looks newer than the release it precedes.
# ---------------------------------------------------------------------------
VERSION="${PJ_VERSION:-${PJ_APP_VERSION}}"
VERSION="${VERSION#v}"
VERSION="${VERSION#V}"
[[ -n "${COMMIT_HASH}" ]] && VERSION="${VERSION}~${COMMIT_HASH}"
DEB_VERSION="${VERSION}-1"
# dpkg's own grammar for an upstream version, enforced here so a malformed tag
# fails loudly instead of producing an uninstallable package.
[[ "${DEB_VERSION}" =~ ^[0-9][A-Za-z0-9.+~-]*$ ]] \
  || { echo "ERROR: '${DEB_VERSION}' is not a valid Debian version" >&2; exit 1; }

case "${PJ_APPIMAGE_ARCH}" in
  x86_64)  DEB_ARCH="amd64" ;;
  aarch64) DEB_ARCH="arm64" ;;
  *) echo "ERROR: no Debian architecture mapping for '${PJ_APPIMAGE_ARCH}'" >&2; exit 1 ;;
esac

# ---------------------------------------------------------------------------
# 2. Soname -> Ubuntu/Debian package map for the Depends derivation (step 4).
# ---------------------------------------------------------------------------
declare -A SONAME_PKG=(
  [ld-linux-x86-64.so.2]=libc6      [libc.so.6]=libc6
  [libm.so.6]=libc6                 [libdl.so.2]=libc6
  [libpthread.so.0]=libc6           [libresolv.so.2]=libc6
  [librt.so.1]=libc6                [libutil.so.1]=libc6
  [libgcc_s.so.1]=libgcc-s1         [libstdc++.so.6]=libstdc++6
  [libz.so.1]=zlib1g                [libcom_err.so.2]=libcom-err2
  [libEGL.so.1]=libegl1             [libGL.so.1]=libgl1
  [libGLX.so.0]=libglx0             [libOpenGL.so.0]=libopengl0
  [libX11.so.6]=libx11-6            [libX11-xcb.so.1]=libx11-xcb1
  [libxcb.so.1]=libxcb1             [libdrm.so.2]=libdrm2
  [libfontconfig.so.1]=libfontconfig1
  [libfreetype.so.6]=libfreetype6
  # Pulled in by the embedded CPython's _curses / _curses_panel extensions.
  # One package provides both ncursesw and panelw.
  [libncursesw.so.6]=libncursesw6   [libpanelw.so.6]=libncursesw6
  [libtinfo.so.6]=libtinfo6
  # Needed by the bundled libgcrypt (jammy's libsystemd links it); libgpg-error
  # itself is on linuxdeploy's excludelist, so the system must provide it.
  [libgpg-error.so.0]=libgpg-error0
)

# ---------------------------------------------------------------------------
# 3. Stage the package tree
# ---------------------------------------------------------------------------
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT
# mktemp -d creates 0700 regardless of umask; the root's mode is recorded in
# the archive like every other directory.
chmod 0755 "${STAGE}"
PREFIX_DIR="${STAGE}${PREFIX}"
mkdir -p "${PREFIX_DIR}" "${STAGE}/usr/bin" \
         "${STAGE}/usr/share/applications" \
         "${STAGE}/usr/share/icons/hicolor/256x256/apps" \
         "${STAGE}/usr/share/icons/hicolor/scalable/apps" \
         "${STAGE}/usr/share/doc/${PACKAGE}" \
         "${STAGE}/DEBIAN"

# Payload: bin (incl. linuxdeploy's qt.conf, which keeps Qt's plugin and
# translation lookup relative to the executable), the whole lib closure —
# which carries lib/plotjuggler/plugins and the CPython stdlib with it — Qt's
# own plugins, and translations.
mkdir -p "${PREFIX_DIR}/bin"
cp -a "${APPDIR}/usr/bin/." "${PREFIX_DIR}/bin/"
cp -a "${APPDIR}/usr/lib"   "${PREFIX_DIR}/lib"
if [[ -d "${APPDIR}/usr/plugins" ]]; then
  cp -a "${APPDIR}/usr/plugins" "${PREFIX_DIR}/plugins"
fi
if [[ -d "${APPDIR}/usr/translations" ]]; then
  cp -a "${APPDIR}/usr/translations" "${PREFIX_DIR}/translations"
fi

# Bundled-library copyright files stay under the private prefix. linuxdeploy
# names them after the DISTRO packages it harvested them from, so installing
# them into /usr/share/doc would make this package claim paths owned by
# libglib2.0-0t64, libkrb5support0 and ~36 others.
if [[ -d "${APPDIR}/usr/share/doc" ]]; then
  mkdir -p "${PREFIX_DIR}/share"
  cp -a "${APPDIR}/usr/share/doc" "${PREFIX_DIR}/share/doc"
fi

# Only these three cross into the system tree — nothing else owns them.
cp -a "${APPDIR}/usr/share/applications/plotjuggler4.desktop" \
      "${STAGE}/usr/share/applications/"
cp -a "${APPDIR}/usr/share/icons/hicolor/256x256/apps/plotjuggler4.png" \
      "${STAGE}/usr/share/icons/hicolor/256x256/apps/"
cp -a "${APPDIR}/usr/share/icons/hicolor/scalable/apps/plotjuggler4.svg" \
      "${STAGE}/usr/share/icons/hicolor/scalable/apps/"

# cp -a preserved the AppDir's modes, which carry the build host's umask (a
# dev checkout easily yields 0775 dirs). A root-owned install must not be
# group/world-writable; stripping write bits is safe — the payload has no
# intentionally writable path.
chmod -R go-w "${STAGE}"

# ---------------------------------------------------------------------------
# 3b. Optional binary swap (release CI ships a PJ_INSTALLATION=deb build).
#     A build-tree binary carries machine-specific Conan/Qt RUNPATH entries;
#     restore the value linuxdeploy sets on the deployed copy.
# ---------------------------------------------------------------------------
if [[ -n "${BINARY_OVERRIDE}" ]]; then
  [[ -x "${BINARY_OVERRIDE}" ]] || { echo "ERROR: --binary '${BINARY_OVERRIDE}' is not executable" >&2; exit 1; }
  command -v patchelf >/dev/null || { echo "ERROR: --binary requires patchelf" >&2; exit 1; }
  command -v strip    >/dev/null || { echo "ERROR: --binary requires strip (binutils)" >&2; exit 1; }
  echo "Binary: substituting ${BINARY_OVERRIDE}"
  install -m 0755 "${BINARY_OVERRIDE}" "${PREFIX_DIR}/bin/plotjuggler4"
  patchelf --set-rpath '$ORIGIN/../lib' "${PREFIX_DIR}/bin/plotjuggler4"
  # linuxdeploy strips what it deploys, so the AppDir binary this replaces was
  # stripped; skipping it would more than double the package for debug info the
  # AppImage does not ship either. backward-cpp keeps resolving names through
  # .dynsym, exactly as it does in the AppImage.
  strip "${PREFIX_DIR}/bin/plotjuggler4"
  # stderr silenced: an older binutils readelf warns about DWARF it cannot parse
  # in .debug_info, which has nothing to do with the dynamic section read here.
  runpath="$(readelf -d "${PREFIX_DIR}/bin/plotjuggler4" 2>/dev/null | sed -n 's/.*R\(UN\)\?PATH.*\[\(.*\)\]/\2/p')"
  [[ "${runpath}" == '$ORIGIN/../lib' ]] \
    || { echo "ERROR: RUNPATH is '${runpath}', expected '\$ORIGIN/../lib'" >&2; exit 1; }
fi

# ---------------------------------------------------------------------------
# 4. Derive the external dependency set from the STAGED payload — after the
#    optional binary substitution, so what is scanned is exactly what ships.
#
#    Every ELF's DT_NEEDED minus the sonames the package itself provides =
#    what the target system must supply. An unmapped soname is a hard error: a
#    dependency bump that pulls in a new external library must fail the release
#    build, not ship a package that cannot start on a clean machine.
#
#    Both sides exempt plugins/ros2-topic-subscriber/dist/* — the same
#    carve-out as the release workflow's glibc audit: those per-distro inners
#    bind to the user's *sourced* ROS installation by design and are only
#    dlopen'ed on machines running that distro, so their DT_NEEDED (librclcpp
#    & co.) must not become Depends, and nothing they carry may satisfy a
#    dependency of the main payload (dist/ is not on any runtime search path).
#    The distro-agnostic proxy .so outside dist/ stays scanned.
# ---------------------------------------------------------------------------
echo "Scanning the staged payload for external shared-library dependencies..."
provided="$(mktemp)"; needed="$(mktemp)"
trap 'rm -f "${provided}" "${needed}"; rm -rf "${STAGE}"' EXIT
while IFS= read -r -d '' elf; do
  case "${elf}" in
    */plugins/ros2-topic-subscriber/dist/*) continue ;;
  esac
  # ELF check on the provided side too: a stray non-ELF file named like a
  # library must not silently satisfy a real system dependency.
  if file -b "${elf}" | grep -q ELF; then
    basename "${elf}"
  fi
done < <(find "${STAGE}" -type f -name '*.so*' -print0) | LC_ALL=C sort -u > "${provided}"
while IFS= read -r -d '' elf; do
  case "${elf}" in
    */plugins/ros2-topic-subscriber/dist/*) continue ;;
  esac
  if file -b "${elf}" | grep -q ELF; then
    readelf -d "${elf}" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'
  fi
done < <(find "${STAGE}" -type f \( -name '*.so*' -o -perm -u+x \) -print0) \
  | LC_ALL=C sort -u > "${needed}"

declare -A WANTED_PKGS=()
unmapped=()
while IFS= read -r soname; do
  pkg="${SONAME_PKG[${soname}]:-}"
  if [[ -z "${pkg}" ]]; then
    unmapped+=("${soname}")
  else
    WANTED_PKGS["${pkg}"]=1
  fi
done < <(LC_ALL=C comm -23 "${needed}" "${provided}")

if (( ${#unmapped[@]} > 0 )); then
  {
    echo "ERROR: the payload needs shared libraries with no declared package:"
    printf '  %s\n' "${unmapped[@]}"
    echo "Add each to SONAME_PKG in $(basename "${BASH_SOURCE[0]}") (and confirm the"
    echo "package name exists on the Ubuntu 22.04 floor), or bundle the library."
  } >&2
  exit 1
fi

# libc6 carries the glibc floor the builder container enforces; the rest are
# unversioned because every one has been present and ABI-stable since 22.04.
DEPENDS="libc6 (>= 2.35)"
for pkg in $(printf '%s\n' "${!WANTED_PKGS[@]}" | LC_ALL=C sort); do
  [[ "${pkg}" == "libc6" ]] && continue
  DEPENDS="${DEPENDS}, ${pkg}"
done
# Non-ELF runtime requirements the DT_NEEDED scan structurally cannot see:
# the wrapper points gRPC at the system CA bundle, and Qt TLS (marketplace
# downloads, update check, telemetry) needs it too.
DEPENDS="${DEPENDS}, ca-certificates"
echo "Depends: ${DEPENDS}"

# ---------------------------------------------------------------------------
# 5. Wrapper, maintainer scripts, docs
# ---------------------------------------------------------------------------
sed "s|@PJ_PREFIX@|${PREFIX}|g" "${SCRIPT_DIR}/plotjuggler4.wrapper.in" \
  > "${STAGE}/usr/bin/${PACKAGE}"
chmod 0755 "${STAGE}/usr/bin/${PACKAGE}"

install -m 0755 "${SCRIPT_DIR}/postinst" "${STAGE}/DEBIAN/postinst"
install -m 0755 "${SCRIPT_DIR}/postrm"   "${STAGE}/DEBIAN/postrm"
install -m 0644 "${SCRIPT_DIR}/copyright" "${STAGE}/usr/share/doc/${PACKAGE}/copyright"

# gzip -n drops the timestamp from the container, so the same inputs and the
# same SOURCE_DATE_EPOCH give identical bytes.
if [[ -n "${SOURCE_DATE_EPOCH:-}" ]]; then
  changelog_date="$(date -u -d "@${SOURCE_DATE_EPOCH}" '+%a, %d %b %Y %H:%M:%S +0000')"
else
  changelog_date="$(date -uR)"
fi
printf '%s (%s) stable; urgency=medium\n\n  * PlotJuggler %s.\n    Release notes: https://github.com/PlotJuggler/PJ4/releases\n\n -- PlotJuggler <noreply@plotjuggler.io>  %s\n' \
  "${PACKAGE}" "${DEB_VERSION}" "${VERSION}" "${changelog_date}" \
  | gzip -9n > "${STAGE}/usr/share/doc/${PACKAGE}/changelog.Debian.gz"

# ---------------------------------------------------------------------------
# 6. Control metadata
# ---------------------------------------------------------------------------
INSTALLED_SIZE="$(du -sk --exclude=DEBIAN "${STAGE}" | cut -f1)"
sed -e "s|@PACKAGE@|${PACKAGE}|g" \
    -e "s|@VERSION@|${DEB_VERSION}|g" \
    -e "s|@ARCH@|${DEB_ARCH}|g" \
    -e "s|@DEPENDS@|${DEPENDS}|g" \
    -e "s|@INSTALLED_SIZE@|${INSTALLED_SIZE}|g" \
    "${SCRIPT_DIR}/control.in" > "${STAGE}/DEBIAN/control"

# md5sums lets `dpkg -V` verify the install later.
( cd "${STAGE}" && find . -path ./DEBIAN -prune -o -type f -print0 \
    | LC_ALL=C sort -z \
    | xargs -0 md5sum \
    | sed 's| \./| |' > DEBIAN/md5sums )
chmod 0644 "${STAGE}/DEBIAN/md5sums"

# ---------------------------------------------------------------------------
# 7. Build
#     -Zxz over dpkg's newer zstd default: xz is readable by every dpkg still
#     in the field, and this package is downloaded and installed by hand.
# ---------------------------------------------------------------------------
mkdir -p "${OUTPUT_DIR}"
OUTPUT="${OUTPUT_DIR}/${PACKAGE}_${DEB_VERSION}_${DEB_ARCH}.deb"
dpkg-deb --root-owner-group -Zxz --build "${STAGE}" "${OUTPUT}" >/dev/null

echo ""
echo "Done: ${OUTPUT}  ($(du -h "${OUTPUT}" | cut -f1))"
