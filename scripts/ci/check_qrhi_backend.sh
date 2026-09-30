#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Fails unless every QRhi view in a ctest log reported the expected backend:
#
#   check_qrhi_backend.sh <ctest-log> <expected-regex>
#
# The views log "<name>: <backend>, QRhiDriverInfo(...)" (category pj.graphics,
# so run ctest with QT_LOGGING_RULES=pj.graphics.info=true and -V). A test
# pass on another backend, or on an unexpected adapter, would prove nothing.
# <expected-regex> (extended) must match each distinct "<backend>, QRhiDriverInfo(...)"
# line from its start, e.g. 'Metal, ' or 'D3D11, .*deviceType=Cpu' (WARP).
set -euo pipefail

log="${1:?usage: check_qrhi_backend.sh <ctest-log> <expected-regex>}"
expected="${2:?usage: check_qrhi_backend.sh <ctest-log> <expected-regex>}"

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
