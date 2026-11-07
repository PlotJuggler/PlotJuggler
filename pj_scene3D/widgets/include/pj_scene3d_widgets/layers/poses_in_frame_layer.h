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

// Scene3DLayer for a PJ.PosesInFrame topic (geometry_msgs/PoseArray equivalent,
// kPosesInFrame). At each tracker tick it decodes the pose set valid at that time
// and expands every pose into a coordinate-axis triad (buildPoseTriadInstances).
// SceneViewWidget reads the arms through prepareForRender()/instances() and draws
// them instanced, so an AMCL-scale pose cloud stays one draw call. The single shared frame_id is resolved to the fixed
// frame at render time.
//
// The per-layer params (arrow length, opacity, X-arrow-only, and a color
// override) are viewer style, never in the message; they persist via
// xmlSaveState/xmlLoadState (which also makes them copy/paste/apply-to-family-able
// through pj_scene_common's serializeLayerParams).
class PosesInFrameLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  PosesInFrameLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~PosesInFrameLayer() override;

  // ISceneLayer / Scene3DLayer
  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] SceneContributions sceneContributions() override {
    return {.poses = this};
  }
  [[nodiscard]] QString sourceFrame() const override;
  // This layer's decode warning, else the owned trail's ("waiting for TF", decode
  // failures): the dock only polls REGISTERED layers, so without the relay an
  // embedded trail that cannot draw would fail silently.
  [[nodiscard]] QString statusWarning() const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  void setVisible(bool visible) override;

  // Coalesces repaints to sample changes, except while the owned trail is on:
  // its past/future split moves with every tick.
  [[nodiscard]] std::uint64_t renderKey(PJ::Timepoint time) const override;

  // QRhi view hook, called once per frame for a visible layer: drains a pending
  // tracker move or style edit and applies the view's arm budget. A layer over
  // the remaining budget drops its geometry and warns; it retries once the budget
  // frees up. Returns true when instances() or the warning changed.
  bool prepareForRender(std::uint64_t remaining_view_arms);
  // Source-frame extent of instances() (arm tips padded by the cone radius).
  [[nodiscard]] std::optional<AABB> sourceBounds() const {
    return source_bounds_.valid ? std::optional<AABB>{source_bounds_} : std::nullopt;
  }
  // Frame-local arms of the current sample; bumped geometryRevision() on change.
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
  // Validates a <poses_in_frame> payload without a layer (layout pre-flight).
  [[nodiscard]] static bool validateXml(const QDomElement& element);

  QWidget* createConfigWidget(QWidget* parent) override;

  // Per-layer display params, driven by the config widget + XML. Size is the
  // triad arm length in meters; opacity (0..1) is the gizmo alpha.
  void setGizmoSize(float meters);
  void setGizmoOpacity(float opacity);
  [[nodiscard]] float gizmoSize() const {
    return gizmo_size_;
  }
  [[nodiscard]] float gizmoOpacity() const {
    return gizmo_opacity_;
  }

  // X-arrow-only mode: draw a single X-axis arrow per pose instead of the full
  // XYZ triad. Geometry only — orthogonal to the color override below.
  void setXArrowOnly(bool x_arrow_only);
  [[nodiscard]] bool xArrowOnly() const {
    return x_arrow_only_;
  }

  // Color override: when enabled, every produced arm (the single X arm OR all
  // three triad arms) is recolored with one shared color instead of the natural
  // per-axis R/G/B. Works in both X-only and full-triad mode.
  void setOverrideColorEnabled(bool enabled);
  void setOverrideColor(QColor color);
  [[nodiscard]] bool overrideColorEnabled() const {
    return override_color_enabled_;
  }
  [[nodiscard]] QColor overrideColor() const {
    return override_color_;
  }

  // Trail: an optional TrailLayer this layer OWNS outright. It is never
  // registered with the dock, so it gets no Topics row and no settings panel of
  // its own — its style rows are appended to this layer's config widget, and it
  // is attached/rendered/torn down by the forwarding hooks below. Enabling
  // builds it (which samples the topic's whole history, hence opt-in);
  // disabling destroys it, discarding the style.
  void setTrailEnabled(bool enabled);
  [[nodiscard]] bool trailEnabled() const {
    return trail_ != nullptr;
  }
  // The owned trail, or nullptr when disabled. The dock reaches through this to
  // re-bind the TF buffer, because layout restore replays layers BEFORE the
  // <config_topic> that binds TF.
  [[nodiscard]] TrailLayer* trail() const {
    return trail_.get();
  }

#ifdef PJ_SCENE3D_TEST_HOOKS
  // Decode + expand at a chosen time without a view (the view normally drives
  // this from prepareForRender() at the tracker time).
  void renderAtForTest(int64_t time_ns) {
    updateAt(time_ns);
  }
  // The arms staged by the last updateAt (frame-local; TF is applied at render).
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
  // The one <poses_in_frame> validator behind xmlLoadState and validateXml. A
  // nested <trail> is structurally allowed; its content is the trail's business.
  [[nodiscard]] static std::optional<ParsedSettings> parseSettings(const QDomElement& element);
  // Decode the first sample at attach to learn the source frame before render.
  bool bootstrap();
  // Decode the sample valid at time_ns and expand it into instances_. No-ops when the same sample is already
  // staged with the current style. A bad sample fails closed: no geometry plus a
  // statusWarning(). Returns true when instances_ or the warning changed.
  bool updateAt(int64_t time_ns);
  // Drop staged geometry + the replay cursors. Called on attach/detach: a prior
  // attachment's UID/frame may belong to a timeline that no longer exists
  // (dataset replace), so it must never anchor or skip the new generation.
  void resetReplayState();
  // Drop instances_ (bumping geometry_revision_ when there were any).
  void clearGeometry();
  // Re-expand the current sample on the next prepare (style edit, reload).
  void markDirty();
  // Apply a pending tracker move or style edit at requested_time_ (once per
  // painted frame, by prepareForRender()). Returns updateAt's.
  bool drainPendingUpdate();
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);
  // Track the (possibly changing) source frame_id; notify the dock on change.
  void updateSourceFrame(const std::string& frame_id);
  // Bring a freshly-created trail up to this layer's current lifecycle state
  // (context, fixed frame, tracker time, visibility). Enabling can happen at any
  // point after attach, so the trail must catch up rather than assume it was
  // present from the start.
  void syncTrailToCurrentState();

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  QMetaObject::Connection reload_connection_;
  // Parsers are deliberately NOT cached: every decode resolves a fresh
  // ParserBinding through ctx_.session (resolveObject()), so a file reload that
  // re-registers the topic's parser slot can never leave a dangling pointer.

  std::string source_frame_;
  std::optional<PJ::Timepoint> render_stamp_;  // objectStamp() of the staged sample
  // Last fixed frame pushed by the dock: renderKey folds fixed<-source, and a
  // trail enabled later rebuilds against it (the gizmos re-resolve per paint).
  QString fixed_frame_;
  bool visible_ = true;
  // Set by setTrackerTime / a style edit; drained once per painted frame
  // (drainPendingUpdate) so a fast scrub or slider drag re-expands only the
  // final state.
  bool tracker_dirty_ = false;
  std::optional<PJ::Timepoint> requested_time_;
  // Arm count and sample that last overflowed the view budget (QRhi).
  BudgetLatch budget_;

  // Viewer style baked into the instance buffer; defaults match the TF "Frames"
  // gizmos. A change bumps style_revision_ so updateAt re-expands the current
  // sample instead of skipping it as unchanged.
  float gizmo_size_ = 0.15f;
  float gizmo_opacity_ = 1.0f;
  bool x_arrow_only_ = false;
  bool override_color_enabled_ = false;
  QColor override_color_{255, 0, 0};  // red, the panel-wide override/solid default
  int style_revision_ = 0;

  // Identity of the sample + style currently staged, so a scrub within one
  // message (or an unchanged style) skips the parse/expand/upload.
  PJ::SequentialUID staged_uid_{};
  int staged_revision_ = -1;

  std::vector<PoseTriadInstance> instances_;  // last expansion (frame-local; TF applied at draw)
  AABB source_bounds_;
  std::uint64_t geometry_revision_ = 0;

  // Owned outright (no QObject parent) so destruction is deterministic and the
  // dock never sees it as a layer. Null when the trail is disabled.
  std::unique_ptr<TrailLayer> trail_;
};

}  // namespace pj::scene3d
