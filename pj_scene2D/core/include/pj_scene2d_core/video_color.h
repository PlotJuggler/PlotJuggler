#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <array>

#include "pj_scene2d_core/decoded_frame.h"

namespace PJ {

/// The YUV→RGB matrix for a colour space and range, **column-major** (the std140 `mat4` the media shader reads),
/// applied as `rgb = (M * vec4(y, u - 0.5, v - 0.5, 1)).rgb`: the constant 4th column carries every range offset, so
/// the shader is the same for every combination. Full range has an all-zero 4th column; limited range folds in the
/// 255/219 luma and 255/224 chroma scales and the 16/255 luma offset.
[[nodiscard]] std::array<float, 16> buildYuvMatrix(YuvColorSpace space, YuvColorRange range) noexcept;

}  // namespace PJ
