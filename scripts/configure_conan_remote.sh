#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# Register (and enable) PlotJuggler's Artifactory Conan remote, which hosts the
# plotjuggler_sdk recipe + binaries. Anonymous read is enough; idempotent.
set -euo pipefail

REMOTE_NAME="plotjuggler-conan"
REMOTE_URL="https://plotjuggler.jfrog.io/artifactory/api/conan/plotjuggler-conan"

if ! conan remote list | grep -q "^${REMOTE_NAME}:"; then
  conan remote add "${REMOTE_NAME}" "${REMOTE_URL}"
fi
conan remote enable "${REMOTE_NAME}" >/dev/null
