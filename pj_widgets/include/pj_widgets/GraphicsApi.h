#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QRhiWidget>
#include <QStringView>
#include <optional>

class QRhi;

namespace PJ {

/// Parses a backend name as accepted by `PJ_GRAPHICS_API` (case-insensitive:
/// opengl, vulkan, metal, d3d11, d3d12). nullopt for anything else.
[[nodiscard]] std::optional<QRhiWidget::Api> parseGraphicsApi(QStringView name);

/// The QRhi backend every `QRhiWidget` in the app must request, before the widget
/// joins a shown hierarchy. Qt runs one backend per top-level window, and the
/// `QOpenGLWidget` plot canvas and 3D view still pin that window to OpenGL, so this
/// is OpenGL on every platform for now.
///
/// `PJ_GRAPHICS_API` overrides it for diagnostics only: a non-OpenGL value leaves
/// the `QOpenGLWidget`s in the same window without a working backend.
[[nodiscard]] QRhiWidget::Api preferredGraphicsApi();

/// Logs (category `pj.graphics`) the backend and adapter `widget` actually got.
/// Call from `QRhiWidget::initialize()` when `rhi()` changes (it is protected, so
/// only the widget can pass it); a software rasterizer or an unexpected fallback is
/// otherwise invisible.
void logGraphicsBackend(const QWidget& widget, QRhi* rhi);

}  // namespace PJ
