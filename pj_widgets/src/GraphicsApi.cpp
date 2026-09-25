// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/GraphicsApi.h"

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <QImage>
#include <QLoggingCategory>
#include <QTimer>
#include <QVariant>
#include <QtGlobal>
#include <memory>
#include <utility>

namespace PJ {
namespace {

Q_LOGGING_CATEGORY(lcGraphics, "pj.graphics")

#ifdef PJ_RHI_TEST_HOOKS
void incrementProperty(QObject& object, const char* name) {
  object.setProperty(name, object.property(name).toInt() + 1);
}
#endif

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
    const std::optional<QRhiWidget::Api> overridden = parseGraphicsApi(requested);
    if (!requested.isEmpty() && !overridden) {
      qCWarning(lcGraphics) << "Ignoring unknown PJ_GRAPHICS_API value" << requested;
    }
    QRhiWidget::Api chosen = overridden.value_or(nativeGraphicsApi(currentGraphicsPlatform(), kRhiWidgetsBuild));
#if QT_CONFIG(metal)
    // Qt's backing store gives up on Metal without a device (it only logs at debug
    // level), which would leave every GPU widget blank; OpenGL 4.1 still works there.
    if (chosen == QRhiWidget::Api::Metal) {
      QRhiMetalInitParams params;
      if (!QRhi::probe(QRhi::Metal, &params)) {
        qCWarning(lcGraphics) << "Metal is unavailable; falling back to OpenGL";
        chosen = QRhiWidget::Api::OpenGL;
      }
    }
#endif
    qCInfo(lcGraphics) << "Graphics API:" << chosen << (overridden ? "(set by PJ_GRAPHICS_API)" : "");
    return chosen;
  }();
  return api;
}

void logGraphicsBackend(QWidget& widget, QRhi* rhi) {
  // ponytail: keyed by address, so a new QRhi allocated where the widget's last
  // one lived is not logged again; key on something else if that ever matters.
  static constexpr char kLoggedRhi[] = "pjLoggedGraphicsRhi";
  const auto key = static_cast<qulonglong>(reinterpret_cast<quintptr>(rhi));
  if (rhi == nullptr || widget.property(kLoggedRhi).toULongLong() == key) {
    return;
  }
  widget.setProperty(kLoggedRhi, key);
  qCInfo(lcGraphics).nospace() << widget.objectName() << ": " << rhi->backendName() << ", " << rhi->driverInfo();
}

void noteRhiAdoption(QRhiWidget& widget, const QRhi* held, QRhi* rhi) {
#ifdef PJ_RHI_TEST_HOOKS
  // Adoptions minus releases is the number of QRhis the widget holds resources
  // on; "stale" is a new QRhi met while another one's resources were still held.
  if (held != nullptr && held != rhi) {
    incrementProperty(widget, "pjTestRhiStale");
  }
  incrementProperty(widget, "pjTestRhiAdoptions");
#else
  Q_UNUSED(held);
#endif
  logGraphicsBackend(widget, rhi);
}

void noteRhiRelease([[maybe_unused]] QRhiWidget& widget, [[maybe_unused]] const QRhi* held) {
#ifdef PJ_RHI_TEST_HOOKS
  if (held != nullptr) {
    incrementProperty(widget, "pjTestRhiReleases");
  }
#endif
}

bool rhiTestFault([[maybe_unused]] const QObject& owner, [[maybe_unused]] const char* name) {
#ifdef PJ_RHI_TEST_HOOKS
  return owner.property("pjTestFailPipeline").toString() == QLatin1StringView(name);
#else
  return false;
#endif
}

bool createRhiPipeline(QObject& owner, QRhiGraphicsPipeline& pipeline, const char* name) {
#ifdef PJ_RHI_TEST_HOOKS
  incrementProperty(owner, "pjTestPipelineAttempts");
#endif
  return !rhiTestFault(owner, name) && pipeline.create();
}

RhiTargetKey RhiTargetKey::of(const QRhiRenderTarget& target, int sample_count) {
  const QRhiRenderPassDescriptor* render_pass = target.renderPassDescriptor();
  return {render_pass, render_pass->serializedFormat(), sample_count};
}

RhiFailureLatch::Change RhiFailureLatch::set(QString reason) {
  if (reason == reason_) {
    return Change::kNone;
  }
  const bool was_failed = failed();
  reason_ = std::move(reason);
  if (!failed()) {
    return Change::kLeft;
  }
  return was_failed ? Change::kChanged : Change::kEntered;
}

void grabPresentedFrame(QRhiWidget& widget, std::function<void(const QImage&)> done) {
  if (!widget.isVisible()) {
    done(widget.grabFramebuffer());
    return;
  }
  // The presented frame and the timeout race; whichever comes first delivers.
  auto pending = std::make_shared<std::function<void(const QImage&)>>(std::move(done));
  auto presented = std::make_shared<QMetaObject::Connection>();
  const auto deliver = [pending, presented](const QImage& image) {
    QObject::disconnect(*presented);
    // A second frameSubmitted may already be queued when the first one delivers.
    if (*pending) {
      std::exchange(*pending, nullptr)(image);
    }
  };
  // Queued: grab after frameSubmitted returns, never from inside the frame.
  QRhiWidget* target = &widget;
  *presented = QObject::connect(
      target, &QRhiWidget::frameSubmitted, target, [target, deliver] { deliver(target->grabFramebuffer()); },
      Qt::QueuedConnection);
  QTimer::singleShot(2000, target, [target, pending, deliver] {
    if (*pending) {
      qCWarning(lcGraphics).nospace() << target->objectName()
                                      << ": the shown widget presented no frame to grab (occluded window?)";
      deliver(QImage());
    }
  });
  widget.update();
}

}  // namespace PJ
