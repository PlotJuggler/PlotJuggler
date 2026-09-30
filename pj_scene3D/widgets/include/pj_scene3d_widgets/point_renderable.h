#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QColor>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/pointcloud.h"  // PointVertex
#include "pj_scene3d_widgets/layer_style.h"
#include "pj_widgets/Colormap.h"
#include "pj_widgets/GraphicsApi.h"  // padForUpload

namespace pj::scene3d {

/// Rewrites `retained` with the records `pack(out, reused)` writes into `out`
/// (the first `max_bytes` bytes of the array) and returns the byte count of,
/// then pads it for upload (PJ::padForUpload()). The records go into
/// `retained`'s own allocation when nothing else shares it (the view's upload
/// batch lets go once its frame is submitted, if it carried over 1 MiB: QRhi
/// keeps a smaller batch's data) and it is at most twice the size needed: a
/// fresh array costs a page fault per 4 KiB on first write. `reused`
/// tells `pack` which it got. Throws std::bad_alloc when a fresh array cannot
/// be allocated, `retained` then unchanged.
template <typename Pack>
void packIntoRetained(QByteArray& retained, qsizetype max_bytes, Pack&& pack) {
  const qsizetype needed = PJ::paddedUploadSize(max_bytes);
  const bool reused = retained.isDetached() && retained.capacity() >= needed && retained.capacity() <= 2 * needed;
  if (reused) {
    retained.resize(needed);
  } else {
    retained = QByteArray(needed, Qt::Uninitialized);  // resize() would round the capacity up
  }
  const qsizetype used =
      pack(std::as_writable_bytes(std::span<char>(retained.data(), static_cast<std::size_t>(needed))), reused);
  PJ::padForUpload(retained, used);
}

// Non-owning QRhi renderer contract for point-like layers. QObject ownership,
// decoding, persistence, and warnings remain in each concrete ISceneLayer; this
// interface only lets the QRhi view share one bounded vertex/GPU path. The
// layers implement it in both builds; the GL build never reads it.
class PointRenderable {
 public:
  virtual ~PointRenderable() = default;

  virtual bool prepareForRender(std::uint64_t remaining_view_vertices) = 0;
  /// The retained cloud: pointCount() records of vertexFormat(), padded for one
  /// upload (PJ::padForUpload()). Implicitly shared: the view
  /// hands this array to the GPU upload without copying it, so a layer writes into
  /// it only while no one else holds it (QByteArray::isDetached()); a write to a
  /// shared array would detach and bring the copy back.
  [[nodiscard]] virtual const QByteArray& vertexBytes() const = 0;
  /// The layout of vertexBytes(); it changes only with a new geometryRevision(). A
  /// format drops only what the current style cannot read: the scalar is stored
  /// whenever a colormapped field needs it (colour, range cull, fade).
  [[nodiscard]] virtual PointVertexFormat vertexFormat() const = 0;
  [[nodiscard]] virtual std::size_t pointCount() const = 0;
  [[nodiscard]] virtual std::uint64_t geometryRevision() const = 0;
  /// The view enqueued the upload of geometry `revision` (vertexBytes() as it was).
  /// The update batch holds its own reference to the array until the frame is
  /// submitted, so a layer may drop its reference now, keeping pointCount(),
  /// sourceBounds(), vertexFormat() and the revision; ensureVertexBytes() rebuilds
  /// it when a new QRhi needs the upload again.
  virtual void onVerticesUploaded(std::uint64_t revision) {
    static_cast<void>(revision);
  }
  /// Called before an upload: rebuilds vertexBytes() if the layer dropped it. False
  /// when it could not (the sample went away); the layer then holds no points.
  virtual bool ensureVertexBytes() {
    return true;
  }
  // Frame the vertices are expressed in (empty until data arrives). Distinct from
  // Scene3DLayer::sourceFrame() (a QString for the dock) so one class can implement both.
  [[nodiscard]] virtual const std::string& renderFrameId() const = 0;
  /// The finite extent of vertexBytes() in renderFrameId(); set whenever the cloud
  /// is non-empty (the point pass asserts it and does not scan).
  [[nodiscard]] virtual std::optional<AABB> sourceBounds() const = 0;
  [[nodiscard]] virtual bool visible() const = 0;
  [[nodiscard]] virtual PointShape shape() const = 0;
  [[nodiscard]] virtual PointColorType colorType() const = 0;
  [[nodiscard]] virtual float sizeMeters() const = 0;
  [[nodiscard]] virtual float sizePixels() const = 0;
  [[nodiscard]] virtual QColor solidColor() const = 0;
  [[nodiscard]] virtual PJ::Colormap colormap() const = 0;
  [[nodiscard]] virtual bool invertLut() const = 0;
  [[nodiscard]] virtual float outsideRangeAlpha() const = 0;
  [[nodiscard]] virtual int scalarAxis() const = 0;
  [[nodiscard]] virtual std::pair<float, float> scalarRange(const glm::mat4& fixed_from_source) const = 0;
  virtual void noteRenderedScalarRange(float minimum, float maximum) = 0;
  [[nodiscard]] virtual bool isDepthCloud() const = 0;
};

// Persisted XML spelling of a colormap and its inverse, shared by every
// point-colored layer so the layout grammar has exactly one mapping. Distinct
// from PJ::colormapName(), the untranslated display label.
[[nodiscard]] inline QString colormapToken(PJ::Colormap colormap) {
  switch (colormap) {
    case PJ::Colormap::kTurbo:
      return QStringLiteral("turbo");
    case PJ::Colormap::kViridis:
      return QStringLiteral("viridis");
    case PJ::Colormap::kPlasma:
      return QStringLiteral("plasma");
    case PJ::Colormap::kGrayscale:
      return QStringLiteral("grayscale");
  }
  return QStringLiteral("turbo");
}

[[nodiscard]] inline std::optional<PJ::Colormap> parseColormap(const QString& text) {
  for (const PJ::Colormap value :
       {PJ::Colormap::kTurbo, PJ::Colormap::kViridis, PJ::Colormap::kPlasma, PJ::Colormap::kGrayscale}) {
    if (text == colormapToken(value)) {
      return value;
    }
  }
  return std::nullopt;
}

}  // namespace pj::scene3d
