#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
#
# Add the PlotJuggler apt repository and install PlotJuggler 4:
#
#   curl -fsSL https://apt.plotjuggler.io/install.sh | sudo sh
#
# publish_apt.sh uploads this file next to the repository it configures, so
# the two cannot drift apart. It is safe to re-run: every step overwrites its
# own file rather than appending.
set -eu

REPO_URL="https://apt.plotjuggler.io"
KEYRING="/etc/apt/keyrings/plotjuggler.asc"
SOURCES="/etc/apt/sources.list.d/plotjuggler.list"

die() { echo "plotjuggler install: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run as root: curl -fsSL ${REPO_URL}/install.sh | sudo sh"
command -v apt-get >/dev/null 2>&1 || die "apt-get not found; on non-Debian systems use the AppImage from https://github.com/PlotJuggler/PlotJuggler/releases/latest"

ARCH="$(dpkg --print-architecture)"
[ "${ARCH}" = "amd64" ] || die "only amd64 is published today (this system is ${ARCH}); use the AppImage instead"

if ! command -v curl >/dev/null 2>&1; then
  apt-get update -qq
  apt-get install -y -qq curl ca-certificates
fi

# An armored .asc keyring needs no gpg on the host: apt >= 2.4 (Ubuntu 22.04,
# Debian 12, the supported floor) reads it directly through signed-by.
install -d -m 0755 /etc/apt/keyrings
curl -fsSL "${REPO_URL}/plotjuggler-archive-keyring.asc" -o "${KEYRING}"
chmod 0644 "${KEYRING}"

echo "deb [arch=amd64 signed-by=${KEYRING}] ${REPO_URL} stable main" > "${SOURCES}"

apt-get update
apt-get install -y plotjuggler4

echo
echo "PlotJuggler 4 is installed: run 'plotjuggler4'. Future releases arrive with 'sudo apt upgrade'."
