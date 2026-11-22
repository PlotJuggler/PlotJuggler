#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QObject>
#include <QString>
#include <QStringList>
#include <chrono>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "pj_scene3d_core/camera/camera.h"  // AABB
#include "pj_scene_common/scene_layer.h"
#include "pj_widgets/Fnv1a.h"

class QWidget;

namespace PJ {
class SessionManager;
}  // namespace PJ

namespace pj::tf {
class TransformBuffer;
}  // namespace pj::tf

namespace pj::scene3d {

using pj::tf::TransformBuffer;
struct PlacementHold;
class PointRenderable;
class ModelRenderable;
class PosesInFrameLayer;
class OccupancyGridLayer;
class VoxelGridLayer;
class SceneEntitiesLayer;
class GridMapLayer;
class MeshMapLayer;
class TrailLayer;

// What one layer draws, family by family, each through the family's typed interface: the layer's registration with the
// view (sceneContributions()), read once per SceneViewWidget::setLayers(), never per frame. A layer may contribute to
// several families (a SceneEntities layer draws markers and models; a pose layer its triads and the trail it owns,
// which comes and goes between frames without a new registration); all its draws keep its logical (dock) slot, and
// within the slot the families submit in the view's fixed family order. A layer with no contribution draws nothing.
struct SceneContributions {
  PointRenderable* points = nullptr;
  TrailLayer* trail = nullptr;         ///< a standalone trail
  PosesInFrameLayer* poses = nullptr;  ///< its triads, and the trail it owns (poses->trail())
  OccupancyGridLayer* occupancy = nullptr;
  VoxelGridLayer* voxels = nullptr;
  SceneEntitiesLayer* markers = nullptr;
  ModelRenderable* models = nullptr;
  GridMapLayer* grid_map = nullptr;
  MeshMapLayer* mesh_map = nullptr;
};

// Folds a transform's 16 bit patterns (float or double) into an in-process key (FNV-1a style). The key changes only
// when the pose moves: with held TF lookups a 60 Hz playhead coalesces repaints down to the TF update rate, with
// interpolated ones a moving frame repaints on every tick.
template <typename T>
[[nodiscard]] uint64_t foldTransformIntoKey(uint64_t key, const glm::mat<4, 4, T>& transform) {
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      key = PJ::fnv1aMixBits(key, transform[col][row]);
    }
  }
  return key;
}

// Folds the fixed<-source transform the view places the layer with (placeObject(): the object's own stamp, else the
// cursor `time`, with the layer's `hold`) into a sample key: the second half of every frame-placed layer's renderKey
// (sample identity + transform). An unresolved lookup folds nothing, so resolving or losing the transform changes the
// key too. Without it a static sample in a moving frame never repaints.
[[nodiscard]] uint64_t foldFrameTransformIntoKey(
    uint64_t key, const TransformBuffer* tf_buffer, const std::string& fixed_frame, const std::string& source_frame,
    std::optional<PJ::Timepoint> object_stamp, PJ::Timepoint time, PlacementHold* hold);

// Widget-global services every 3D layer needs at attach time. Scene3DDockWidget always passes this concrete context,
// and concrete layers static_cast PJ::SceneLayerContext to it in attach() to retrieve the per-dataset TF buffer.
struct Scene3DLayerContext : PJ::SceneLayerContext {
  std::shared_ptr<TransformBuffer> tf_buffer;
};

// Abstract base for everything the 3D scene renders for a user-added topic: identity, lifecycle, config, XML,
// visibility and repaint come from PJ::ISceneLayer; this adds the 3D frame hooks consumed by Scene3DDockWidget and
// SceneViewWidget. Layers own no GPU resource: the view owns all GPU state.
class Scene3DLayer : public PJ::ISceneLayer {
  Q_OBJECT
 public:
  explicit Scene3DLayer(QObject* parent = nullptr);
  ~Scene3DLayer() override;

  // Source frames a layer can render in if the user-picked fixed frame cannot resolve via TF.
  [[nodiscard]] virtual QStringList fallbackFrames() const = 0;

  // The primary frame this layer's data is expressed in: the dock decides from it whether the layer can be transformed
  // into the selected fixed frame and marks the layer with a warning in the topic list if not. What counts as
  // "primary": PointCloud / GridMap    : message header.frame_id URDF / RobotDescription : root link frame (e.g.
  // base_link) Marker arrays           : the first entity's frame_id (the message-level frame; entities in other frames
  // are placed per frame, see placementHoldFor()) Empty when no data has been decoded yet: the dock treats that as
  // "pending data", not "broken".
  [[nodiscard]] virtual QString sourceFrame() const = 0;

  // The scene families this layer draws through (see SceneContributions); constant for the layer's lifetime.
  [[nodiscard]] virtual SceneContributions sceneContributions() = 0;

  // The frame the view places this layer's geometry from; empty before data arrives, and for a trail (its points are
  // already in the fixed frame). One override also implements PointRenderable's and ModelRenderable's.
  [[nodiscard]] virtual const std::string& renderFrameId() const;

  // The rendered object's own stamp on the TF header clock (objectStamp()), or nullopt when it has none: the view then
  // places the layer at the cursor in the entry clock (placeObject()). Overridden by layers that draw one stamped
  // object; one override also implements PointRenderable's.
  [[nodiscard]] virtual std::optional<PJ::Timepoint> renderStamp() const {
    return std::nullopt;
  }

  // What placeObject() keeps for this layer's stamped object, so a TF sample evicted under it does not move it. The
  // view, the dock's scene bounds and renderKey() all place the layer with it, so they agree. Virtual only so a layer
  // that is also a PointRenderable overrides both with one function; it returns this one.
  [[nodiscard]] virtual PlacementHold* placementHold() const {
    return placement_hold_.get();
  }
  // The hold that places this layer's geometry in `frame`: placementHold() for a layer drawn in one frame. A layer
  // drawn in several (a SceneEntities finding) keeps one per frame, or placing one frame would overwrite another's.
  [[nodiscard]] virtual PlacementHold* placementHoldFor(const std::string& /*frame*/) const {
    return placementHold();
  }

  // Source-frame extent of this layer's geometry; nullopt when it reports none (no data decoded yet, or a bounds-less
  // kind). The dock transforms each box into the current fixed frame before unioning them for camera framing and
  // adaptive near/far.
  [[nodiscard]] virtual std::optional<AABB> worldBounds() const {
    return std::nullopt;
  }

  // The layer's own row warning (icon + tooltip), distinct from the dock's frame-orphan check: its warning() parts
  // (decode/admission/budget, renderer, derived topic), one per line; empty means nothing to warn about. The dock
  // combines it with the orphan state (Scene3DDockWidget::updateLayerWarning); statusWarningChanged() fires whenever
  // the value changes.
  [[nodiscard]] virtual QString statusWarning() const {
    return warning();
  }

  // Renderer part of statusWarning(): a GPU-side failure (texture limit, allocation, per-view budget) reported by the
  // QRhi view. A later success clears only this part.
  void noteRenderFailure(QString warning);
  void noteRenderSuccess();

 signals:
  // The dock unions new source-frame candidates into the fixed-frame combo's fallback list.
  void fallbackFramesChanged(const QStringList& frames);

  // sourceFrame()'s value changed: the dock re-runs its orphan check (event-driven, no polling per tracker tick).
  void sourceFrameChanged(const QString& new_frame);

  // statusWarning()'s value changed: the dock re-runs its per-layer warning combine so the row icon/tooltip stays in
  // sync. Routed through recomputeOrphanStates, NOT the base relay, so it merges with the orphan state instead of
  // overwriting it.
  void statusWarningChanged();

 protected:
  void setDataWarning(QString warning) {
    setWarningPart(PJ::SceneWarningParts::Part::kData, std::move(warning));
  }
  [[nodiscard]] const QString& dataWarning() const;
  [[nodiscard]] const QString& renderWarning() const;
  // Called after the renderer part changed, for a layer that mirrors it elsewhere (e.g. a status line).
  virtual void renderWarningChanged() {}
  // The dock merges the warning with the orphan state (statusWarningChanged), never the base relay.
  void onWarningChanged() override {
    emit statusWarningChanged();
  }

 private:
  std::unique_ptr<PlacementHold> placement_hold_;  // never null
};

}  // namespace pj::scene3d
