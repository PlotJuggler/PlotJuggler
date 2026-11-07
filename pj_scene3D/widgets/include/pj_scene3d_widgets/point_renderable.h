#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QColor>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "pj_base/time.hpp"
#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/pointcloud.h"  // PointVertex
#include "pj_scene3d_widgets/layer_style.h"
#include "pj_widgets/Colormap.h"

namespace pj::scene3d {

struct PlacementHold;

// Non-owning renderer contract for point-like layers. QObject ownership,
// decoding, persistence, and warnings remain in each concrete ISceneLayer; this
// interface only lets the view share one bounded vertex/GPU path.
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
  /// The cloud's own stamp on the TF header clock, or nullopt to place it at the
  /// cursor; see Scene3DLayer::renderStamp().
  [[nodiscard]] virtual std::optional<PJ::Timepoint> renderStamp() const {
    return std::nullopt;
  }
  /// The hold the view places the cloud with; see Scene3DLayer::placementHold().
  [[nodiscard]] virtual PlacementHold* placementHold() const {
    return nullptr;
  }
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

}  // namespace pj::scene3d
