// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared scaffolding for the QRhiWidget-in-ADS-dock tests (stand-in widgets in
// rhi_dock_lifecycle_test.cpp, the real plot/Scene3D/Scene2D widgets in
// rhi_mixed_dock_test.cpp). Header-only; each is its own runner, registered per
// backend.

#include <DockWidget.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QMutex>
#include <QRhiWidget>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "pj_gpu_test_gate.h"  // pjPreferRealDisplayQpa, PJ_SKIP_ON_HEADLESS_QPA, grab/colour helpers

#ifdef PJ_WITH_SCENE2D
#include "pj_scene2d_core/media_source.h"
#endif

namespace pj_rhi_dock_test {

// Runs before main(): the real QPA (pjPreferRealDisplayQpa()), and QRhi's leak
// report, so a widget that keeps GPU resources past its QRhi's lifetime logs
// "unreleased resources", which WarningCollector flags.
inline const bool kRealDisplayPlatform = [] {
  qputenv("QT_RHI_LEAK_CHECK", "1");
  // As pj_app's main(): see the comment there.
  QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  return pjPreferRealDisplayQpa();
}();

// Collects Qt warnings, so a test can assert none mentions a QRhi problem.
class WarningCollector {
 public:
  WarningCollector() {
    instance_ = this;
    previous_ = qInstallMessageHandler(&WarningCollector::handle);
  }
  ~WarningCollector() {
    qInstallMessageHandler(previous_);
    instance_ = nullptr;
  }
  WarningCollector(const WarningCollector&) = delete;
  WarningCollector& operator=(const WarningCollector&) = delete;

  /// Warnings that mean a QRhiWidget did not get, or lost, a working QRhi, or
  /// that a QRhi died with resources a widget never released.
  [[nodiscard]] QStringList rhiProblems() const {
    QMutexLocker lock(&mutex_);
    QStringList out;
    for (const QString& message : messages_) {
      if (message.contains(u"graphics API") || message.contains(u"No QRhi") || message.contains(u"QRhiWidget") ||
          message.contains(u"Failed to create") || message.contains(u"unreleased resources")) {
        out << message;
      }
    }
    return out;
  }

 private:
  static void handle(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if ((type == QtWarningMsg || type == QtCriticalMsg) && instance_ != nullptr) {
      QMutexLocker lock(&instance_->mutex_);
      instance_->messages_ << message;
    }
    if (instance_ != nullptr && instance_->previous_ != nullptr) {
      instance_->previous_(type, context, message);
    }
  }

  static inline WarningCollector* instance_ = nullptr;
  QtMessageHandler previous_ = nullptr;
  mutable QMutex mutex_;
  QStringList messages_;
};

[[nodiscard]] inline bool colourNear(QRgb actual, const QColor& expected) {
  return pjColourNear(actual, expected, 8);
}

[[nodiscard]] inline QImage presentAndGrab(QRhiWidget* widget) {
  return pjPresentAndGrab(widget);
}

/// Floats `dock` into its own top-level and waits until that window is exposed.
inline void floatDock(ads::CDockWidget* dock) {
  dock->setFloating();
  ASSERT_TRUE(dock->isFloating());
  ASSERT_TRUE(QTest::qWaitForWindowExposed(dock->window()));
}

#ifdef PJ_WITH_SCENE2D
// Delivers one frame and then nothing, like a paused source: whatever the viewer
// shows after a reparent comes from what it retained.
class OneShotMediaSource final : public PJ::MediaSource {
 public:
  explicit OneShotMediaSource(PJ::MediaFrame frame) : frame_(std::move(frame)) {}
  void setTimestamp(int64_t /*ts_ns*/) override {}
  std::optional<PJ::MediaFrame> takeFrame() override {
    return std::exchange(frame_, std::nullopt);
  }

 private:
  std::optional<PJ::MediaFrame> frame_;
};

// 64x64 RGBA: red top half, blue bottom half, and a filled green circle
// annotation in the middle.
[[nodiscard]] inline PJ::MediaFrame orientationFrame() {
  constexpr int kSize = 64;
  PJ::DecodedFrame image;
  image.width = kSize;
  image.height = kSize;
  image.format = PJ::PixelFormat::kRGBA8888;
  image.pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(kSize * kSize * 4));
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      uint8_t* px = image.pixels->data() + static_cast<size_t>((y * kSize + x) * 4);
      px[0] = y < kSize / 2 ? 255 : 0;
      px[1] = 0;
      px[2] = y < kSize / 2 ? 0 : 255;
      px[3] = 255;
    }
  }
  PJ::ImageAnnotation annotation;
  PJ::CircleAnnotation circle;
  circle.center = {kSize / 2.0, kSize / 2.0};
  circle.radius = 6.0;
  circle.color = {0, 255, 0, 255};
  circle.fill_color = {0, 255, 0, 255};
  annotation.circles.push_back(circle);
  PJ::MediaFrame frame;
  frame.base = image;
  frame.overlays.push_back(PJ::SceneFrame{.timestamp = 0, .annotations = {annotation}});
  return frame;
}

// The viewer shows orientationFrame(): image top on top (Vulkan's clip space is
// y-down) and the overlay present.
inline void expectOrientationFrame(QRhiWidget* viewer, const char* where) {
  const QImage image = presentAndGrab(viewer);
  ASSERT_FALSE(image.isNull()) << where << ": no frame presented";
  const int cx = image.width() / 2;
  const int cy = image.height() / 2;
  const int offset = std::min(image.width(), image.height()) * 3 / 10;
  EXPECT_TRUE(colourNear(image.pixel(cx, cy - offset), Qt::red)) << where << ": top is not red";
  EXPECT_TRUE(colourNear(image.pixel(cx, cy + offset), Qt::blue)) << where << ": bottom is not blue";
  EXPECT_TRUE(colourNear(image.pixel(cx, cy), Qt::green)) << where << ": overlay missing";
}
#endif

}  // namespace pj_rhi_dock_test
