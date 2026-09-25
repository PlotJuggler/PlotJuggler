#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QColor>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_widgets/layer_style.h"
#include "pj_widgets/Colormap.h"

namespace pj::scene3d {

// GPU-ready vertex retained by PointCloudLayer and DepthCloudLayer for the QRhi
// view. Positions stay in the source frame; SceneViewWidget applies
// fixed<-source so camera/TF changes never rebuild the retained CPU vertices.
struct PointVertex {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  float scalar = 0.0F;
  std::uint32_t rgba = 0xFFFFFFFFU;  // byte0=R, byte1=G, byte2=B, byte3=A
};
static_assert(sizeof(PointVertex) == 20);

// Non-owning QRhi renderer contract for point-like layers. QObject ownership,
// decoding, persistence, and warnings remain in each concrete ISceneLayer; this
// interface only lets the QRhi view share one bounded vertex/GPU path. The
// layers implement it in both builds; the GL build never reads it.
class PointRenderable {
 public:
  virtual ~PointRenderable() = default;

  virtual bool prepareForRender(std::uint64_t remaining_view_vertices) = 0;
  /// The retained cloud, packed PointVertex records. Implicitly shared: the view
  /// hands this array to the GPU upload without copying it, so a layer never writes
  /// into an array it has handed out (that would detach and bring the copy back);
  /// every sample gets a fresh one.
  [[nodiscard]] virtual const QByteArray& vertexBytes() const = 0;
  [[nodiscard]] std::size_t pointCount() const {
    return static_cast<std::size_t>(vertexBytes().size()) / sizeof(PointVertex);
  }
  [[nodiscard]] virtual std::uint64_t geometryRevision() const = 0;
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
