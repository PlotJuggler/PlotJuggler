#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# Runs a command while holding the machine-wide build slot, so compiles, links and
# benchmarks never overlap (one at a time keeps memory bounded and measurements
# quiet). Plain test runs don't need the slot.
#
#   scripts/perf/build_slot.sh <command...>
#
# The slot is an flock on $PJ_BUILD_LOCK (default: pj_build.lock in
# $XDG_RUNTIME_DIR, else /tmp); every tool sharing a machine must use the same
# path. `flock -o` closes the lock fd before running the command, so a
# compositor or server the command leaves behind does not keep holding the slot.
set -euo pipefail
exec flock -o "${PJ_BUILD_LOCK:-${XDG_RUNTIME_DIR:-/tmp}/pj_build.lock}" "$@"
