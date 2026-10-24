#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Fails unless every QRhi view in a ctest log reported the expected backend:
#
#   check_qrhi_backend.sh <ctest-log> <expected-regex> [<plot-backend>]
#
# The views log "<name>: <backend>, QRhiDriverInfo(...)" (category pj.graphics,
# so run ctest with QT_LOGGING_RULES=pj.graphics.info=true and -V). A test
# pass on another backend, or on an unexpected adapter, would prove nothing.
# <expected-regex> (extended) must match each distinct "<backend>, QRhiDriverInfo(...)"
# line from its start, e.g. 'Metal, ' or 'D3D11, .*deviceType=Cpu' (WARP).
# With <plot-backend> (QRhi::backendName(), e.g. D3D11 or Metal), at least one
# plot canvas must also have run, and every "PJ_WASM_PLOT_RHI_READY backend=<name>"
# line must name it.
set -euo pipefail

usage="usage: check_qrhi_backend.sh <ctest-log> <expected-regex> [<plot-backend>]"
log="${1:?$usage}"
expected="${2:?$usage}"
plot_backend="${3:-}"

adapters="$(grep -o '[A-Za-z0-9]*, QRhiDriverInfo(.*' "$log" | sort -u || true)"
echo "$adapters"
if [[ -z "$adapters" ]]; then
  echo "::error::no view logged its QRhi backend"
  exit 1
fi
if grep -Ev "^(${expected})" <<< "$adapters"; then
  echo "::error::a view ran on something other than '${expected}' (lines above)"
  exit 1
fi

if [[ -n "$plot_backend" ]]; then
  plots="$(grep -o 'PJ_WASM_PLOT_RHI_READY backend=[^ ]*' "$log" | tr -d '\r' | sort | uniq -c || true)"
  echo "$plots"
  if [[ -z "$plots" ]]; then
    echo "::error::no plot canvas logged its QRhi backend"
    exit 1
  fi
  if grep -v " backend=${plot_backend}\$" <<< "$plots"; then
    echo "::error::a plot canvas ran on something other than '${plot_backend}' (lines above)"
    exit 1
  fi
fi
