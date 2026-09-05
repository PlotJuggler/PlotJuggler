#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#ifdef PJ_TARGET_WASM

#include "pj_scene3d_widgets/Scene3DDockWidget_wasm.h"

#else

#include <QByteArray>
#include <QDomDocument>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_scene_common/scene_layer.h"

class QResizeEvent;
class QSettings;
class QToolButton;
class QWidget;

namespace pj::scene3d {
class SceneViewWidget;
class TransformService;
class UrdfPackageResolver;
struct TrailSource;
class TrailLayer;
}  // namespace pj::scene3d

namespace PJ {

class ComboBox;

// Dock content for 3D scene topics. Generic layer lifetime, ordering,
// visibility, tracker-time dispatch, and XML layer persistence live in
// SceneDockWidget; this subclass owns 3D-only state: the OpenGL view, the
// per-dataset TF buffer, the fixed-frame overlay, fallback frames, and orphan
// detection.
class Scene3DDockWidget : public SceneDockWidget {
  Q_OBJECT
 public:
  explicit Scene3DDockWidget(QWidget* parent = nullptr);
  ~Scene3DDockWidget() override;

  // Also (re)connects the live-samples slot that feeds streamed FrameTransform
  // messages into the TF buffer (see reconnectLiveSamples). Must call the base.
  void setSessionManager(SessionManager* session) override;

  void setTransformService(pj::scene3d::TransformService* service);

  // The owned GL view, for the app's scene-controls panel (grid/gizmo/look
  // setters live on SceneViewWidget). Null until the dock realizes its view.
  [[nodiscard]] pj::scene3d::SceneViewWidget* sceneView() {
    return view_;
  }

  /// Single source of truth for the canonical object types the 3D scene family
  /// handles: render layers (kPointCloud, kCompressedPointCloud, kOccupancyGrid,
  /// kRobotDescription, kSceneEntities — see acceptsObjectType) plus scene-wide config topics
  /// (kFrameTransforms via handleSceneConfigTopic). Host-side drop routing reads this.
  [[nodiscard]] static bool handlesObjectType(sdk::BuiltinObjectType object_type);

  bool addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);

  void setSettings(QSettings* settings);
  void setEmbeddedAssets(QMap<QString, QByteArray> assets);

  /// The dataset source path that keys the resolver's per-source remembered-roots
  /// map and seeds its auto search roots (step 1 of the package:// chain).
  /// Single-path approximation: the app feeds the most recently loaded source,
  /// so in an additive multi-file session a robot model resolves against the
  /// last-loaded file's directory, not necessarily its own dataset's file.
  void setSourcePath(const QString& path);

  bool tryAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;
  void onTrackerTime(double time) override;

  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  enum class FixedFrameMode { kAutoRoot, kExplicit };

  [[nodiscard]] QList<pj::scene3d::FrameRow> availableFrames() const {
    return available_frames_;
  }
  [[nodiscard]] QString currentFixedFrame() const;
  [[nodiscard]] bool isAutoRootMode() const {
    return fixed_frame_mode_ == FixedFrameMode::kAutoRoot;
  }
  // The frame the camera currently follows ("" = not following). Reads the view;
  // empty until the view exists. See setFollowFrame / SceneViewWidget::setFollowFrame.
  [[nodiscard]] QString currentFollowFrame() const;

  /// True when the topic has a live layer that is currently visible.
  [[nodiscard]] bool layerVisible(ObjectTopicId topic_id) const;

  // Cached per-row warning state. `is_orphan` is true when the row should show a
  // warning — either an unresolved/disconnected source frame OR a layer-reported
  // statusWarning() (e.g. a model that failed to fetch/import); `reason` is the
  // tooltip text. recomputeOrphanStates() OR-combines the two sources.
  struct OrphanSnapshot {
    bool is_orphan = false;
    QString reason;
  };
  [[nodiscard]] OrphanSnapshot orphanState(ObjectTopicId topic_id) const;

  // Store topics carrying a robot description (metadata builtin_object_type ==
  // kRobotDescription), for the scene-controls panel's Model/URDF topic combo.
  struct RobotDescriptionTopic {
    ObjectTopicId topic_id;
    QString name;
  };
  [[nodiscard]] QList<RobotDescriptionTopic> robotDescriptionTopics() const;

#ifdef PJ_SCENE3D_TEST_HOOKS
  // The dataset the dock's TF buffer is currently bound to (0 = unbound), and
  // whether a TF buffer is bound at all. Lets tests assert the M.17 rebind/reset
  // logic without spinning the GL view up.
  [[nodiscard]] DatasetId boundDatasetIdForTest() const {
    return dataset_id_;
  }
  [[nodiscard]] bool hasTransformBufferForTest() const {
    return tf_buffer_ != nullptr;
  }
  // The last tracker instant forwarded to the layers/view (raw ns), already
  // converted display->absolute and clamped. Lets tests assert the "Use time
  // offset" shift is recovered for a layer-less (TF-only) dock without an
  // OpenGL view; reaches the protected base accessor.
  [[nodiscard]] std::optional<int64_t> lastTrackerNsForTest() const {
    return lastTrackerNs();
  }
#endif

 public slots:
  // Create a local File/URL robot layer (synthetic registry id, never passed to
  // the ObjectStore). With a path, the layer loads that URDF immediately — the
  // panel's one-click "Load URDF…" flow; empty = blank layer, configure later.
  // Returns the layer's registry id ({0} on failure) so the caller can later
  // removeTopic() it — the panel's per-row remove flow.
  ObjectTopicId addRobotModelLayer(const QString& urdf_path = {});
  // Same local-layer flow, loading the URDF over http(s) instead of from disk.
  ObjectTopicId addRobotModelLayerFromUrl(const QString& url);
  // Adds a trail layer tracking a TF frame or a pose topic's first pose,
  // registered under a synthetic local id (a trail is not a wire-object topic —
  // see TrailLayer). Persisted with role="trail". Returns {0} on failure.
  ObjectTopicId addTrailLayer(const pj::scene3d::TrailSource& source);
  void setFixedFrame(const QString& frame);
  void setFixedFrameAutoRoot();
  // Set the camera follow target ("" = off). Forwards to the view and emits
  // followFrameChanged; persisted per-dock in the layout XML (next to fixed_frame).
  void setFollowFrame(const QString& frame);
  // Recenter the camera on the current follow target (the "Follow frame" recenter
  // button). No-op when not following or the view does not exist.
  void recenterOnFollowFrame();

 signals:
  void availableFramesChanged(const QList<pj::scene3d::FrameRow>& frames);
  void currentFixedFrameChanged(const QString& frame);
  void fixedFrameModeChanged(bool is_auto_root);
  void followFrameChanged(const QString& frame);
  /// Emitted once per view creation, at the end of createSceneView(); sceneView()
  /// is non-null from here. Lets the host apply view-only state (scene controls)
  /// to a dock whose lazily-created view did not yet exist when it was wired.
  void sceneViewReady();

 protected:
  QWidget* createSceneView() override;
  std::unique_ptr<SceneLayerContext> makeContext() override;
  [[nodiscard]] bool acceptsObjectType(sdk::BuiltinObjectType object_type) const override;
  [[nodiscard]] bool acceptsDeferredObjectType(sdk::BuiltinObjectType object_type) const override;
  [[nodiscard]] DeferredElementKind deferredElementKind(sdk::BuiltinObjectType object_type) const override;
  bool handleSceneConfigTopic(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;
  // Also prunes evicted FrameTransforms config topics (no render layer) and resets
  // the TF binding when its dataset is gone; the base adds the keep-if-never-
  // populated rule, so this only reports whether live layers/config remain.
  bool pruneEvictedObjects() override;
  // A TF-only dock consumes FrameTransforms as a config topic and creates no render
  // layer, so the base layers_ scan returns 0; fall back to dataset_id_ (the bound
  // TF dataset, kept in sync by prepareTransformBufferForTopic /
  // resetTransformBindingIfDatasetGone) so onTrackerTime recovers the "Use time
  // offset" shift. Returns 0 only when fully unbound.
  [[nodiscard]] DatasetId representativeDatasetId() const override;
  void syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) override;
  void refreshView() override;
  // Folds the view's TF-overlay fingerprint into the repaint gate so a moving TF
  // tree (axis triads / parent-connection lines), which no layer owns, is not
  // frozen when every visible layer's own renderKey is unchanged.
  [[nodiscard]] uint64_t viewRenderKey(PJ::Timepoint time) const override;
  [[nodiscard]] QString xmlTag() const override;
  void resizeEvent(QResizeEvent* event) override;

 private slots:
  void onAvailableFrames(const QList<pj::scene3d::FrameRow>& frames);
  // TransformService::datasetTransformsReady handler. A dataset's TF buffer can
  // be filled AFTER this dock already bound (and read) it empty — the cloud
  // toolbox ingests /tf post-download, unlike the file-open path which ingests
  // before any view binds. The fill is in-place (shared_ptr identity kept) but
  // the view caches its frame list + orphan states, so a late fill is invisible
  // until re-read. Re-poll the frame hierarchy + orphan states when the ready
  // dataset is the one this dock is showing.
  void onDatasetTransformsReady(DatasetId dataset_id);

 private:
  enum class ElementRestoreResult { kRestored, kConsumed, kDeferred, kInvalid };
  // Shared body of addTopic(). enforce_image_gate runs the kImage depth-encoding
  // gate (firstSampleIsDepthEncoded) — true for interactive adds (drop / family
  // switch), false for layout restore, which trusts the saved layer type because
  // the topic's first sample may not be loaded yet (gating on an absent sample
  // would silently drop a saved DepthCloud layer).
  bool addTopicImpl(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title, bool enforce_image_gate);

  [[nodiscard]] ElementRestoreResult restoreLayerElement(
      const QDomElement& layer_el, QString* deferred_topic_name = nullptr);
  // role="trail" branch of restoreLayerElement: rebuilds a TrailLayer from the
  // <trail> payload by hosting an unconfigured TrailLayer and delegating the
  // restore (source resolution included) to TrailLayer::xmlLoadStateResult,
  // honoring its tri-state result — the RobotModelLayer pattern.
  [[nodiscard]] ElementRestoreResult restoreTrailElement(const QDomElement& layer_el, QString* deferred_topic_name);
  // Shared wire/insert/cleanup tail of the two trail creation paths
  // (interactive addTrailLayer, restore's unconfigured layer).
  ObjectTopicId addTrailLayerImpl(ObjectTopicId topic_id, std::unique_ptr<pj::scene3d::TrailLayer> layer);
  // Saved-topic resolution ladder shared by restoreLayerElement and
  // restoreTrailElement: reads topic_name/dataset_* attributes off `el` and
  // yields the live topic id (kRestored), a deferral (kDeferred, with the
  // topic name for the pending queue), or kInvalid.
  [[nodiscard]] ElementRestoreResult resolveSavedTopicRef(
      const QDomElement& el, QString* deferred_topic_name, sdk::BuiltinObjectType expected_type,
      std::optional<ObjectTopicId>& out_topic) const;
  [[nodiscard]] ElementRestoreResult restoreConfigTopicElement(const QDomElement& config_el);
  void applyRestoredLayerOrder();
  // Per-element restore hook the base SceneDockWidget's shared pending-retry loop calls:
  // dispatches on the tag (<config_topic> -> restoreConfigTopicElement, else a render
  // <layer> -> restoreLayerElement).
  bool restoreOnePending(const QDomElement& element) override;
  void prepareTransformBufferForTopic(ObjectTopicId topic_id);
  // Fans the current TF binding out to every TrailLayer (which capture the
  // buffer at attach and would otherwise never see a later bind/unbind — the
  // restore path binds TF AFTER the layers replay).
  void pushTransformBufferToTrailLayers();
  // After a tracked topic is dropped, reset the TF binding (tf_buffer_/dataset_id_
  // → null/0, push the empty buffer to the view) when no remaining tracked topic
  // belongs to the bound dataset. This is what lets the dock rebind to a second
  // dataset's TF tree after the first dataset's topics are gone (M.17).
  void resetTransformBindingIfDatasetGone();
  // Connect to SessionManager::samplesIngested so streamed FrameTransform
  // messages are accumulated into the dataset's TF buffer live (file load uses
  // the bulk ingest instead). Re-callable: drops any prior connection first.
  void reconnectLiveSamples(SessionManager* session);
  // Advance every visible object layer (pointcloud / markers / occupancy) to the
  // newest timestamp now present in the ObjectStore. Streaming's samplesIngested
  // carries only scalar TopicIds, so — like the 2D dock — we ignore that list and
  // query the store directly; without this the object layers stay frozen on their
  // last decoded frame while only the TF buffer advances.
  void driveVisibleLayersToLiveEdge();
  // Push a resolved tracker time (raw int64 ns) to every visible layer and to the
  // view, then refresh bounds + repaint. Shared tail of the two tracker-time entry
  // points (live-edge drive and onTrackerTime), each of which owns its own
  // clamp/edge policy before calling this.
  void pushTrackerTimeToView(int64_t time_ns);
  void wireScene3DLayer(pj::scene3d::Scene3DLayer* layer);
  void absorbFallbackFrames(pj::scene3d::Scene3DLayer* layer);
  void applyResolvedFixedFrame(const QString& frame);
  void applyRestoredFixedFrame(FixedFrameMode mode, const QString& frame);
  void resetDatasetBindingForRestore();
  // The fixed frame to adopt in auto mode: the dataset's remembered manual choice
  // (TransformService::rememberedFixedFrame) when it is still present in `frames`,
  // else the map/world/odom heuristic (pickFixedFrame). This is what makes a
  // freshly-created dock default to the last frame the user picked by hand for the
  // same TransformBuffer. Read-only w.r.t. the memory — seeding never records, so
  // there is no feedback loop with the setFixedFrame() recording path.
  [[nodiscard]] QString resolveAutoFixedFrame(const QList<pj::scene3d::FrameRow>& frames) const;
  void refreshFrameOverlayCombo();
  void onOverlayFramePicked(int index);
  void layoutFrameOverlayCombo();
  // Transform each layer's source-frame worldBounds() into the current fixed frame,
  // union them, and push the result to the camera (adaptive near/far + framing).
  // Cheap; called on layer changes and tracker-time changes.
  void updateSceneBounds();
  // Re-evaluate each layer's frame-resolvability and emit layerWarningChanged on
  // flips. force=false (the hot live-ingest path only) early-returns when neither
  // the TF buffer revision nor the fixed frame changed since the last full run, so
  // the per-tick orphan walk is skipped when nothing TF-related moved. Every other
  // call site forces a full recompute.
  void recomputeOrphanStates(bool force = true);
  [[nodiscard]] bool isLocalLayerId(ObjectTopicId topic_id) const;
  ObjectTopicId allocateLocalLayerId();

  pj::scene3d::TransformService* transform_service_ = nullptr;
  pj::scene3d::SceneViewWidget* view_ = nullptr;
  std::shared_ptr<pj::scene3d::TransformBuffer> tf_buffer_;
  // Dataset that owns tf_buffer_, cached when the buffer is first bound so the
  // live-samples slot can drive incremental TF ingest without re-deriving it.
  DatasetId dataset_id_ = 0;
  // Store topics this dock currently tracks, mapped to their owning dataset.
  // Covers BOTH render-layer topics (recorded in prepareTransformBufferForTopic)
  // AND config topics (FrameTransforms consumed by handleSceneConfigTopic). Used
  // to detect when the last topic of the TF-bound dataset is gone so the dock can
  // rebind tf_buffer_/dataset_id_ to a surviving dataset (M.17). Local robot
  // layers (synthetic ids, no store dataset) are never recorded here.
  std::unordered_map<uint32_t, DatasetId> scene_topic_datasets_;
  // Subset of scene_topic_datasets_ keys that are FrameTransforms config topics
  // (no render layer). A live TF-only dock (zero layers) survives catalog churn
  // and advances to the live edge solely because of this set (H.3 / H.4).
  std::unordered_set<uint32_t> config_topics_;
  // Live streamed-TF ingest hookup (SessionManager::samplesIngested).
  QMetaObject::Connection live_samples_conn_;
  // TransformService::datasetTransformsReady hookup. A progressive file load folds
  // TF into the buffer incrementally (FileLoader drives the bulk ingest on each
  // flush, emitting that signal); we re-render at last_tracker_display_ so the scene
  // fills as the file loads. Streaming uses ingestNewTransforms (no signal) + the
  // live-samples slot, so this fires on the file path only.
  QMetaObject::Connection tf_ready_conn_;
  // Last finite display-axis time pushed via onTrackerTime, replayed when the TF
  // buffer changes mid-load to re-resolve layers against the new transforms.
  double last_tracker_display_ = 0.0;
  std::unique_ptr<pj::scene3d::UrdfPackageResolver> package_resolver_;
  QSettings* settings_ = nullptr;
  QMap<QString, QByteArray> embedded_assets_;
  QString source_path_;
  uint32_t next_local_layer_topic_id_ = std::numeric_limits<uint32_t>::max();
  std::unordered_set<uint32_t> local_layer_ids_;
  std::unordered_map<int64_t, int> restored_layer_orders_;
  bool xml_rollback_in_progress_ = false;

  QList<pj::scene3d::FrameRow> available_frames_;
  std::vector<std::string> fallback_frames_;
  FixedFrameMode fixed_frame_mode_ = FixedFrameMode::kAutoRoot;
  ComboBox* frame_overlay_combo_ = nullptr;
  // Top-right overlay controls (children of `this`, not the QOpenGLWidget — z-order):
  // the camera-model selector and a Home (reset-to-default-view) button, anchored
  // to the left of the orientation gizmo.
  ComboBox* camera_model_combo_ = nullptr;
  QToolButton* home_button_ = nullptr;

  /// Cached per-topic orphan/warning state (keyed by topicKey), so
  /// recomputeOrphanStates() only emits layerWarningChanged when a layer's
  /// frame-resolvability actually flips.
  std::unordered_map<int64_t, OrphanSnapshot> orphan_states_;

  /// Change-detection cache for recomputeOrphanStates(force=false): the TF buffer
  /// revision() and fixed frame at the last full recompute. The hot live-ingest
  /// path skips the orphan walk while both are unchanged.
  uint64_t last_orphan_revision_ = 0;
  QString last_orphan_fixed_frame_;
};

}  // namespace PJ

#endif  // PJ_TARGET_WASM
