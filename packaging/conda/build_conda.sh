#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Build the plotjuggler4 conda package into <output-dir> (default
# ./conda-output). PJ4's own dependency packages (recipes/) resolve from the
# plotjuggler channel, or from a local channel passed with --channel:
#
#   packaging/conda/build_conda.sh [--channel ./local-channel] [--output-dir DIR]
#
# PJ_VERSION (e.g. the release tag) overrides versions.env's PJ_APP_VERSION.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=../../versions.env
source "${ROOT}/versions.env"

CHANNEL="https://prefix.dev/plotjuggler"
OUTPUT="${ROOT}/conda-output"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --channel) CHANNEL="$(realpath "$2")"; shift 2 ;;
    --output-dir) OUTPUT="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

PJ_VERSION="${PJ_VERSION:-${PJ_APP_VERSION}}" PJ_QT_VERSION="${PJ_QT_VERSION}" \
  pixi exec --spec "rattler-build>=0.66,<0.67" \
  rattler-build build --recipe "${ROOT}/packaging/conda" -m "${ROOT}/recipes/variants.yaml" \
  -c conda-forge -c "${CHANNEL}" --output-dir "${OUTPUT}"
