#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Includes <rhi/qrhi.h>, so its includer links Qt6::GuiPrivate, as every QRhi
// renderer does.
#include <rhi/qrhi.h>

namespace PJ {

/// Straight-alpha source-over: colour SrcAlpha / OneMinusSrcAlpha, alpha One /
/// OneMinusSrcAlpha. The blend of the plot canvas, the media viewer and every
/// data-graded Scene3D pipeline.
[[nodiscard]] inline QRhiGraphicsPipeline::TargetBlend straightAlphaBlend() {
  QRhiGraphicsPipeline::TargetBlend blend;
  blend.enable = true;
  blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
  blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  blend.srcAlpha = QRhiGraphicsPipeline::One;
  blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  return blend;
}

}  // namespace PJ
