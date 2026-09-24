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

#include <gtest/gtest.h>

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QString>
#include <QStringList>
#include <optional>
#include <string>
#include <utility>

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
