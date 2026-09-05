// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared scaffolding for the real-GL SceneViewWidget tests (hud_overlay_gl_test,
// tf_connections_gl_test): the live-context availability check and the
// driver-reported GL version probe both tests use to skip on headless / too-old
// backends. Header-only; consumers already link pj_scene3d_widgets + GTest.

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QString>
#include <QStringList>
#include <QSurface>
#include <QSurfaceFormat>
#include <utility>

#include "pj_scene3d_widgets/scene_view_widget.h"

namespace pj::scene3d::test {

// True when the view holds a usable (valid) GL context.
inline bool haveGl(const SceneViewWidget& view) {
  return view.context() != nullptr && view.context()->isValid();
}

// (major, minor) parsed from a glGetString(GL_VERSION) string. Desktop GL_VERSION
// begins "MAJOR.MINOR…" (e.g. "4.5 (Core Profile) Mesa…"); null or unparsable
// (e.g. OpenGL ES) yields (0, 0), which callers treat as "too old".
inline std::pair<int, int> parseGlVersion(const char* version) {
  const QStringList parts = QString::fromLatin1(version).section(QLatin1Char(' '), 0, 0).split(QLatin1Char('.'));
  return {parts.value(0).toInt(), parts.value(1).toInt()};
}

// (major, minor) of the live GL context as the DRIVER reports it via
// glGetString(GL_VERSION) — NOT the requested QSurfaceFormat, which can survive
// into context()->format() even when the driver granted an older context. Returns
// (0, 0) when the context is unusable or the string is unparsable (e.g. OpenGL
// ES), which callers treat as "too old". Reading it needs the context current, so
// this makes it current on its own surface and releases it again (side-effect-free
// for the widget — its grabFramebuffer() calls re-make it current as usual).
inline std::pair<int, int> liveGlVersion(const SceneViewWidget& view) {
  QOpenGLContext* ctx = view.context();
  if (ctx == nullptr || !ctx->isValid()) {
    return {0, 0};
  }
  QSurface* surface = ctx->surface();
  if (surface == nullptr || !ctx->makeCurrent(surface)) {
    const QSurfaceFormat fmt = ctx->format();  // fallback: can't make current
    return {fmt.majorVersion(), fmt.minorVersion()};
  }
  const auto* version = reinterpret_cast<const char*>(ctx->functions()->glGetString(GL_VERSION));
  ctx->doneCurrent();
  return parseGlVersion(version);
}

}  // namespace pj::scene3d::test
