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
#include <cstring>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <utility>

#include "pj_scene3d_core/camera/camera.h"  // AABB
#include "pj_scene_common/scene_layer.h"

class QWidget;

namespace PJ {
class SessionManager;
}  // namespace PJ

namespace pj::scene3d {

class TransformBuffer;
struct ViewParams;
struct FrameContext;

// Fold a transform's 16 float bit patterns into a renderKey (FNV-1a style). The
// TF is a zero-order hold, so keys only change when the pose actually moves —
// what lets a 60 Hz playhead coalesce repaints down to the TF update rate.
[[nodiscard]] inline uint64_t foldTransformIntoKey(uint64_t key, const glm::mat4& transform) {
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      uint32_t bits = 0;
      std::memcpy(&bits, &transform[col][row], sizeof(bits));
      key = (key ^ bits) * 0x100000001b3ULL;
    }
  }
  return key;
}

// Widget-global services every 3D layer needs at attach time. Scene3DDockWidget
// always passes this concrete context to Scene3DLayer instances; concrete layers
// static_cast PJ::SceneLayerContext to Scene3DLayerContext in attach() and rely
// on that invariant to retrieve the per-dataset TF buffer.
struct Scene3DLayerContext : PJ::SceneLayerContext {
  std::shared_ptr<TransformBuffer> tf_buffer;
};

// Abstract base for everything the 3D scene renders on behalf of a user-added
// topic. Generic identity/lifecycle/config/XML/visibility/repaint behavior is
// inherited from PJ::ISceneLayer; this type adds only 3D-specific frame and GL
// hooks consumed by Scene3DDockWidget and SceneViewWidget.
//
// Concrete subclasses live under
// `pj_scene3D/widgets/include/pj_scene3d_widgets/layers/`.
class Scene3DLayer : public PJ::ISceneLayer {
  Q_OBJECT
 public:
  explicit Scene3DLayer(QObject* parent = nullptr);
  ~Scene3DLayer() override;

  // Reports source frames a layer can render in if the user-picked fixed-frame
  // cannot resolve via TF.
  [[nodiscard]] virtual QStringList fallbackFrames() const = 0;

  // The primary frame this layer's data is expressed in. Used by the dock to
  // decide whether the layer can be transformed into the selected fixed frame;
  // if not, the layer is marked with a warning in the topic list.
  //
  // Concrete layer kinds choose what counts as "primary":
  //   PointCloud / LaserScan / GridMap : message header.frame_id
  //   URDF / RobotDescription          : root link frame (e.g. base_link)
  //   Marker arrays                    : message-level header.frame_id
  //                                      (per-marker frame overrides are a
  //                                      v2 refinement)
  // Return an empty string when no data has been decoded yet — the dock
  // treats that as "not warning, pending data" rather than "broken".
  [[nodiscard]] virtual QString sourceFrame() const = 0;

  // GL. initializeGL() is called by the SceneViewWidget at the top of
  // every paintGL with a current GL context. Subclasses are expected to
  // guard against double-init internally (`if (initialized_) return;`)
  // — mirroring the idiom each IRenderPass already uses. The
  // per-frame call is what lets layers added *after* the widget has
  // been realised initialise their GL state on the first paint they
  // see, without requiring the dock to hold a current context at
  // attach time. render() is called every paintGL frame; the layer
  // internally sequences its one or more IRenderPass instances. frame_ctx
  // (TF buffer + fixed frame + time) is borrowed for the call's duration —
  // layers must not store it.
  virtual void initializeGL() = 0;
  virtual void render(const ViewParams& view_params, const FrameContext& frame_ctx) = 0;

  // Drop the GL resources owned by this layer's render pass(es), returning
  // them to their pre-initializeGL state. Called by SceneViewWidget when the
  // GL context is about to be destroyed (see IRenderPass::releaseGL); the
  // layer re-initializes lazily on the next paintGL.
  virtual void releaseGL() = 0;

  // Source-frame extent of this layer's geometry, or nullopt when
  // the layer reports no bounds (no data decoded yet, or a bounds-less layer
  // kind). The dock transforms each box into the current fixed frame before
  // unioning them for camera framing and adaptive near/far. NON-pure so existing
  // layer kinds need no change; concrete kinds with geometry override it.
  [[nodiscard]] virtual std::optional<AABB> worldBounds() const {
    return std::nullopt;
  }

  // World-space AABB of this layer's shadow-CASTER geometry at the current frame, or
  // nullopt for layers that cast nothing. DISTINCT from worldBounds() on purpose:
  // worldBounds() drives camera framing (and RobotModelLayer deliberately reports
  // none so a moving robot can't yank the camera), whereas the shadow frustum MUST
  // enclose the robot mesh. Only mesh-bearing layers (RobotModelLayer,
  // SceneEntitiesLayer model primitives) override this; everything else casts no
  // shadow. Non-const and FrameContext-taking because the bounds depend on the
  // current TF poses (the layer rebuilds its draw list to compute them).
  [[nodiscard]] virtual std::optional<AABB> meshShadowBounds(const FrameContext& /*frame_ctx*/) {
    return std::nullopt;
  }

  // Depth-only render of this layer's shadow casters from the light's point of view,
  // into the shadow map currently bound by the SceneViewWidget pre-pass.
  // `light_view_proj` is the world->light-clip matrix from fitDirectionalShadowCamera.
  // Default no-op: only mesh layers cast (they forward their visual draws to their
  // MeshRenderPass, which owns the per-context depth-only program). REQUIRES a current
  // GL context (called from the pre-pass, before renderScene).
  virtual void renderShadowCasters(const glm::mat4& /*light_view_proj*/, const FrameContext& /*frame_ctx*/) {}

  // A layer-specific warning to surface on the layer's row (icon + tooltip),
  // distinct from the dock's frame-orphan check — e.g. a model that failed to
  // fetch or import. Empty means "nothing to warn about". The dock OR-combines
  // this with the orphan state so neither clobbers the other (see
  // Scene3DDockWidget::recomputeOrphanStates). Default: no layer warning.
  // Emit statusWarningChanged() whenever the returned value changes.
  [[nodiscard]] virtual QString statusWarning() const {
    return {};
  }

 signals:
  // Layer noticed new source-frame candidates — the dock unions these
  // into the fixed-frame combo's fallback list.
  void fallbackFramesChanged(const QStringList& frames);

  // The value sourceFrame() would return has changed — the dock listens
  // and re-runs its orphan check (event-driven so it doesn't have to poll
  // per tracker tick).
  void sourceFrameChanged(const QString& new_frame);

  // The value statusWarning() would return has changed — the dock re-runs its
  // per-layer warning combine so the row icon/tooltip stays in sync without
  // polling. (Routed through recomputeOrphanStates, NOT the base relay, so it
  // merges with the orphan state instead of overwriting it.)
  void statusWarningChanged();
};

}  // namespace pj::scene3d
