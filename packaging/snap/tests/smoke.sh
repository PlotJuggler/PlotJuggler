#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# Run as a desktop user with the classic snap, host Mesa, Xvfb and Weston.
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd -P)
work=$(mktemp -d)
trap 'rm -rf -- "${work}"' EXIT
export XDG_CONFIG_HOME="${work}/config"
export XDG_DATA_HOME="${work}/data"
export XDG_CACHE_HOME="${work}/cache"
export PJ_DISABLE_TELEMETRY=1
export LIBGL_ALWAYS_SOFTWARE=1

extensions="${XDG_DATA_HOME}/PlotJuggler/PlotJuggler4/extensions"
if [[ -e "${extensions}" ]]; then
  echo "Expected no user extensions before the snap's first launch: ${extensions}" >&2
  exit 1
fi

QT_QPA_PLATFORM=offscreen timeout 30 snap run plotjuggler --version
timeout 30 snap run plotjuggler --selftest-python

set +e
QT_QPA_PLATFORM=xcb timeout 40 xvfb-run -a --server-args='-screen 0 1280x800x24' \
  snap run plotjuggler --nosplash --test-data
rc=${?}
set -e
if [[ "${rc}" -ne 124 ]]; then
  echo "PlotJuggler did not survive the startup timeout (exit ${rc})." >&2
  exit 1
fi

if [[ ! -d "${extensions}" || -z "$(ls -A "${extensions}")" ]]; then
  echo "Expected a nonempty extension directory after first launch: ${extensions}" >&2
  exit 1
fi

# The upstream render check captures output to a file; keep snap-run's streams
# on a pipe so its output is captured reliably across the snap launcher boundary.
# shellcheck disable=SC2016 # Arguments are expanded by the child Bash.
timeout 120 sh "${repo}/packaging/smoke/wayland_check.sh" \
  bash -o pipefail -c 'snap run plotjuggler "${@}" 2>&1 | cat' --
echo 'Snap startup, Python, extension seeding and Wayland rendering passed.'
