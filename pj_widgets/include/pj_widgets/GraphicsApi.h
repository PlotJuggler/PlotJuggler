#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QRhiWidget>
#include <QString>
#include <QStringView>
#include <QVector>
#include <QtGlobal>
#include <algorithm>
#include <functional>
#include <optional>

class QImage;
class QRhi;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiRenderTarget;
class QRhiRenderBuffer;
class QRhiTexture;
class QRhiBuffer;
class QRhiResourceUpdateBatch;
class QRhiTextureRenderTarget;
class QShader;

namespace PJ {

/// Parses a backend name as accepted by `PJ_GRAPHICS_API` (case-insensitive:
/// opengl, vulkan, metal, d3d11, d3d12). nullopt for anything else.
[[nodiscard]] std::optional<QRhiWidget::Api> parseGraphicsApi(QStringView name);

/// Platform families the graphics-API policy distinguishes. A value rather than
/// `#ifdef`s so every platform's policy is testable from any host.
enum class GraphicsPlatform { Linux, Windows, MacOS, Wasm };

/// The platform this translation unit is compiled for. Linux stands for every
/// other desktop Unix too.
[[nodiscard]] constexpr GraphicsPlatform currentGraphicsPlatform() {
#if defined(Q_OS_WASM)
  return GraphicsPlatform::Wasm;
#elif defined(Q_OS_WIN)
  return GraphicsPlatform::Windows;
#elif defined(Q_OS_DARWIN)
  return GraphicsPlatform::MacOS;
#else
  return GraphicsPlatform::Linux;
#endif
}

/// The API a platform uses when nothing overrides it: D3D11 on Windows (always
/// present, WARP without a driver), Metal on macOS (GL is capped at 4.1 there),
/// OpenGL on Linux and WebGL2. Every GPU widget is a `QRhiWidget`, so no
/// `QOpenGLWidget` pins a window to OpenGL.
[[nodiscard]] constexpr QRhiWidget::Api nativeGraphicsApi(GraphicsPlatform platform) {
  switch (platform) {
    case GraphicsPlatform::Windows:
      return QRhiWidget::Api::Direct3D11;
    case GraphicsPlatform::MacOS:
      return QRhiWidget::Api::Metal;
    case GraphicsPlatform::Linux:
    case GraphicsPlatform::Wasm:
      break;
  }
  return QRhiWidget::Api::OpenGL;
}

/// The QRhi backend every `QRhiWidget` in the app must request, before the widget
/// joins a shown hierarchy.
///
/// **One API per top-level window**: Qt composites a window's widgets with a single
/// QRhi, created for the first GPU widget it meets. A widget asking for another API
/// logs "The top-level window is already using another graphics API" and renders
/// nothing, and a `QOpenGLWidget` always asks for OpenGL. Every GPU widget therefore
/// requests this one value; floating docks are separate top-levels under the same rule.
///
/// It is `nativeGraphicsApi()` for this platform. `PJ_GRAPHICS_API` overrides it for
/// diagnostics; an override other than that default is probed by creating a
/// throwaway QRhi, and one that cannot start falls back to the default with a
/// warning. When Metal cannot start (a VM without a Metal device; Qt does not
/// retry it) this falls back to OpenGL. The default D3D11 is not probed: Qt already
/// retries it on the WARP software adapter. Resolved and logged (category
/// `pj.graphics`) on the first call, which must precede every GPU widget (pj_app
/// makes it right after constructing `QApplication`).
[[nodiscard]] QRhiWidget::Api preferredGraphicsApi();

/// Logs (category `pj.graphics`) the backend and adapter `widget` actually got,
/// once per QRhi: a software rasterizer or an unexpected fallback is otherwise
/// invisible. The widget passes `rhi()` (protected) from its initialize(); see
/// noteRhiAdoption().
void logGraphicsBackend(QWidget& widget, QRhi* rhi);

/// Bookkeeping for a QRhiWidget whose initialize() (re)builds its resources on
/// `rhi` while holding resources on `held` (null when it holds none). Call it
/// before releasing them. Logs the backend (logGraphicsBackend()) and counts
/// `pjTestRhiAdoptions`, and `pjTestRhiStale` when `held` is another QRhi Qt did
/// not release first (RhiTestHooks.h).
void noteRhiAdoption(QRhiWidget& widget, const QRhi* held, QRhi* rhi);

/// The releaseResources() counterpart: counts `pjTestRhiReleases` when `held`
/// is set (RhiTestHooks.h).
void noteRhiRelease(QRhiWidget& widget, const QRhi* held);

/// `pipeline.create()`, unless rhiTestFault(owner, name) or rhiTestShaderFault()
/// (RhiTestHooks.h). Each call counts in `owner`'s `pjTestPipelineAttempts`, so a
/// test can see that a failed widget does not retry on every frame.
[[nodiscard]] bool createRhiPipeline(QObject& owner, QRhiGraphicsPipeline& pipeline, const char* name);
/// The owner-less form: `pipeline.create()`, unless rhiTestShaderFault().
[[nodiscard]] bool createRhiPipeline(QRhiGraphicsPipeline& pipeline);

/// What a QRhiWidget built its pipelines for on its current QRhi. The
/// descriptor's address catches a new target whose format serializes the same
/// (OpenGL); the serialized format catches a new one allocated at the old address.
struct RhiTargetKey {
  const QRhiRenderPassDescriptor* render_pass = nullptr;
  QVector<quint32> format;
  int sample_count = 0;

  [[nodiscard]] static RhiTargetKey of(const QRhiRenderTarget& target, int sample_count);
  bool operator==(const RhiTargetKey&) const = default;
};

/// A GPU widget's failure state, so each onset is reported once.
class RhiFailureLatch {
 public:
  enum class Change { kNone, kEntered, kChanged, kLeft };

  /// Enters (non-empty `reason`) or leaves (empty) the failure state, or
  /// changes its reason. The caller reports kEntered (log, renderFailed()).
  Change set(QString reason);
  [[nodiscard]] const QString& reason() const noexcept {
    return reason_;
  }
  [[nodiscard]] bool failed() const noexcept {
    return !reason_.isEmpty();
  }

 private:
  QString reason_;
};

/// The .qsb pack at `path` (a resource), read once per process and shared by
/// every widget and QRhi; invalid (and logged) when it cannot be read. GUI thread only.
[[nodiscard]] QShader loadRhiShader(const QString& path);

/// A colour-only render target over a QRhiWidget's own colour buffer, for a
/// widget with autoRenderTarget off (no depth-stencil). Its render pass
/// descriptor survives rebuilds while compatible, so pipelines built on it
/// survive resizes.
class RhiColorTarget {
 public:
  RhiColorTarget() = default;
  ~RhiColorTarget();
  RhiColorTarget(const RhiColorTarget&) = delete;
  RhiColorTarget& operator=(const RhiColorTarget&) = delete;

  /// Makes the target current for a QRhiWidget's colour buffer on `rhi`: its
  /// colorTexture() `texture`, else its msaaColorBuffer() `msaa` resolving into
  /// its resolveTexture() `resolve` (the widget passes its own, which are
  /// protected). `refresh` re-creates it on a buffer the widget may have
  /// rebuilt in place (from initialize(): a resize or a format change);
  /// otherwise only a missing or replaced attachment rebuilds it. False when
  /// there is no colour buffer or the target cannot be created (the target is
  /// then dropped).
  bool ensure(QRhi& rhi, QRhiTexture* texture, QRhiRenderBuffer* msaa, QRhiTexture* resolve, bool refresh);
  [[nodiscard]] QRhiTextureRenderTarget* target() const noexcept {
    return target_;
  }
  [[nodiscard]] QRhiRenderPassDescriptor* renderPass() const noexcept {
    return render_pass_;
  }
  /// Deletes the target and its descriptor (before the QRhi goes away).
  void release();

 private:
  QRhiTextureRenderTarget* target_ = nullptr;
  QRhiRenderPassDescriptor* render_pass_ = nullptr;
  const void* attachment_ = nullptr;
};

/// Delivers the next frame `widget` presents, read back as an image (null after
/// 2 s without a presented frame, e.g. an occluded window). A hidden widget is
/// grabbed offscreen immediately. Asynchronous: `done` runs from the event loop,
/// never from inside render().
void grabPresentedFrame(QRhiWidget& widget, std::function<void(const QImage&)> done);

/// The static-upload size Mesa's anv (26.0) copies fast. It records each Vulkan
/// copy (QRhi's vkCmdCopyBuffer for a static upload) by searching its element
/// count for a 2D/3D surface shape on the CPU: a count with a large prime factor
/// costs milliseconds (36 ms per frame for a 50-curve plot, 22 ms for a
/// 1000003-point cloud; TECHNICAL_NOTES §9), a multiple of this or a copy of at
/// most this size does not.
inline constexpr qsizetype kUploadGranule = 64 * 1024;

/// The size an array of `used` bytes takes padded for a one-piece upload:
/// `used` up to kUploadGranule, else the next multiple of it.
[[nodiscard]] constexpr qsizetype paddedUploadSize(qsizetype used) {
  return used <= kUploadGranule ? used : (used + kUploadGranule - 1) / kUploadGranule * kUploadGranule;
}

/// Resizes `bytes`, whose first `used` bytes are the records, to
/// paddedUploadSize(used) and zeroes what follows the records (never drawn), so
/// one uploadStaticBuffer() of it is a fast copy. The padding also keeps a Vulkan
/// Immutable buffer's upload at one staging buffer: QRhi gives each copy into
/// one its own full-size staging buffer. Allocate paddedUploadSize() of the
/// largest `used` up front to keep the resize in place.
inline void padForUpload(QByteArray& bytes, qsizetype used) {
  const qsizetype padded = paddedUploadSize(used);
  bytes.resize(padded);
  std::fill(bytes.begin() + used, bytes.end(), '\0');
}

/// `updates->uploadStaticBuffer(buffer, offset, data)` into a Static `buffer`,
/// for data that is not padded (padForUpload()): issued as up to two copies, the
/// leading multiple of kUploadGranule, then the rest. The leading part reads
/// `data` in place, without holding it: `data` must stay alive and unchanged
/// until the frame is submitted. The remainder (under kUploadGranule) is copied.
/// Data of at most kUploadGranule or a whole multiple of it is one upload that
/// shares `data`.
void uploadStaticBufferInPieces(
    QRhiResourceUpdateBatch* updates, QRhiBuffer* buffer, quint32 offset, const QByteArray& data);
/// The same for `size` bytes at `data`, both pieces copied into the batch (as
/// `uploadStaticBuffer(buffer, offset, size, data)` copies them).
void uploadStaticBufferInPieces(
    QRhiResourceUpdateBatch* updates, QRhiBuffer* buffer, quint32 offset, quint32 size, const void* data);

}  // namespace PJ
