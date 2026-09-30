// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared gate for OpenGL-availability skips across the GL-backed test suites
// (pj_plotting, pj_scene3D, ...). Every site that self-skips because there is
// no display surface, makeCurrent() failed, or the driver's GL version is too
// old should use PJ_SKIP_WITHOUT_GPU(reason) instead of a bare GTEST_SKIP().
//
// With PJ_REQUIRE_GPU_TESTS=1 in the environment — set by CI lanes that are
// supposed to have a working GL stack (Linux xvfb + Mesa llvmpipe) — the same
// reason becomes a hard test FAILURE instead of a silent skip, so a broken GL
// stack cannot make such a lane go quietly green. Unset (local dev, and
// platforms with no GPU such as Windows CI), behaviour is unchanged: a plain
// skip. Available to any test target via pj_add_test_runner (see PjTests.cmake).
// The display-test helpers at the end (real QPA, headless skip, grab wait,
// backend name) serve the QRhi display runners.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#ifdef QT_WIDGETS_LIB
#include <QRhiWidget>
#endif

#include "pj_base/sdk/platform.hpp"

// True when the environment demands a working GPU/GL stack for these tests.
inline bool pjGpuRequiredByEnv() {
  return PJ::sdk::getEnv("PJ_REQUIRE_GPU_TESTS") == "1";
}

// NOTE: `reason` is deliberately NOT parenthesized here — most call sites pass a
// streamed chain (`"GL " << version << " below 4.5"`, not a single value), and
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

// (major, minor) parsed from a glGetString(GL_VERSION) string. Desktop GL_VERSION
// begins "MAJOR.MINOR…" (e.g. "4.5 (Core Profile) Mesa…"); null or unparsable
// (e.g. OpenGL ES) yields (0, 0), which callers treat as "too old".
inline std::pair<int, int> parseGlVersion(const char* version) {
  const QStringList parts = QString::fromLatin1(version).section(QLatin1Char(' '), 0, 0).split(QLatin1Char('.'));
  return {parts.value(0).toInt(), parts.value(1).toInt()};
}

// Gets `context` current on `surface` with a live GL>=4.5 context, the floor every
// scene #version 450 shader needs, then checks the DRIVER-reported GL_VERSION —
// not the requested QSurfaceFormat, which can survive into context.format() even
// when the driver granted an older context. Set the desired format on `context`
// and leave `surface` uncreated: the surface takes the context's negotiated
// format, as QOffscreenSurface requires for makeCurrent() to succeed everywhere.
// Returns the skip reason to hand to PJ_SKIP_WITHOUT_GPU, or nullopt once
// `context` is current and usable.
[[nodiscard]] inline std::optional<std::string> gl45ContextUnavailable(
    QOffscreenSurface& surface, QOpenGLContext& context) {
  if (!context.create()) {
    return "could not create an OpenGL context";
  }
  surface.setFormat(context.format());
  surface.create();
  if (!surface.isValid()) {
    return "no usable offscreen surface (headless without GL)";
  }
  if (!context.makeCurrent(&surface)) {
    return "could not make the OpenGL context current";
  }
  const auto* version = reinterpret_cast<const char*>(context.functions()->glGetString(GL_VERSION));
  if (parseGlVersion(version) < std::pair<int, int>(4, 5)) {
    return std::string("GL ") + (version != nullptr ? version : "?") + " below 4.5 — can't compile scene shaders";
  }
  return std::nullopt;
}

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

#ifdef QT_WIDGETS_LIB
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
#endif
