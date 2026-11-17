#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QDomDocument>
#include <QDomElement>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"            // PJ::ObjectTopicId, PJ::SequentialUID
#include "pj_scene3d_core/poses_in_frame_render.h"  // PoseTriadInstance
#include "pj_scene3d_core/scene_limits.h"           // BudgetLatch
#include "pj_scene3d_widgets/layers/trail_layer.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

class QWidget;

namespace pj::scene3d {

// Scene3DLayer for a PJ.PosesInFrame topic (geometry_msgs/PoseArray equivalent, kPosesInFrame). At each tracker tick
// it decodes the pose set valid at that time and expands every pose into a coordinate-axis triad
// (buildPoseTriadInstances); SceneViewWidget reads the arms through prepareForRender()/instances() and draws them
// instanced, so an AMCL-scale pose cloud is one draw call. The single shared frame_id is resolved to the fixed frame
// at render time.
//
// Per-layer params (arrow length, opacity, X-arrow-only, color override) are viewer style, never in the message; they
// persist via xmlSaveState/xmlLoadState, which also makes them copy/paste/apply-to-family-able through
// serializeLayerParams (pj_scene_common).
class PosesInFrameLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  PosesInFrameLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~PosesInFrameLayer() override;

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] SceneContributions sceneContributions() override {
    return {.poses = this};
  }
  [[nodiscard]] QString sourceFrame() const override;
  // This layer's decode warning, else the owned trail's ("waiting for TF", decode failures): the dock polls only
  // REGISTERED layers, so without the relay an embedded trail that cannot draw would fail silently.
  [[nodiscard]] QString statusWarning() const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  void setVisible(bool visible) override;

  // Coalesces repaints to sample changes, except while the owned trail is on: its past/future split moves every tick.
  [[nodiscard]] std::uint64_t renderKey(PJ::Timepoint time) const override;

  // QRhi view hook, once per frame for a visible layer: drains a pending tracker move or style edit and applies the
  // view's arm budget. A layer over the remaining budget drops its geometry and warns, retrying once the budget frees
  // up. True when instances() or the warning changed.
  bool prepareForRender(std::uint64_t remaining_view_arms);
  [[nodiscard]] std::optional<AABB> sourceBounds() const {
    return source_bounds_.valid ? std::optional<AABB>{source_bounds_} : std::nullopt;
  }
  [[nodiscard]] const std::vector<PoseTriadInstance>& instances() const {
    return instances_;
  }
  [[nodiscard]] std::uint64_t geometryRevision() const {
    return geometry_revision_;
  }
  [[nodiscard]] const std::string& renderFrameId() const override {
    return source_frame_;
  }
  [[nodiscard]] std::optional<PJ::Timepoint> renderStamp() const override {
    return render_stamp_;
  }
  [[nodiscard]] bool visible() const {
    return visible_;
  }
  [[nodiscard]] static bool validateXml(const QDomElement& element);

  QWidget* createConfigWidget(QWidget* parent) override;

  // Per-layer display params (config widget + XML): size is the triad arm length in metres, opacity (0..1) the alpha.
  void setGizmoSize(float meters);
  void setGizmoOpacity(float opacity);
  [[nodiscard]] float gizmoSize() const {
    return gizmo_size_;
  }
  [[nodiscard]] float gizmoOpacity() const {
    return gizmo_opacity_;
  }

  // X-arrow-only: a single X-axis arrow per pose instead of the full XYZ triad. Geometry only, orthogonal to the colour
  // override.
  void setXArrowOnly(bool x_arrow_only);
  [[nodiscard]] bool xArrowOnly() const {
    return x_arrow_only_;
  }

  // Colour override: every produced arm (the single X arm or all three triad arms) takes one shared colour instead of
  // the per-axis R/G/B, in both X-only and full-triad mode.
  void setOverrideColorEnabled(bool enabled);
  void setOverrideColor(QColor color);
  [[nodiscard]] bool overrideColorEnabled() const {
    return override_color_enabled_;
  }
  [[nodiscard]] QColor overrideColor() const {
    return override_color_;
  }

  // Trail: an optional TrailLayer this layer OWNS outright. It is never registered with the dock (no Topics row, no
  // settings panel of its own): its style rows are appended to this layer's config widget and the forwarding hooks
  // below attach, render and tear it down. Enabling builds it, sampling the topic's whole history (hence opt-in);
  // disabling destroys it, discarding the style.
  void setTrailEnabled(bool enabled);
  [[nodiscard]] bool trailEnabled() const {
    return trail_ != nullptr;
  }
  // The owned trail, or nullptr when disabled. The dock reaches through this to re-bind the TF buffer, because layout
  // restore replays layers BEFORE the <config_topic> that binds TF.
  [[nodiscard]] TrailLayer* trail() const {
    return trail_.get();
  }

#ifdef PJ_SCENE3D_TEST_HOOKS
  void renderAtForTest(int64_t time_ns) {
    updateAt(time_ns);
  }
  [[nodiscard]] const std::vector<PoseTriadInstance>& instancesForTest() const {
    return instances_;
  }
#endif

 private:
  struct ParsedSettings {
    float gizmo_size = 0.15f;
    float gizmo_opacity = 1.0f;
    bool x_arrow_only = false;
    bool override_color_enabled = false;
    std::optional<QColor> override_color;
  };
  // The one <poses_in_frame> validator behind xmlLoadState and validateXml; a nested <trail> is structurally allowed,
  // its content is the trail's business.
  [[nodiscard]] static std::optional<ParsedSettings> parseSettings(const QDomElement& element);
  bool bootstrap();
  // Decode the sample valid at time_ns and expand it into instances_. No-op when the same sample is already staged with
  // the current style. A bad sample fails closed: no geometry plus a statusWarning(). True when instances_ or the
  // warning changed.
  bool updateAt(int64_t time_ns);
  // Drops staged geometry and the replay cursors. Called on attach/detach: a prior attachment's UID/frame may belong to
  // a timeline that no longer exists (dataset replace), so it must never anchor or skip the new generation.
  void resetReplayState();
  void clearGeometry();
  void markDirty();
  // Applies a pending tracker move or style edit at requested_time_ (once per painted frame, by prepareForRender()).
  bool drainPendingUpdate();
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);
  void updateSourceFrame(const std::string& frame_id);
  // Brings a freshly created trail up to this layer's current lifecycle state (context, fixed frame, tracker time,
  // visibility): enabling can happen at any point after attach.
  void syncTrailToCurrentState();

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  QMetaObject::Connection reload_connection_;
  // Parsers are deliberately NOT cached: every decode resolves a fresh ParserBinding through ctx_.session
  // (resolveObject()), so a file reload that re-registers the topic's parser slot cannot leave a dangling pointer.

  std::string source_frame_;
  std::optional<PJ::Timepoint> render_stamp_;  // objectStamp() of the staged sample
  // Last fixed frame pushed by the dock: renderKey folds fixed<-source, and a trail enabled later rebuilds against it.
  QString fixed_frame_;
  bool visible_ = true;
  // Set by setTrackerTime / a style edit, drained once per painted frame (drainPendingUpdate) so a fast scrub or slider
  // drag re-expands only the final state.
  bool tracker_dirty_ = false;
  std::optional<PJ::Timepoint> requested_time_;
  BudgetLatch budget_;

  // Viewer style baked into the instance buffer; defaults match the TF "Frames" gizmos. A change bumps style_revision_
  // so updateAt re-expands the current sample instead of skipping it as unchanged.
  float gizmo_size_ = 0.15f;
  float gizmo_opacity_ = 1.0f;
  bool x_arrow_only_ = false;
  bool override_color_enabled_ = false;
  QColor override_color_{255, 0, 0};  // red, the panel-wide override/solid default
  int style_revision_ = 0;

  // Identity of the sample + style currently staged: a scrub within one message (or an unchanged style) skips the
  // parse/expand/upload.
  PJ::SequentialUID staged_uid_{};
  int staged_revision_ = -1;

  std::vector<PoseTriadInstance> instances_;  // last expansion (frame-local; TF applied at draw)
  AABB source_bounds_;
  std::uint64_t geometry_revision_ = 0;

  // Owned outright (no QObject parent) so destruction is deterministic and the dock never sees it as a layer.
  std::unique_ptr<TrailLayer> trail_;
};

}  // namespace pj::scene3d
