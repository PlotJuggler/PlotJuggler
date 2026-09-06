#!/usr/bin/env bash
# Runs the full ctest suite headless. Many widget tests create a QApplication
# without forcing a platform plugin, so a plain `ctest` pops their windows on
# the desktop; `offscreen` keeps them invisible (CI gets the same via xvfb).
# All arguments are forwarded to ctest.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/versions.env"

export QT_QPA_PLATFORM=offscreen
export QT_IM_MODULE=""
export QT_PLUGIN_PATH="${SCRIPT_DIR}/.qt/${PJ_QT_VERSION}/gcc_64/plugins"

exec ctest --test-dir "${SCRIPT_DIR}/build" --output-on-failure "$@"
