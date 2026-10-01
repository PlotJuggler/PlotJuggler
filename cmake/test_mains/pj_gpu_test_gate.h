// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared gate for GPU-availability skips across the GPU test suites
// (pj_plotting, pj_scene3D, ...). Every site that self-skips because there is
// no display surface or no usable graphics device should use
// PJ_SKIP_WITHOUT_GPU(reason) instead of a bare GTEST_SKIP().
//
// With PJ_REQUIRE_GPU_TESTS=1 in the environment — set by CI lanes that are
// supposed to have a working GPU stack (Linux xvfb + Mesa llvmpipe) — the same
// reason becomes a hard test FAILURE instead of a silent skip, so a broken GL
// stack cannot make such a lane go quietly green. Unset (local dev, and
// platforms with no GPU such as Windows CI), behaviour is unchanged: a plain
// skip. Available to every MAIN gui runner of pj_add_test_runner (PjTests.cmake).
// The display-test helpers at the end (real QPA, headless skip, grab waits,
// colour comparison, backend name) serve the QRhi display runners.

#include <gtest/gtest.h>

#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QPointer>
#include <QRhiWidget>
#include <QString>
#include <QTimer>
#include <cstdlib>
#include <functional>
#include <string>

#include "pj_base/sdk/platform.hpp"

// True when the environment demands a working GPU/GL stack for these tests.
inline bool pjGpuRequiredByEnv() {
  return PJ::sdk::getEnv("PJ_REQUIRE_GPU_TESTS") == "1";
}

// NOTE: `reason` is deliberately NOT parenthesized here — most call sites pass a
// streamed chain (`"no QRhi for " << backend`, not a single value), and
// wrapping that in parens would evaluate it standalone (const char* << int) before
// it ever reaches the ostream GTEST_FAIL()/GTEST_SKIP() return.
#define PJ_SKIP_WITHOUT_GPU(reason) \
  do {                              \
    if (pjGpuRequiredByEnv()) {     \
      GTEST_FAIL() << reason;       \
    } else {                        \
      GTEST_SKIP() << reason;       \
    }                               \
  } while (0)

// ---- Display tests (GPU widgets on a real window system) ----------------------

// The gui test main defaults to `offscreen`, which composites no GPU widget.
// Call from a static initializer (before main()) in a display test: with a
// display, it asks for the platform's real QPA instead (wayland, then xcb, on
// Linux). An explicit QT_QPA_PLATFORM (test.sh's offscreen) still wins, and
// PJ_SKIP_ON_HEADLESS_QPA() then skips. Always true.
inline bool pjPreferRealDisplayQpa() {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
#if defined(Q_OS_WIN)
    qputenv("QT_QPA_PLATFORM", "windows");
#elif defined(Q_OS_MACOS)
    qputenv("QT_QPA_PLATFORM", "cocoa");
#else
    if (qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
      qputenv("QT_QPA_PLATFORM", "wayland;xcb");
    } else if (qEnvironmentVariableIsSet("DISPLAY")) {
      qputenv("QT_QPA_PLATFORM", "xcb");
    }
#endif
  }
  return true;
}

// Skips (fails under PJ_REQUIRE_GPU_TESTS=1) on a QPA that cannot composite a GPU widget.
#define PJ_SKIP_ON_HEADLESS_QPA()                                                                           \
  do {                                                                                                      \
    const QString pj_qpa = QGuiApplication::platformName();                                                 \
    if (pj_qpa == QLatin1String("offscreen") || pj_qpa == QLatin1String("minimal")) {                       \
      PJ_SKIP_WITHOUT_GPU("QPA '" << pj_qpa.toStdString() << "' cannot composite GPU widgets (xvfb-run?)"); \
    }                                                                                                       \
  } while (0)

// Drives an asynchronous grab (a widget's grabPresentedFrame(done)) to completion,
// as RGB32. Null when nothing arrived within 5 s, or `watched` died first.
inline QImage pjWaitForGrab(
    const std::function<void(std::function<void(const QImage&)>)>& grab, const QObject* watched = nullptr) {
  for (int i = 0; i < 3; ++i) {
    QCoreApplication::processEvents();
  }
  QImage image;
  bool delivered = false;
  QEventLoop loop;
  QTimer::singleShot(5000, &loop, &QEventLoop::quit);
  if (watched != nullptr) {
    QObject::connect(watched, &QObject::destroyed, &loop, &QEventLoop::quit);
  }
  grab([&](const QImage& frame) {
    image = frame;
    delivered = true;
    loop.quit();
  });
  if (!delivered) {
    loop.exec();
  }
  return image.convertToFormat(QImage::Format_RGB32);
}

// Presents one real frame of `widget`, then grabs it: the OpenGL QRhi's grab-only
// frames reuse stale state (pj_scene3D/docs/TECHNICAL_NOTES.md, Qt bugs). Null when
// nothing was presented within 5 s.
[[nodiscard]] inline QImage pjPresentAndGrab(QRhiWidget* widget) {
  bool presented = false;
  QEventLoop loop;
  QTimer::singleShot(5000, &loop, &QEventLoop::quit);
  const auto connection = QObject::connect(widget, &QRhiWidget::frameSubmitted, &loop, [&presented, &loop] {
    presented = true;
    loop.quit();
  });
  widget->update();
  loop.exec();
  QObject::disconnect(connection);
  return presented ? widget->grabFramebuffer() : QImage();
}

// True when every channel of `actual` is within `tolerance` of `expected`.
[[nodiscard]] inline bool pjColourNear(QRgb actual, const QColor& expected, int tolerance) {
  return std::abs(qRed(actual) - expected.red()) <= tolerance &&
         std::abs(qGreen(actual) - expected.green()) <= tolerance &&
         std::abs(qBlue(actual) - expected.blue()) <= tolerance;
}

// QRhi::backendName() of the backend `api` requests (pass PJ::preferredGraphicsApi():
// PJ_GRAPHICS_API, else the platform default).
[[nodiscard]] inline QString pjRhiBackendName(QRhiWidget::Api api) {
  switch (api) {
    case QRhiWidget::Api::Vulkan:
      return QStringLiteral("Vulkan");
    case QRhiWidget::Api::Direct3D11:
      return QStringLiteral("D3D11");
    case QRhiWidget::Api::Direct3D12:
      return QStringLiteral("D3D12");
    case QRhiWidget::Api::Metal:
      return QStringLiteral("Metal");
    case QRhiWidget::Api::Null:
      return QStringLiteral("Null");
    default:
      return QStringLiteral("OpenGL");
  }
}
