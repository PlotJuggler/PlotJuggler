#!/usr/bin/env bash
# Registers THIS checkout's dev build with the user's desktop (application menu
# and, on Wayland, the dock icon) — user-scoped, nothing under /usr.
#
# Wayland compositors map a window to its launcher by app_id
# (QGuiApplication::setDesktopFileName in main.cpp) -> plotjuggler4.desktop ->
# its Icon= name in the icon theme. A packaged install ships those files; a dev
# build has none, so GNOME shows the generic gear. This installs a user-local
# entry whose Exec is this checkout's run.sh.
#
# xdg-utils are used on purpose: xdg-icon-resource bumps the theme directory's
# mtime, which is what makes GTK/GNOME rescan the theme. A bare copy into an
# existing hicolor dir stays invisible until that directory is touched.
#
#   scripts/install_desktop_entry.sh              # install / refresh
#   scripts/install_desktop_entry.sh --uninstall
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME=plotjuggler4
ICON_PNG="${ROOT}/resources/svg/${NAME}.png"
ICON_SVG="${ROOT}/resources/svg/plotjuggler.svg"

for tool in xdg-desktop-menu xdg-icon-resource; do
  command -v "${tool}" >/dev/null || { echo "ERROR: ${tool} not found (apt install xdg-utils)"; exit 1; }
done

if [[ "${1:-}" == "--uninstall" ]]; then
  xdg-desktop-menu uninstall --novendor --mode user "${NAME}.desktop" || true
  xdg-icon-resource uninstall --novendor --mode user --size 256 "${NAME}" || true
  rm -f "${XDG_DATA_HOME:-${HOME}/.local/share}/icons/hicolor/scalable/apps/${NAME}.svg"
  echo "Removed the ${NAME} desktop entry and icons."
  exit 0
fi

TMP="$(mktemp -d)"; trap 'rm -rf "${TMP}"' EXIT
# xdg-desktop-menu insists the file name carries a vendor prefix unless
# --novendor is given, and it copies the file, so a temp copy with the final
# name is all it needs. The Exec is absolute: the entry must work from anywhere.
sed -e "s|^Exec=.*|Exec=${ROOT}/run.sh|" "${ROOT}/appimage/${NAME}.desktop" > "${TMP}/${NAME}.desktop"

xdg-desktop-menu install --novendor --mode user "${TMP}/${NAME}.desktop"
xdg-icon-resource install --novendor --mode user --size 256 "${ICON_PNG}" "${NAME}"
# xdg-icon-resource takes only raster files; the scalable icon goes in by hand,
# after the tool above has already bumped the theme mtime.
SCALABLE="${XDG_DATA_HOME:-${HOME}/.local/share}/icons/hicolor/scalable/apps"
mkdir -p "${SCALABLE}" && cp "${ICON_SVG}" "${SCALABLE}/${NAME}.svg"

echo "Installed ${NAME}.desktop (Exec=${ROOT}/run.sh). Relaunch PlotJuggler to see its icon in the dock."
