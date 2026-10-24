#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Bake the PlotRhiCanvas shaders into multi-backend .qsb packs:
#
#   bake.sh <qsb> [output-dir]      (default output-dir: this directory)
#
# The packs are committed because desktop Qt kits ship no qsb. The WASM
# configure step re-runs this script into a scratch directory and fails on any
# byte difference, so bake with the qsb of the Qt pinned by PJ_QT_VERSION in
# versions.env: pack bytes depend on the qsb version.
#
# Targets: GLSL ES 300 (WebGL2), GLSL 120/150/330/440 (every desktop OpenGL
# context QRhi may get), HLSL 5.0 (D3D11/12), MSL 1.2 (Metal); SPIR-V (Vulkan)
# is always included.
set -euo pipefail

QSB="${1:?usage: bake.sh <qsb> [output-dir]}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${2:-${HERE}}"

cd "${HERE}"
for source in *.vert *.frag; do
  "${QSB}" --glsl "300 es,120,150,330,440" --hlsl 50 --msl 12 -o "${OUT}/${source}.qsb" "${source}"
done
