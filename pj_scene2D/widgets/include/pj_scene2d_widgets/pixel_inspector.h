#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QPoint>
#include <QSize>
#include <QString>
#include <QWidget>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "pj_scene2d_core/decoded_frame.h"

namespace PJ {

/// RGB888 whatever the frame's storage format.
struct InspectorRgb {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  [[nodiscard]] bool operator==(const InspectorRgb& other) const noexcept {
    return r == other.r && g == other.g && b == other.b;
  }
};

/// The clockwise view rotations a MediaViewState accepts, in selector order.
inline constexpr std::array<int, 4> kViewRotations{0, 90, -90, 180};

/// True for the rotations that swap the displayed width and height.
[[nodiscard]] constexpr bool isQuarterTurn(int rotation_deg) {
  return rotation_deg == 90 || rotation_deg == -90;
}

/// Per-axis clip-space scale at zoom 1 that fits the (rotated) image inside the
/// widget: the aspect-preserving sx/sy of MediaViewerWidget::buildViewTransform().
/// {1, 1} while the frame aspect is unknown (<= 0).
[[nodiscard]] QPointF fitScale(QSize widget_size, float frame_aspect, int rotation_deg);

/// Pan after dragging `delta_px` (widget pixels, y down) at `zoom`, so the grabbed
/// image point follows the cursor. `fit` comes from fitScale().
[[nodiscard]] QPointF panAfterDrag(QPointF pan, float zoom, QPointF delta_px, QSize widget_size, QPointF fit);

/// Pan after zooming from `old_zoom` to `new_zoom` so the image point under
/// `cursor_px` stays under it. `fit` comes from fitScale().
[[nodiscard]] QPointF panAfterZoom(
    QPointF pan, float old_zoom, float new_zoom, QPointF cursor_px, QSize widget_size, QPointF fit);

/// Maps a widget point to an image pixel using MediaViewerWidget::buildViewTransform()
/// zoom/pan/rotation semantics. Returns nullopt for invalid sizes/zoom or off-image
/// points; pan is normalized clip-space offset (+X moves image right, +Y moves it up)
/// and `rotation_deg` is the clockwise quarter-turn (0, 90, -90 or 180).
[[nodiscard]] std::optional<QPoint> widgetPointToImagePixel(
    QPointF widget_point, QSize widget_size, QSize image_size, float zoom, float pan_x, float pan_y, int rotation_deg);

/// Nullopt for invalid storage or an off-image point.
[[nodiscard]] std::optional<InspectorRgb> pixelRgbAt(const DecodedFrame& frame, int x, int y);

/// Extracts a row-major RGB888 crop centered on the image pixel. Pixels outside
/// the frame remain zero-filled; invalid frames or non-positive crop sizes return {}.
[[nodiscard]] std::vector<uint8_t> extractRgbCrop(const DecodedFrame& frame, int center_x, int center_y, int crop_size);

/// Metres. Nullopt for a non-depth frame, an off-image point or a no-data pixel (isValidDepth(), the shader's
/// `!(d > 0)` rule), so the readout agrees with the display.
[[nodiscard]] std::optional<float> depthMetersAt(const DecodedFrame& frame, int x, int y);

/// The depth shader's colour for `depth_m`, from the colorFor() the GPU LUT is built from (equal up to LUT
/// quantization). `depth_m` must be valid (depthMetersAt() first).
[[nodiscard]] InspectorRgb depthColormapColor(float depth_m, const DepthColorParams& params);

/// Tooltip magnifier for the hovered pixel: an RGB zoom grid (updatePixel) or, for depth, the value and a colormap
/// swatch (updateDepth). Owns only its crop copy.
class PixelInspector : public QWidget {
  Q_OBJECT

 public:
  explicit PixelInspector(QWidget* parent = nullptr);

  /// `crop_rgb` is crop_size*crop_size RGB888, row-major; a shorter buffer hides the tooltip.
  void updatePixel(std::vector<uint8_t> crop_rgb, int crop_size, int image_x, int image_y);

  /// `depth_m` nullopt shows "— (no data)".
  void updateDepth(int image_x, int image_y, std::optional<float> depth_m, const DepthColorParams& params);

  /// Shows near the cursor and flips left/up when the tooltip would cross the
  /// available screen edge.
  void showNear(const QPoint& global_pos);
  void hideImmediately();

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  enum class Mode { Rgb, Depth };

  [[nodiscard]] InspectorRgb cropPixel(int x, int y) const;
  void paintRgb(QPainter& painter);
  void paintDepth(QPainter& painter);
  /// Shared by updateDepth (sizing) and paintDepth (drawing), so they agree.
  [[nodiscard]] QString depthValueText() const;

  Mode mode_ = Mode::Rgb;

  std::vector<uint8_t> crop_data_;
  int crop_size_ = 0;

  std::optional<float> depth_m_;  ///< nullopt: no-data pixel
  DepthColorParams depth_params_;

  int image_x_ = 0;
  int image_y_ = 0;
};

}  // namespace PJ
