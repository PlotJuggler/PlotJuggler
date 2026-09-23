#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
#
# Add the PlotJuggler apt repository and install PlotJuggler 4:
#
#   curl -fsSL https://apt.plotjuggler.io/pj4_install.sh | sudo sh
#
# Safe to re-run: it rewrites its own two files (the keyring and the source
# entry) and then installs or upgrades plotjuggler4. Every check runs before
# anything on the system is changed. publish_apt.sh uploads this file next to
# the repository it configures.
#
# To undo:
#   sudo apt remove plotjuggler4
#   sudo rm /etc/apt/sources.list.d/plotjuggler.list /etc/apt/keyrings/plotjuggler.asc
set -eu

REPO_URL="https://apt.plotjuggler.io"
KEYRING="/etc/apt/keyrings/plotjuggler.asc"
SOURCES="/etc/apt/sources.list.d/plotjuggler.list"
# The release build's floor; the package declares it as libc6 (>= 2.35).
MIN_GLIBC="2.35"
APPIMAGE="https://github.com/PlotJuggler/PlotJuggler/releases/latest"

say() { printf '==> %s\n' "$*"; }
die() { printf 'plotjuggler install: %s\n' "$*" >&2; exit 1; }
# Waits for a concurrent apt run (unattended-upgrades on a fresh boot) instead
# of failing on its lock.
apt_get() { apt-get -o DPkg::Lock::Timeout=120 "$@"; }

preflight() {
  [ "$(id -u)" -eq 0 ] || die "must run as root: curl -fsSL ${REPO_URL}/pj4_install.sh | sudo sh"

  if ! command -v apt-get >/dev/null 2>&1 || ! command -v dpkg >/dev/null 2>&1; then
    die "this installer is for Debian and Ubuntu; on other distributions use the AppImage: ${APPIMAGE}"
  fi

  arch="$(dpkg --print-architecture)"
  [ "${arch}" = "amd64" ] \
    || die "only amd64 packages are published, and this system is ${arch}"

  # Checked here rather than left to apt, which would reject the package only
  # after the repository had been added.
  glibc="$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{ print $2 }')"
  if [ -z "${glibc}" ] || dpkg --compare-versions "${glibc}" lt "${MIN_GLIBC}"; then
    die "PlotJuggler 4 needs glibc ${MIN_GLIBC} or newer (Ubuntu 22.04+, Debian 12+); this system has ${glibc:-an unknown version}"
  fi

  # apt rejects every 'apt update' when one repository is listed twice with
  # different keys, so an entry added by hand under another name must go first.
  others="$(grep -rlsF "${REPO_URL#https://}" /etc/apt/sources.list /etc/apt/sources.list.d \
            | grep -vxF "${SOURCES}" | tr '\n' ' ' || true)"
  [ -z "${others}" ] \
    || die "the PlotJuggler repository is already configured in: ${others}- remove that entry, then run this again"
}

add_repository() {
  if ! command -v curl >/dev/null 2>&1; then
    say "Installing curl..."
    apt_get update -qq
    apt_get install -y -qq curl ca-certificates
  fi

  say "Adding the PlotJuggler repository (${REPO_URL})..."
  key="$(mktemp)"
  trap 'rm -f "${key}"' EXIT
  curl -fsSL "${REPO_URL}/plotjuggler-archive-keyring.asc" -o "${key}" \
    || die "could not download the signing key from ${REPO_URL}; check the connection (or proxy) to that host"
  # A captive portal or proxy can answer 200 with an HTML page; installing that
  # as the keyring would break 'apt update' with an obscure parse error.
  grep -q '^-----BEGIN PGP PUBLIC KEY BLOCK-----' "${key}" \
    || die "${REPO_URL} did not return a PGP key (is a proxy or captive portal intercepting HTTPS?)"
  install -d -m 0755 /etc/apt/keyrings
  install -m 0644 "${key}" "${KEYRING}"
  echo "deb [arch=amd64 signed-by=${KEYRING}] ${REPO_URL} stable main" > "${SOURCES}"
}

install_package() {
  say "Updating package lists..."
  apt_get update || die "'apt-get update' failed; the errors above name the repository at fault"
  # 'apt-get update' only warns about an unreachable repository, which would
  # otherwise surface later as the misleading "Unable to locate package".
  apt-cache show plotjuggler4 >/dev/null 2>&1 \
    || die "plotjuggler4 is not available from ${REPO_URL}; see the warnings from 'apt-get update' above"

  # shellcheck disable=SC2016  # ${Status}/${Version} are dpkg-query fields
  if dpkg-query -W -f='${Status}' plotjuggler4 2>/dev/null | grep -q 'ok installed'; then
    say "PlotJuggler 4 is already installed; upgrading it if a newer release exists..."
  else
    say "Installing PlotJuggler 4..."
  fi
  apt_get install -y plotjuggler4

  # shellcheck disable=SC2016
  version="$(dpkg-query -W -f='${Version}' plotjuggler4)"
  say "Done: plotjuggler4 ${version} is installed. Start it with 'plotjuggler4'."
  say "Future releases arrive with 'sudo apt update && sudo apt upgrade'."
}

# Everything runs from here, the last line: if the download is cut short, sh
# stops at a syntax error instead of running half a script.
main() {
  preflight
  add_repository
  install_package
}

main
