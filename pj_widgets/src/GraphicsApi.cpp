// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/GraphicsApi.h"

#include <rhi/qrhi.h>

#include <QLoggingCategory>
#include <QtGlobal>
#include <utility>

namespace PJ {
namespace {

Q_LOGGING_CATEGORY(lcGraphics, "pj.graphics")

}  // namespace

std::optional<QRhiWidget::Api> parseGraphicsApi(QStringView name) {
  using Api = QRhiWidget::Api;
  static constexpr std::pair<QLatin1StringView, Api> kApis[] = {
      {QLatin1StringView("opengl"), Api::OpenGL},    {QLatin1StringView("vulkan"), Api::Vulkan},
      {QLatin1StringView("metal"), Api::Metal},      {QLatin1StringView("d3d11"), Api::Direct3D11},
      {QLatin1StringView("d3d12"), Api::Direct3D12},
  };
  for (const auto& [candidate, api] : kApis) {
    if (name.compare(candidate, Qt::CaseInsensitive) == 0) {
      return api;
    }
  }
  return std::nullopt;
}

QRhiWidget::Api preferredGraphicsApi() {
  static const QRhiWidget::Api api = [] {
    const QString requested = qEnvironmentVariable("PJ_GRAPHICS_API");
    if (!requested.isEmpty()) {
      if (const auto parsed = parseGraphicsApi(requested)) {
        qCWarning(lcGraphics) << "PJ_GRAPHICS_API overrides the graphics backend:" << requested;
        return *parsed;
      }
      qCWarning(lcGraphics) << "Ignoring unknown PJ_GRAPHICS_API value" << requested;
    }
    return QRhiWidget::Api::OpenGL;
  }();
  return api;
}

void logGraphicsBackend(const QWidget& widget, QRhi* rhi) {
  if (rhi != nullptr) {
    qCInfo(lcGraphics).nospace() << widget.objectName() << ": " << rhi->backendName() << ", " << rhi->driverInfo();
  }
}

}  // namespace PJ
