// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/GraphicsApi.h"

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <rhi/qshader.h>

#include <QFile>
#include <QHash>
#include <QImage>
#include <QLoggingCategory>
#include <QTimer>
#include <QVariant>
#include <QtGlobal>
#if QT_CONFIG(opengl)
#include <QOffscreenSurface>
#endif
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#include <QtGui/private/qvulkandefaultinstance_p.h>
#endif
#include <memory>
#include <utility>

#include "pj_widgets/RhiTestHooks.h"

namespace PJ {
namespace {

Q_LOGGING_CATEGORY(lcGraphics, "pj.graphics")

// Whether Qt can create a QRhi for `api` here, as the window's backing store
// would. Creates and drops a context or device (Metal only checks for one):
// tens of milliseconds.
bool canCreateRhi(QRhiWidget::Api api) {
  switch (api) {
    case QRhiWidget::Api::OpenGL: {
#if QT_CONFIG(opengl)
      const std::unique_ptr<QOffscreenSurface> surface(QRhiGles2InitParams::newFallbackSurface());
      QRhiGles2InitParams params;
      params.fallbackSurface = surface.get();
      return QRhi::probe(QRhi::OpenGLES2, &params);
#else
      return false;
#endif
    }
    case QRhiWidget::Api::Vulkan: {
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
      // Qt's default instance, which the window's backing store uses next: the
      // probe then adds only a device, not a second instance.
      QRhiVulkanInitParams params;
      params.inst = QVulkanDefaultInstance::instance();
      return params.inst != nullptr && QRhi::probe(QRhi::Vulkan, &params);
#else
      return false;
#endif
    }
    case QRhiWidget::Api::Metal: {
#if QT_CONFIG(metal)
      QRhiMetalInitParams params;
      return QRhi::probe(QRhi::Metal, &params);
#else
      return false;
#endif
    }
    case QRhiWidget::Api::Direct3D11: {
#ifdef Q_OS_WIN
      QRhiD3D11InitParams params;
      return QRhi::probe(QRhi::D3D11, &params);
#else
      return false;
#endif
    }
    case QRhiWidget::Api::Direct3D12: {
#ifdef Q_OS_WIN
      QRhiD3D12InitParams params;
      return QRhi::probe(QRhi::D3D12, &params);
#else
      return false;
#endif
    }
    case QRhiWidget::Api::Null:
      return true;
  }
  return false;
}

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
    const QRhiWidget::Api native = nativeGraphicsApi(currentGraphicsPlatform());
    QRhiWidget::Api chosen = overridden.value_or(native);
    // Only an override other than the default is probed: the default path stays
    // free of a throwaway device at startup.
    if (chosen != native && !canCreateRhi(chosen)) {
      qCWarning(lcGraphics) << "PJ_GRAPHICS_API" << requested << "cannot start here; using" << native;
      chosen = native;
    }
    // Qt's backing store gives up on Metal without a device (it only logs at debug
    // level), which would leave every GPU widget blank; OpenGL 4.1 still works there.
    if (chosen == QRhiWidget::Api::Metal && !canCreateRhi(chosen)) {
      qCWarning(lcGraphics) << "Metal is unavailable; falling back to OpenGL";
      chosen = QRhiWidget::Api::OpenGL;
    }
    qCInfo(lcGraphics) << "Graphics API:" << chosen << (overridden == chosen ? "(set by PJ_GRAPHICS_API)" : "");
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
  // Adoptions minus releases is the number of QRhis the widget holds resources
  // on; "stale" is a new QRhi met while another one's resources were still held.
  if (held != nullptr && held != rhi) {
    rhiTestCount(widget, "pjTestRhiStale");
  }
  rhiTestCount(widget, "pjTestRhiAdoptions");
  logGraphicsBackend(widget, rhi);
}

void noteRhiRelease(QRhiWidget& widget, const QRhi* held) {
  if (held != nullptr) {
    rhiTestCount(widget, "pjTestRhiReleases");
  }
}

bool createRhiPipeline(QObject& owner, QRhiGraphicsPipeline& pipeline, const char* name) {
  rhiTestCount(owner, "pjTestPipelineAttempts");
  return !rhiTestFault(owner, name) && createRhiPipeline(pipeline);
}

bool createRhiPipeline(QRhiGraphicsPipeline& pipeline) {
  return !rhiTestShaderFault(pipeline) && pipeline.create();
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

QShader loadRhiShader(const QString& path) {
  // Every widget and every new QRhi (a float or redock) reuses the packs: the
  // resources never change, so each is read and deserialized once per process.
  static QHash<QString, QShader> loaded;
  if (const auto cached = loaded.constFind(path); cached != loaded.cend()) {
    return *cached;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    qCWarning(lcGraphics) << "Failed to open shader" << path;
    return {};
  }
  QShader shader = QShader::fromSerialized(file.readAll());
  if (!shader.isValid()) {
    qCWarning(lcGraphics) << "Invalid shader pack" << path;
    return {};
  }
  rhiTestNoteShaderPath(shader, path);
  loaded.insert(path, shader);
  return shader;
}

RhiColorTarget::~RhiColorTarget() {
  release();
}

bool RhiColorTarget::ensure(
    QRhi& rhi, QRhiTexture* texture, QRhiRenderBuffer* msaa, QRhiTexture* resolve, bool refresh) {
  const void* attachment = texture != nullptr ? static_cast<const void*>(texture) : msaa;
  if (attachment == nullptr) {
    return false;
  }
  if (target_ != nullptr && attachment == attachment_ && !refresh) {
    return true;
  }
  if (target_ == nullptr || attachment != attachment_) {
    QRhiColorAttachment color;
    if (texture != nullptr) {
      color.setTexture(texture);
    } else {
      color.setRenderBuffer(msaa);
      color.setResolveTexture(resolve);
    }
    auto target =
        std::unique_ptr<QRhiTextureRenderTarget>(rhi.newTextureRenderTarget(QRhiTextureRenderTargetDescription(color)));
    auto render_pass = std::unique_ptr<QRhiRenderPassDescriptor>(target->newCompatibleRenderPassDescriptor());
    delete target_;
    target_ = target.release();
    if (render_pass_ == nullptr || !render_pass_->isCompatible(render_pass.get())) {
      delete render_pass_;
      render_pass_ = render_pass.release();
    }
    target_->setRenderPassDescriptor(render_pass_);
    attachment_ = attachment;
  }
  if (!target_->create()) {
    delete target_;
    target_ = nullptr;
    attachment_ = nullptr;
    return false;
  }
  return true;
}

void RhiColorTarget::release() {
  delete target_;
  target_ = nullptr;
  delete render_pass_;
  render_pass_ = nullptr;
  attachment_ = nullptr;
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

namespace {
constexpr auto kGranule = static_cast<quint32>(kUploadGranule);
}  // namespace

void uploadStaticBufferInPieces(
    QRhiResourceUpdateBatch* updates, QRhiBuffer* buffer, quint32 offset, const QByteArray& data) {
  const auto size = static_cast<quint32>(data.size());
  const quint32 head = size / kGranule * kGranule;
  if (head == 0 || head == size) {
    updates->uploadStaticBuffer(buffer, offset, data);
    return;
  }
  updates->uploadStaticBuffer(buffer, offset, QByteArray::fromRawData(data.constData(), head));
  updates->uploadStaticBuffer(buffer, offset + head, size - head, data.constData() + head);
}

void uploadStaticBufferInPieces(
    QRhiResourceUpdateBatch* updates, QRhiBuffer* buffer, quint32 offset, quint32 size, const void* data) {
  const quint32 head = size / kGranule * kGranule;
  if (head > 0 && head < size) {
    updates->uploadStaticBuffer(buffer, offset, head, data);
    updates->uploadStaticBuffer(buffer, offset + head, size - head, static_cast<const char*>(data) + head);
    return;
  }
  updates->uploadStaticBuffer(buffer, offset, size, data);
}

}  // namespace PJ
