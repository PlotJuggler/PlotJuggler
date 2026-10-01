#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Bake the media viewer shaders into multi-backend .qsb packs:
#
#   bake.sh <qsb> [output-dir]      (default output-dir: this directory)
#
# The packs are committed because desktop Qt kits ship no qsb. The WASM
# configure step re-runs this script into a scratch directory and fails on any
# byte difference, so bake with the qsb of the Qt pinned by PJ_QT_VERSION in
# versions.env: pack bytes depend on the qsb version.
#
# Every pack: GLSL ES 300 (WebGL2), HLSL 5.0 (D3D11/12), MSL 1.2 (Metal) and
# SPIR-V (Vulkan, always included). Desktop GLSL: the image pass bakes 330
# (OpenGL 3.3, macOS GL 4.1) and 440; the overlay packs keep 100 es, 120 and
# 150, and a 3.3 or 4.1 core context picks 150.
set -euo pipefail

QSB="${1:?usage: bake.sh <qsb> [output-dir]}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$(cd "${2:-${HERE}}" && pwd)"

cd "${HERE}"
for source in yuv_to_rgb.vert yuv_to_rgb.frag; do
  "${QSB}" --glsl "300 es,330,440" --hlsl 50 --msl 12 -o "${OUT}/${source}.qsb" "${source}"
done
for source in scene_lines.vert scene_lines.frag scene_text.vert scene_text.frag; do
  "${QSB}" --glsl "100 es,120,150,300 es" --hlsl 50 --msl 12 -o "${OUT}/${source}.qsb" "${source}"
done
