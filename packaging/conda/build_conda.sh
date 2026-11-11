#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Build the plotjuggler4 conda package into <output-dir> (default
# ./conda-output). PJ4's own dependency packages (recipes/) resolve from the
# plotjuggler channel, or from a local channel passed with --channel:
#
#   packaging/conda/build_conda.sh [--channel ./local-channel] [--output-dir DIR]
#                                  [--variant KEY=VALUE]...
#
# --variant narrows the variants.yaml matrix (e.g. --variant python=3.14
# --variant ffmpeg=9.0 builds one package, not four). PJ_VERSION (e.g. the
# release tag) overrides versions.env's PJ_APP_VERSION. With CCACHE_DIR set,
# compilation goes through ccache, shared by the variants and by later builds.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=../../versions.env
source "${ROOT}/versions.env"

CHANNEL="https://prefix.dev/plotjuggler"
OUTPUT="${ROOT}/conda-output"
VARIANT_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --channel) CHANNEL="$(realpath "$2")"; shift 2 ;;
    --output-dir) OUTPUT="$2"; shift 2 ;;
    --variant) VARIANT_ARGS+=(--variant "$2"); shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

PJ_VERSION="${PJ_VERSION:-${PJ_APP_VERSION}}" \
  pixi exec --spec "rattler-build>=0.66,<0.67" \
  rattler-build build --recipe "${ROOT}/packaging/conda" -m "${ROOT}/recipes/variants.yaml" \
  -c conda-forge -c "${CHANNEL}" --output-dir "${OUTPUT}" \
  ${VARIANT_ARGS[@]+"${VARIANT_ARGS[@]}"}
