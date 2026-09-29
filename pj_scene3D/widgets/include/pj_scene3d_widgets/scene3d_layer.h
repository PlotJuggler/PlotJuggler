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

namespace pj::scene3d {

class TransformBuffer;

// Fold a transform's 16 bit patterns (float or double) into an in-process key
// (FNV-1a style). The TF is a zero-order hold, so keys only change when the pose
// actually moves — what lets a 60 Hz playhead coalesce repaints down to the TF
// update rate.
template <typename T>
[[nodiscard]] uint64_t foldTransformIntoKey(uint64_t key, const glm::mat<4, 4, T>& transform) {
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      key = PJ::fnv1aMixBits(key, transform[col][row]);
    }
  }
  return key;
}

// Folds the fixed<-source transform at `time` into a sample key, the second half
// of every frame-placed layer's renderKey (sample identity + transform). An
// unresolved lookup folds nothing, so resolving or losing the transform changes
// the key too. Without it a static sample in a moving frame never repaints.
[[nodiscard]] uint64_t foldFrameTransformIntoKey(
    uint64_t key, const TransformBuffer* tf_buffer, const std::string& fixed_frame, const std::string& source_frame,
    PJ::Timepoint time);

// Widget-global services every 3D layer needs at attach time. Scene3DDockWidget
// always passes this concrete context to Scene3DLayer instances; concrete layers
// static_cast PJ::SceneLayerContext to Scene3DLayerContext in attach() and rely
// on that invariant to retrieve the per-dataset TF buffer.
struct Scene3DLayerContext : PJ::SceneLayerContext {
  std::shared_ptr<TransformBuffer> tf_buffer;
};

// Abstract base for everything the 3D scene renders on behalf of a user-added
// topic. Generic identity/lifecycle/config/XML/visibility/repaint behavior is
// inherited from PJ::ISceneLayer; this type adds only 3D-specific frame hooks
// consumed by Scene3DDockWidget and SceneViewWidget. Layers own no GPU resource:
// the view owns all GPU state.
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

  // Source-frame extent of this layer's geometry, or nullopt when
  // the layer reports no bounds (no data decoded yet, or a bounds-less layer
  // kind). The dock transforms each box into the current fixed frame before
  // unioning them for camera framing and adaptive near/far. NON-pure so existing
  // layer kinds need no change; concrete kinds with geometry override it.
  [[nodiscard]] virtual std::optional<AABB> worldBounds() const {
    return std::nullopt;
  }

  // The layer's own row warning (icon + tooltip), distinct from the dock's
  // frame-orphan check: the data part (decode, admission, budget) then the
  // renderer part, one per line. Empty means nothing to warn about. The dock
  // combines it with the orphan state (Scene3DDockWidget::updateLayerWarning).
  // statusWarningChanged() fires whenever the value changes.
  [[nodiscard]] virtual QString statusWarning() const {
    return warning_;
  }

  // Renderer part of statusWarning(): a GPU-side failure (texture limit,
  // allocation, per-view budget) reported by the QRhi view. A later success
  // clears only this part.
  void noteRenderFailure(QString warning);
  void noteRenderSuccess();

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

 protected:
  // Data part of statusWarning().
  void setDataWarning(QString warning);
  [[nodiscard]] const QString& dataWarning() const {
    return data_warning_;
  }
  [[nodiscard]] const QString& renderWarning() const {
    return render_warning_;
  }
  // Called after the renderer part changed (a layer that mirrors it elsewhere,
  // e.g. in a status line, refreshes there).
  virtual void renderWarningChanged() {}

 private:
  void updateWarning();

  QString data_warning_;
  QString render_warning_;
  QString warning_;  // data_warning_ + render_warning_, cached for statusWarning()
};

}  // namespace pj::scene3d
