#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Bake every Scene3D QRhi shader into a multi-backend .qsb pack:
#
#   bake.sh <qsb> [output-dir]      (default output-dir: this directory)
#
# The packs are committed. The WASM configure step re-runs this script into a
# scratch directory and fails on any byte difference, so bake with the qsb of
# the Qt pinned by PJ_QT_VERSION in versions.env: pack bytes depend on the qsb
# version.
#
# Targets: GLSL ES 300 (WebGL2), GLSL 330 (OpenGL 3.3 / macOS GL 4.1) and 440,
# HLSL 5.0 (D3D11/12), MSL 1.2 (Metal); SPIR-V (Vulkan) is always included.
set -euo pipefail

QSB="${1:?usage: bake.sh <qsb> [output-dir]}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${2:-${HERE}}"
TARGETS=(--glsl "300 es,330,440" --hlsl 50 --msl 12)

cd "${HERE}"
for source in *.vert *.frag; do
  "${QSB}" "${TARGETS[@]}" -o "${OUT}/${source}.qsb" "${source}"
done
# Fullscreen triangle for backends whose texture row 0 is not at NDC y = -1.
"${QSB}" "${TARGETS[@]}" -DPJ_FLIP_Y -o "${OUT}/present_flip_y.vert.qsb" present.vert
# The filled-cell grid as the shadow receiver.
"${QSB}" "${TARGETS[@]}" -DPJ_GRID_SHADOW -o "${OUT}/grid_cell_shadow.frag.qsb" grid_cell.frag
# Point-list variant of the point shaders (gl_PointSize), for backends with
# QRhi::VertexShaderPointSize.
"${QSB}" "${TARGETS[@]}" -DPJ_POINT_TOPOLOGY -o "${OUT}/point_list.vert.qsb" point.vert
"${QSB}" "${TARGETS[@]}" -DPJ_POINT_TOPOLOGY -o "${OUT}/point_list.frag.qsb" point.frag
