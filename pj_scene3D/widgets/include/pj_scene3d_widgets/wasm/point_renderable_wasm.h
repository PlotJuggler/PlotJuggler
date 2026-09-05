#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QString>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_scene3d_core/camera/camera.h"
#include "pj_widgets/Colormap.h"

namespace pj::scene3d {

// GPU-ready vertex shared by the browser's canonical PointCloud and DepthCloud
// adapters. Positions stay in the source frame; SceneViewWidget applies
// fixed<-source so camera/TF changes never rebuild the retained CPU vertices.
struct WasmPointVertex {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  float scalar = 0.0F;
  std::uint32_t rgba = 0xFFFFFFFFU;  // byte0=R, byte1=G, byte2=B, byte3=A
};
static_assert(sizeof(WasmPointVertex) == 20);

enum class WasmPointShape : std::int32_t { kSphere, kPoint, kCube };
enum class WasmPointColorType : std::int32_t { kField, kSolid, kRgb };

// Non-owning renderer contract for point-like WASM layers. QObject ownership,
// decoding, persistence, and warnings remain in each concrete ISceneLayer; this
// interface only lets the QRhi view share one bounded vertex/GPU path.
class WasmPointRenderable {
 public:
  virtual ~WasmPointRenderable() = default;

  virtual bool prepareForRender(std::uint64_t remaining_view_vertices) = 0;
  [[nodiscard]] virtual const std::vector<WasmPointVertex>& vertices() const = 0;
  [[nodiscard]] virtual std::uint64_t geometryRevision() const = 0;
  [[nodiscard]] virtual const std::string& sourceFrame() const = 0;
  [[nodiscard]] virtual std::optional<AABB> sourceBounds() const = 0;
  [[nodiscard]] virtual bool visible() const = 0;
  [[nodiscard]] virtual WasmPointShape shape() const = 0;
  [[nodiscard]] virtual WasmPointColorType colorType() const = 0;
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
