// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_widgets/pixel_inspector.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QPalette>
#include <QScreen>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "pj_scene2d_core/depth_range.h"  // isValidDepth — shared depth no-data rule
#include "pj_scene2d_core/video_color.h"  // buildYuvMatrix — single source of the YUV->RGB coefficients
#include "pj_widgets/Colormap.h"          // colorFor — same colormap source the GPU depth LUT is built from
#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

constexpr int kZoomCellSize = 15;
constexpr int kInfoPanelWidth = 160;
constexpr int kPadding = 8;
constexpr int kSwatchSize = 40;
constexpr int kCursorOffset = 20;
constexpr int kFontSize = 11;
constexpr int kMinInfoHeight = 140;
constexpr int kRgbBytesPerPixel = 3;
constexpr int kRgbaBytesPerPixel = 4;
constexpr int kLineHeight = 20;         ///< vertical step between info-panel text lines
constexpr int kDepthBytesPerPixel = 4;  ///< kDepthR32F: one float32 per pixel

[[nodiscard]] uint8_t clampToByte(float value) {
  return static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

// Read-out YUV->RGB for the inspector. Uses the SAME matrix the GPU display path
// applies (buildYuvMatrix, selected from the frame's signalled colorimetry), so the
// inspected value matches the pixel on screen for BT.601 / limited-range content too
// — not a hardcoded full-range BT.709 (which would disagree with the display).
[[nodiscard]] InspectorRgb yuvToRgb(uint8_t y, uint8_t u, uint8_t v, YuvColorSpace space, YuvColorRange range) {
  const std::array<float, 16> m = buildYuvMatrix(space, range);  // column-major; applied to (y, u-0.5, v-0.5, 1)
  const float yf = static_cast<float>(y) / 255.0f;
  const float uf = static_cast<float>(u) / 255.0f - 0.5f;
  const float vf = static_cast<float>(v) / 255.0f - 0.5f;
  return {
      clampToByte((m[0] * yf + m[4] * uf + m[8] * vf + m[12]) * 255.0f),
      clampToByte((m[1] * yf + m[5] * uf + m[9] * vf + m[13]) * 255.0f),
      clampToByte((m[2] * yf + m[6] * uf + m[10] * vf + m[14]) * 255.0f),
  };
}

[[nodiscard]] bool hasExpectedStorage(const DecodedFrame& frame) {
  return frame.width > 0 && frame.height > 0 && frame.pixels != nullptr &&
         frame.pixels->size() >= expectedBufferSize(frame.width, frame.height, frame.format);
}

PJ::theme::Theme widgetTheme(const QWidget* widget) {
  (void)widget;
  return PJ::theme::appTheme();
}

}  // namespace

QPointF fitScale(QSize widget_size, float frame_aspect, int rotation_deg) {
  if (frame_aspect <= 0.0f || widget_size.width() <= 0 || widget_size.height() <= 0) {
    return {1.0, 1.0};
  }
  const double shown_aspect = isQuarterTurn(rotation_deg) ? 1.0 / frame_aspect : frame_aspect;
  const double widget_aspect = static_cast<double>(widget_size.width()) / static_cast<double>(widget_size.height());
  if (widget_aspect > shown_aspect) {
    return {shown_aspect / widget_aspect, 1.0};
  }
  return {1.0, widget_aspect / shown_aspect};
}

QPointF panAfterDrag(QPointF pan, float zoom, QPointF delta_px, QSize widget_size, QPointF fit) {
  // Clip space is y-up, widget pixels are y-down.
  const double clip_dx = delta_px.x() / widget_size.width() * 2.0;
  const double clip_dy = -delta_px.y() / widget_size.height() * 2.0;
  return {pan.x() + clip_dx / (fit.x() * zoom), pan.y() + clip_dy / (fit.y() * zoom)};
}

QPointF panAfterZoom(QPointF pan, float old_zoom, float new_zoom, QPointF cursor_px, QSize widget_size, QPointF fit) {
  const double clip_x = 2.0 * cursor_px.x() / widget_size.width() - 1.0;
  const double clip_y = 1.0 - 2.0 * cursor_px.y() / widget_size.height();
  const double inverse_zoom_step = 1.0 / new_zoom - 1.0 / old_zoom;
  return {pan.x() + clip_x / fit.x() * inverse_zoom_step, pan.y() + clip_y / fit.y() * inverse_zoom_step};
}

std::optional<QPoint> widgetPointToImagePixel(
    QPointF widget_point, QSize widget_size, QSize image_size, float zoom, float pan_x, float pan_y, int rotation_deg) {
  if (widget_size.width() <= 0 || widget_size.height() <= 0 || image_size.width() <= 0 || image_size.height() <= 0 ||
      zoom <= 0.0f) {
    return std::nullopt;
  }

  const float image_aspect = static_cast<float>(image_size.width()) / static_cast<float>(image_size.height());
  const QPointF fit = fitScale(widget_size, image_aspect, rotation_deg);
  const float sx = zoom * static_cast<float>(fit.x());
  const float sy = zoom * static_cast<float>(fit.y());

  if (sx == 0.0f || sy == 0.0f) {
    return std::nullopt;
  }

  const float widget_u = static_cast<float>(widget_point.x()) / static_cast<float>(widget_size.width());
  const float widget_v = static_cast<float>(widget_point.y()) / static_cast<float>(widget_size.height());
  const float clip_x = widget_u * 2.0f - 1.0f;
  const float clip_y = (1.0f - widget_v) * 2.0f - 1.0f;

  // Undo the clockwise quarter-turn (inverse of buildViewTransform's rotate).
  const float rotated_x = clip_x / sx - pan_x;
  const float rotated_y = clip_y / sy - pan_y;
  float quad_x = rotated_x;
  float quad_y = rotated_y;
  switch (rotation_deg) {
    case 90:
      quad_x = -rotated_y;
      quad_y = rotated_x;
      break;
    case -90:
      quad_x = rotated_y;
      quad_y = -rotated_x;
      break;
    case 180:
      quad_x = -rotated_x;
      quad_y = -rotated_y;
      break;
    default:
      break;
  }
  const float u = (quad_x + 1.0f) * 0.5f;
  const float v = (1.0f - quad_y) * 0.5f;
  if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
    return std::nullopt;
  }

  const int x = std::clamp(static_cast<int>(u * static_cast<float>(image_size.width())), 0, image_size.width() - 1);
  const int y = std::clamp(static_cast<int>(v * static_cast<float>(image_size.height())), 0, image_size.height() - 1);
  return QPoint(x, y);
}

std::optional<InspectorRgb> pixelRgbAt(const DecodedFrame& frame, int x, int y) {
  if (!hasExpectedStorage(frame) || x < 0 || y < 0 || x >= frame.width || y >= frame.height) {
    return std::nullopt;
  }

  const uint8_t* data = frame.pixels->data();
  const size_t index = static_cast<size_t>(y) * static_cast<size_t>(frame.width) + static_cast<size_t>(x);
  switch (frame.format) {
    case PixelFormat::kRGB888: {
      const uint8_t* p = data + index * kRgbBytesPerPixel;
      return InspectorRgb{p[0], p[1], p[2]};
    }
    case PixelFormat::kBGR888: {
      const uint8_t* p = data + index * kRgbBytesPerPixel;
      return InspectorRgb{p[2], p[1], p[0]};
    }
    case PixelFormat::kRGBA8888: {
      const uint8_t* p = data + index * kRgbaBytesPerPixel;
      return InspectorRgb{p[0], p[1], p[2]};
    }
    case PixelFormat::kBGRA8888: {
      const uint8_t* p = data + index * kRgbaBytesPerPixel;
      return InspectorRgb{p[2], p[1], p[0]};
    }
    case PixelFormat::kYUV420P: {
      const int uv_w = (frame.width + 1) / 2;
      const int uv_h = (frame.height + 1) / 2;
      const size_t y_size = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
      const size_t uv_index = static_cast<size_t>(y / 2) * static_cast<size_t>(uv_w) + static_cast<size_t>(x / 2);
      if (uv_index >= static_cast<size_t>(uv_w) * static_cast<size_t>(uv_h)) {
        return std::nullopt;
      }
      return yuvToRgb(
          data[index], data[y_size + uv_index], data[y_size + static_cast<size_t>(uv_w) * uv_h + uv_index],
          frame.color_space, frame.color_range);
    }
    case PixelFormat::kNV12: {
      const int uv_w = (frame.width + 1) / 2;
      const int uv_h = (frame.height + 1) / 2;
      const int uv_row_bytes = 2 * uv_w;  // interleaved U,V pairs; = frame.width only for even widths
      const size_t y_size = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
      const size_t uv_index =
          (static_cast<size_t>(y / 2) * static_cast<size_t>(uv_row_bytes) + static_cast<size_t>(x / 2) * 2U);
      if (uv_index + 1 >= static_cast<size_t>(uv_row_bytes) * static_cast<size_t>(uv_h)) {
        return std::nullopt;
      }
      return yuvToRgb(
          data[index], data[y_size + uv_index], data[y_size + uv_index + 1], frame.color_space, frame.color_range);
    }
    case PixelFormat::kMono8: {
      const uint8_t value = data[index];
      return InspectorRgb{value, value, value};
    }
    case PixelFormat::kMono16: {
      const size_t byte_index = index * 2;
      uint16_t value = 0;
      std::memcpy(&value, data + byte_index, sizeof(value));
      const uint8_t gray = static_cast<uint8_t>(value >> 8);
      return InspectorRgb{gray, gray, gray};
    }
    case PixelFormat::kDepthR32F:
      // Raw metric depth; the displayed color is computed in the shader (LUT), so
      // there is no RGB to sample from the frame buffer.
      return std::nullopt;
  }
  return std::nullopt;
}

std::vector<uint8_t> extractRgbCrop(const DecodedFrame& frame, int center_x, int center_y, int crop_size) {
  if (crop_size <= 0 || !hasExpectedStorage(frame)) {
    return {};
  }

  std::vector<uint8_t> crop(static_cast<size_t>(crop_size) * static_cast<size_t>(crop_size) * kRgbBytesPerPixel, 0);
  const int half = crop_size / 2;
  for (int row = 0; row < crop_size; ++row) {
    for (int col = 0; col < crop_size; ++col) {
      const int x = center_x - half + col;
      const int y = center_y - half + row;
      const auto rgb = pixelRgbAt(frame, x, y);
      if (!rgb.has_value()) {
        continue;
      }
      const size_t offset =
          (static_cast<size_t>(row) * static_cast<size_t>(crop_size) + static_cast<size_t>(col)) * kRgbBytesPerPixel;
      crop[offset] = rgb->r;
      crop[offset + 1] = rgb->g;
      crop[offset + 2] = rgb->b;
    }
  }
  return crop;
}

std::optional<float> depthMetersAt(const DecodedFrame& frame, int x, int y) {
  if (frame.format != PixelFormat::kDepthR32F || !hasExpectedStorage(frame) || x < 0 || y < 0 || x >= frame.width ||
      y >= frame.height) {
    return std::nullopt;
  }
  const size_t byte_index =
      (static_cast<size_t>(y) * static_cast<size_t>(frame.width) + static_cast<size_t>(x)) * kDepthBytesPerPixel;
  float value = 0.0f;
  std::memcpy(&value, frame.pixels->data() + byte_index, sizeof(value));
  if (!isValidDepth(value)) {  // shared rule: matches DepthPipelineSource / shader `!(d > 0)`
    return std::nullopt;
  }
  return value;
}

InspectorRgb depthColormapColor(float depth_m, const DepthColorParams& params) {
  const float span = std::max(params.far_m - params.near_m, 1e-6f);  // GPU guards far-near the same way
  float t = std::clamp((depth_m - params.near_m) / span, 0.0f, 1.0f);
  if (params.invert) {
    t = 1.0f - t;
  }
  const ColormapRgb c = colorFor(static_cast<Colormap>(params.colormap), t);  // == GPU LUT source colour
  return InspectorRgb{clampToByte(c.r * 255.0f), clampToByte(c.g * 255.0f), clampToByte(c.b * 255.0f)};
}

PixelInspector::PixelInspector(QWidget* parent) : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint) {
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_ShowWithoutActivating);
  setAttribute(Qt::WA_TransparentForMouseEvents);
}

void PixelInspector::updatePixel(std::vector<uint8_t> crop_rgb, int crop_size, int image_x, int image_y) {
  if (crop_size <= 0 ||
      crop_rgb.size() < static_cast<size_t>(crop_size) * static_cast<size_t>(crop_size) * kRgbBytesPerPixel) {
    crop_data_.clear();
    crop_size_ = 0;
    hideImmediately();
    return;
  }

  mode_ = Mode::Rgb;
  crop_data_ = std::move(crop_rgb);
  crop_size_ = crop_size;
  image_x_ = image_x;
  image_y_ = image_y;

  const int zoom_area = crop_size_ * kZoomCellSize;
  setFixedSize(zoom_area + kInfoPanelWidth + kPadding * 3, std::max(zoom_area, kMinInfoHeight) + kPadding * 2);
  update();
}

void PixelInspector::updateDepth(
    int image_x, int image_y, std::optional<float> depth_m, const DepthColorParams& params) {
  mode_ = Mode::Depth;
  depth_m_ = depth_m;
  depth_params_ = params;
  image_x_ = image_x;
  image_y_ = image_y;

  // Two text lines (Position, Depth) + a colormap swatch — no zoom grid. Size to
  // fit the wider line: the "— (no data)" string is longer than a metric value, so
  // a fixed width would clip it.
  const QFontMetrics fm{QFont(u"monospace"_s, kFontSize)};
  const QString position_line = u"Position: %1, %2"_s.arg(image_x_).arg(image_y_);
  const int text_w = std::max(fm.horizontalAdvance(position_line), fm.horizontalAdvance(depthValueText()));
  const int content_w = std::max(text_w, kSwatchSize) + kPadding * 2;
  const int content_h = kPadding + kLineHeight + 8 + kLineHeight + 12 + kSwatchSize + kPadding;
  setFixedSize(content_w, content_h);
  update();
}

void PixelInspector::showNear(const QPoint& global_pos) {
  QScreen* screen = QApplication::screenAt(global_pos);
  if (screen == nullptr) {
    screen = QApplication::primaryScreen();
  }
  if (screen == nullptr) {
    return;
  }

  const QRect screen_rect = screen->availableGeometry();
  int x = global_pos.x() + kCursorOffset;
  int y = global_pos.y() + kCursorOffset;
  if (x + width() > screen_rect.right()) {
    x = global_pos.x() - width() - kCursorOffset;
  }
  if (y + height() > screen_rect.bottom()) {
    y = global_pos.y() - height() - kCursorOffset;
  }
  move(x, y);

  if (!isVisible()) {
    show();
  } else {
    raise();
  }
}

void PixelInspector::hideImmediately() {
  hide();
}

void PixelInspector::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.fillRect(rect(), PJ::theme::overlay(PJ::theme::Overlay::Hud, widgetTheme(this)));
  if (mode_ == Mode::Depth) {
    paintDepth(painter);
  } else {
    paintRgb(painter);
  }
}

void PixelInspector::paintRgb(QPainter& painter) {
  if (crop_data_.empty() || crop_size_ <= 0) {
    return;
  }

  const int zoom_area = crop_size_ * kZoomCellSize;
  const int grid_x = kPadding;
  const int grid_y = kPadding;
  for (int row = 0; row < crop_size_; ++row) {
    for (int col = 0; col < crop_size_; ++col) {
      const auto pixel = cropPixel(col, row);
      painter.fillRect(
          grid_x + col * kZoomCellSize, grid_y + row * kZoomCellSize, kZoomCellSize, kZoomCellSize,
          QColor(pixel.r, pixel.g, pixel.b));
    }
  }

  const auto fw_theme = PJ::theme::appTheme();
  painter.setPen(QPen(PJ::theme::surface(PJ::theme::Surface::Separation, fw_theme), 1));
  for (int i = 0; i <= crop_size_; ++i) {
    painter.drawLine(grid_x + i * kZoomCellSize, grid_y, grid_x + i * kZoomCellSize, grid_y + zoom_area);
    painter.drawLine(grid_x, grid_y + i * kZoomCellSize, grid_x + zoom_area, grid_y + i * kZoomCellSize);
  }

  const int half = crop_size_ / 2;
  painter.setPen(
      QPen(PJ::theme::outline(PJ::theme::OutlineRole::Interactive, PJ::theme::OutlineState::Focused, fw_theme), 2));
  painter.drawRect(grid_x + half * kZoomCellSize, grid_y + half * kZoomCellSize, kZoomCellSize, kZoomCellSize);

  const auto center = cropPixel(half, half);
  int info_x = grid_x + zoom_area + kPadding;
  int info_y = grid_y;
  QFont mono(u"monospace"_s, kFontSize);
  painter.setFont(mono);
  painter.setPen(PJ::theme::onOverlayHud(fw_theme));

  painter.drawText(info_x, info_y + kLineHeight, u"Position: %1, %2"_s.arg(image_x_).arg(image_y_));
  info_y += kLineHeight + 8;
  painter.drawText(
      info_x, info_y + kLineHeight,
      u"RGB: %1, %2, %3"_s.arg(static_cast<int>(center.r), 3)
          .arg(static_cast<int>(center.g), 3)
          .arg(static_cast<int>(center.b), 3));
  info_y += kLineHeight + 4;

  const QString hex = u"#%1%2%3"_s.arg(static_cast<int>(center.r), 2, 16, QChar('0'))
                          .arg(static_cast<int>(center.g), 2, 16, QChar('0'))
                          .arg(static_cast<int>(center.b), 2, 16, QChar('0'))
                          .toUpper();
  painter.drawText(info_x, info_y + kLineHeight, hex);
  info_y += kLineHeight + 12;

  painter.fillRect(info_x, info_y, kSwatchSize, kSwatchSize, QColor(center.r, center.g, center.b));
  painter.setPen(QPen(PJ::theme::surface(PJ::theme::Surface::Separation, fw_theme), 1));
  painter.drawRect(info_x, info_y, kSwatchSize, kSwatchSize);
}

void PixelInspector::paintDepth(QPainter& painter) {
  const auto fw_theme = PJ::theme::appTheme();
  int x = kPadding;
  int y = kPadding;
  QFont mono(u"monospace"_s, kFontSize);
  painter.setFont(mono);
  painter.setPen(PJ::theme::onOverlayHud(fw_theme));

  // Labels padded so the values line up under each other in the monospace font.
  painter.drawText(x, y + kLineHeight, u"Position: %1, %2"_s.arg(image_x_).arg(image_y_));
  y += kLineHeight + 8;
  painter.drawText(x, y + kLineHeight, depthValueText());
  y += kLineHeight + 12;

  // Swatch: the colour the GPU shows for this pixel, or muted grey for no-data
  // (the shader renders no-data transparent, so there is no real colour to show).
  const InspectorRgb swatch =
      depth_m_.has_value() ? depthColormapColor(*depth_m_, depth_params_) : InspectorRgb{70, 70, 70};
  painter.fillRect(x, y, kSwatchSize, kSwatchSize, QColor(swatch.r, swatch.g, swatch.b));
  painter.setPen(QPen(PJ::theme::surface(PJ::theme::Surface::Separation, fw_theme), 1));
  painter.drawRect(x, y, kSwatchSize, kSwatchSize);
}

QString PixelInspector::depthValueText() const {
  // "Depth:" padded to the width of "Position: " so values align in the monospace font.
  if (depth_m_.has_value()) {
    return u"Depth:    %1 m"_s.arg(*depth_m_, 0, 'f', 3);
  }
  return u"Depth:    — (no data)"_s;
}

InspectorRgb PixelInspector::cropPixel(int x, int y) const {
  if (x < 0 || y < 0 || x >= crop_size_ || y >= crop_size_ || crop_data_.empty()) {
    return {};
  }
  const size_t offset =
      (static_cast<size_t>(y) * static_cast<size_t>(crop_size_) + static_cast<size_t>(x)) * kRgbBytesPerPixel;
  if (offset + 2 >= crop_data_.size()) {
    return {};
  }
  return InspectorRgb{crop_data_[offset], crop_data_[offset + 1], crop_data_[offset + 2]};
}

}  // namespace PJ
