#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

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
#include "pj_scene3d_widgets/layers/trail_layer.h"  // TrailSource: a slot argument, so moc needs it complete
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_scene_common/saved_layer_header.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_scene_common/scene_layer.h"
#include "pj_tf/tf_buffer.h"

class QLabel;
class QResizeEvent;
class QSettings;
class QWidget;

namespace pj::scene3d {
class FixedFrameMemory;
class Scene3DLayer;
class SceneViewWidget;
class UrdfPackageResolver;
class TrailLayer;
}  // namespace pj::scene3d

namespace PJ {

class ComboBox;
class SvgButton;
class TransformService;

// Dock content for 3D scene topics, on every platform. Layer lifetime, ordering, visibility, tracker-time dispatch and
// XML persistence live in SceneDockWidget; this subclass owns the 3D-only state: the view, the per-dataset TF buffer,
// the fixed-frame overlay, fallback frames and the per-row warning (frame orphan state combined with each layer's
// statusWarning()). Browser-only policy sits behind PJ_TARGET_WASM.
class Scene3DDockWidget : public SceneDockWidget {
  Q_OBJECT
 public:
  explicit Scene3DDockWidget(QWidget* parent = nullptr);
  ~Scene3DDockWidget() override;

  // Also (re)connects the live-samples slot that feeds streamed FrameTransform messages into the TF buffer
  // (reconnectLiveSamples). Must call the base.
  void setSessionManager(SessionManager* session) override;

  void setTransformService(PJ::TransformService* service);

  // Optional: without it the dock neither records nor seeds from the manual fixed-frame choice. Not owned; must outlive
  // the dock.
  void setFixedFrameMemory(pj::scene3d::FixedFrameMemory* memory) {
    fixed_frame_memory_ = memory;
  }

  // For the app's scene-controls panel (grid/gizmo/look setters live on SceneViewWidget). Null until the dock realizes
  // its view.
  [[nodiscard]] pj::scene3d::SceneViewWidget* sceneView() {
    return view_;
  }

  /// Single source of truth for the object types the 3D family handles: render layers (acceptsObjectType) plus
  /// scene-wide config topics (kFrameTransforms, handleSceneConfigTopic). Host-side drop routing reads this.
  [[nodiscard]] static bool handlesObjectType(sdk::BuiltinObjectType object_type);

  bool addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);

  /// Fits the first content to arrive once: the fixed frame becomes the layer's own frame and the camera frames
  /// its bounds, so content far from the default frame's origin is not hidden. For a view the user fills (a drop
  /// into an empty view, a plugin scene); cancelled by user camera input, a fixed-frame pick and a layout restore.
  void frameInitialContent();
  [[nodiscard]] bool initialContentFramingPending() const {
    return initial_content_framing_;
  }

  void setSettings(QSettings* settings);
  void setEmbeddedAssets(QMap<QString, QByteArray> assets);

  /// The dataset source path that keys the resolver's per-source remembered-roots map and seeds its auto search roots.
  /// Single-path approximation: the app feeds the most recently loaded source, so in an additive multi-file session a
  /// robot model resolves against the last-loaded file's directory, not necessarily its own dataset's.
  void setSourcePath(const QString& path);

  /// The user's package search folders (Preferences, "Locate..."): every dock searches them at workspace depth.
  static QStringList packageSearchFolders(const QSettings& settings);
  static void setPackageSearchFolders(QSettings& settings, const QStringList& folders);
  /// Re-reads the package search folders and reloads every robot model that has unresolved meshes.
  void reloadPackageSearchFolders();

  bool tryAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;
  [[nodiscard]] bool canAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type) const override;
  [[nodiscard]] std::vector<SceneLayerInfo> configTopics() const override;
  void onTrackerTime(double time) override;

  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;
  /// Parse-only: refuses what xmlLoadState refuses without looking at the data (a layer whose topic is not loaded is
  /// not a failure), plus a malformed payload of such a deferred layer, which xmlLoadState would only refuse once the
  /// topic arrives.
  [[nodiscard]] bool xmlValidateState(const QDomElement& element) const override;

  enum class FixedFrameMode { kAutoRoot, kExplicit };

  [[nodiscard]] QList<pj::tf::FrameRow> availableFrames() const {
    return available_frames_;
  }
  [[nodiscard]] QString currentFixedFrame() const;
  [[nodiscard]] bool isAutoRootMode() const {
    return fixed_frame_mode_ == FixedFrameMode::kAutoRoot;
  }
  // "" = not following; empty until the view exists.
  [[nodiscard]] QString currentFollowFrame() const;

  [[nodiscard]] bool layerVisible(ObjectTopicId topic_id) const;

  // Cached per-row warning state: `is_orphan` is set for an unresolved or disconnected source frame OR a layer-reported
  // statusWarning() (e.g. a model that failed to fetch or import); `reason` is the tooltip text.
  struct OrphanSnapshot {
    bool is_orphan = false;
    QString reason;
  };
  [[nodiscard]] OrphanSnapshot orphanState(ObjectTopicId topic_id) const;

  // Store topics whose metadata builtin_object_type is kRobotDescription (the scene-controls panel's URDF combo).
  struct RobotDescriptionTopic {
    ObjectTopicId topic_id;
    QString name;
  };
  [[nodiscard]] QList<RobotDescriptionTopic> robotDescriptionTopics() const;

#ifdef PJ_SCENE3D_TEST_HOOKS
  // The dataset the dock's TF buffer is bound to (0 = unbound), and whether a TF buffer is bound at all: lets tests
  // assert the rebind/reset logic without spinning the view up.
  [[nodiscard]] DatasetId boundDatasetIdForTest() const {
    return dataset_id_;
  }
  [[nodiscard]] bool hasTransformBufferForTest() const {
    return tf_buffer_ != nullptr && !tf_buffer_is_local_;
  }
  // The last tracker instant forwarded to the layers/view (raw ns), already converted display->absolute and clamped:
  // lets tests assert the "Use time offset" shift is recovered for a layer-less (TF-only) dock without a view.
  [[nodiscard]] std::optional<int64_t> lastTrackerNsForTest() const {
    return lastTrackerNs();
  }
#endif

#ifdef PJ_TARGET_WASM
  // Browser route for a local URDF: the sandbox grants the file's bytes, never a
  // path. The bytes live only in this layer and are not saved in the layout, so
  // a restored layer asks the user to select the file again.
  ObjectTopicId addRobotModelLayerFromContent(QString filename, QByteArray bytes);
#endif

 public slots:
  // A local File/URL robot layer under a synthetic registry id (never passed to the ObjectStore). With a path the layer
  // loads that URDF at once; empty = blank layer, configure later. Returns the id ({0} on failure) for a later
  // removeTopic().
  ObjectTopicId addRobotModelLayer(const QString& urdf_path = {});
  ObjectTopicId addRobotModelLayerFromUrl(const QString& url);
  // Adds a trail layer tracking a TF frame or a pose topic's first pose, under a synthetic local id (a trail is not a
  // wire-object topic, see TrailLayer). Persisted with role="trail". Returns {0} on failure.
  ObjectTopicId addTrailLayer(const pj::scene3d::TrailSource& source);
  void setFixedFrame(const QString& frame);
  void setFixedFrameAutoRoot();
  // "" = off. Forwards to the view, emits followFrameChanged and is persisted per dock in the layout XML.
  void setFollowFrame(const QString& frame);
  // Recenters the camera on the follow target; no-op when not following or without a view.
  void recenterOnFollowFrame();

 signals:
  void availableFramesChanged(const QList<pj::tf::FrameRow>& frames);
  void currentFixedFrameChanged(const QString& frame);
  void fixedFrameModeChanged(bool is_auto_root);
  void followFrameChanged(const QString& frame);
  /// Emitted once per view creation, at the end of createSceneView(); sceneView() is non-null from here. Lets the host
  /// apply view-only state to a dock whose lazily-created view did not exist when it was wired.
  void sceneViewReady();

 protected:
  QWidget* createSceneView() override;
  std::unique_ptr<SceneLayerContext> makeContext() override;
  [[nodiscard]] bool acceptsObjectType(sdk::BuiltinObjectType object_type) const override;
  [[nodiscard]] bool acceptsDeferredObjectType(sdk::BuiltinObjectType object_type) const override;
  [[nodiscard]] DeferredElementKind deferredElementKind(sdk::BuiltinObjectType object_type) const override;
  bool handleSceneConfigTopic(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;
  bool removeSceneConfigTopic(ObjectTopicId topic_id) override;
  // Also prunes evicted FrameTransforms config topics (no render layer) and resets the TF binding when its dataset is
  // gone; the base adds the keep-if-never-populated rule, so this only reports whether live layers/config remain.
  bool pruneEvictedObjects() override;
  // A TF-only dock consumes FrameTransforms as a config topic and has no render layer, so the base layers_ scan returns
  // 0: fall back to dataset_id_ (the bound TF dataset) so onTrackerTime recovers the "Use time offset" shift. 0 only
  // when fully unbound.
  [[nodiscard]] DatasetId representativeDatasetId() const override;
  void syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) override;
  void refreshView() override;
  // Folds the view's TF-overlay fingerprint into the repaint gate: a moving TF tree (triads, parent-connection lines),
  // which no layer owns, must not freeze while every visible layer's renderKey is unchanged.
  [[nodiscard]] uint64_t viewRenderKey(PJ::Timepoint time) const override;
  [[nodiscard]] QString xmlTag() const override;
  void resizeEvent(QResizeEvent* event) override;
  // User camera input on the view cancels a pending frameInitialContent().
  bool eventFilter(QObject* watched, QEvent* event) override;

 private slots:
  void onAvailableFrames(const QList<pj::tf::FrameRow>& frames);
  // TransformService::datasetTransformsReady handler. A dataset's TF buffer can be filled AFTER this dock bound (and
  // read) it empty (the cloud toolbox ingests /tf post-download). The fill is in place (shared_ptr identity kept) but
  // the view caches its frame list and orphan states, so re-poll both when the ready dataset is the one shown.
  void onDatasetTransformsReady(DatasetId dataset_id);

 private:
  // kConsumed: a valid element the dock deliberately dropped (a foreign dataset's
  // topic, already reported through topicRefused); the restore carries on.
  enum class ElementRestoreResult { kRestored, kConsumed, kDeferred, kInvalid };
  // Shared body of addTopic(). `enforce_image_gate` runs the kImage depth-encoding gate (firstSampleIsDepthEncoded):
  // true for interactive adds, false for layout restore, which trusts the saved layer type because the topic's first
  // sample may not be loaded yet (gating on an absent sample would silently drop a saved DepthCloud layer).
  bool addTopicImpl(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title, bool enforce_image_gate);

  [[nodiscard]] ElementRestoreResult restoreLayerElement(
      const QDomElement& layer_el, QString* deferred_topic_name = nullptr);
  // role="trail" branch of restoreLayerElement: hosts an unconfigured TrailLayer and delegates the restore (source
  // resolution included) to TrailLayer::xmlLoadStateResult, honoring its tri-state result.
  [[nodiscard]] ElementRestoreResult restoreTrailElement(
      const QDomElement& layer_el, const SavedLayerHeader& header, QString* deferred_topic_name);
  ObjectTopicId addTrailLayerImpl(ObjectTopicId topic_id, std::unique_ptr<pj::scene3d::TrailLayer> layer);
  // Saved-topic resolution shared by restoreLayerElement and restoreConfigTopicElement: binds `ref` (parsed from `el`
  // with SavedTopicRef::kRequired) and yields kRestored (live topic id), kDeferred (with the topic name for the
  // pending queue), kConsumed (the element's dataset lost the restore's dataset choice) or kInvalid.
  [[nodiscard]] ElementRestoreResult resolveSavedTopicRef(
      const QDomElement& el, const SavedTopicHeader& ref, QString* deferred_topic_name,
      sdk::BuiltinObjectType expected_type, std::optional<ObjectTopicId>& out_topic);
  [[nodiscard]] ElementRestoreResult restoreConfigTopicElement(const QDomElement& config_el);
  void applyRestoredLayerOrder();
  // Per-element restore hook of the base's pending-retry loop: <config_topic> -> restoreConfigTopicElement, else a
  // <layer> -> restoreLayerElement.
  bool restoreOnePending(const QDomElement& element) override;
  void prepareTransformBufferForTopic(ObjectTopicId topic_id);
  // Fans the current TF binding out to every TrailLayer and RobotModelLayer, which capture the buffer at attach and
  // would otherwise never see a later bind/unbind (restore binds TF AFTER the layers replay).
  void pushTransformBufferToLayers();
  // Gives a dock whose only content is local robot layers (no dataset) a private TF buffer: the robots inject their
  // fixed-joint bridges into it and the view poses links through it. A dataset binding replaces it.
  void ensureLocalTransformBuffer();
  // After a tracked topic is dropped, resets the TF binding (tf_buffer_/dataset_id_ to null/0, empty buffer to the
  // view) when no remaining tracked topic belongs to the bound dataset, so the dock can rebind to a second dataset's TF
  // tree.
  void resetTransformBindingIfDatasetGone();
  // Accumulates streamed FrameTransform messages into the dataset's TF buffer live (file load uses the bulk ingest).
  // Re-callable: drops any prior connection first.
  void reconnectLiveSamples(SessionManager* session);
  // Advances every visible object layer to the newest timestamp in the ObjectStore. samplesIngested carries only scalar
  // TopicIds, so like the 2D dock this ignores that list and queries the store; without it the object layers stay
  // frozen on their last decoded frame while only the TF buffer advances.
  void driveVisibleLayersToLiveEdge();
  // Pushes a resolved tracker time (raw ns) to every visible layer and the view, then refreshes bounds and repaints.
  // Shared tail of the live-edge drive and onTrackerTime, each owning its own clamp/edge policy.
  void pushTrackerTimeToView(int64_t time_ns);
  void wireScene3DLayer(pj::scene3d::Scene3DLayer* layer);
  void absorbFallbackFrames(pj::scene3d::Scene3DLayer* layer);
  void applyResolvedFixedFrame(const QString& frame);
  void applyRestoredFixedFrame(FixedFrameMode mode, const QString& frame);
  void resetDatasetBindingForRestore();
  // The fixed frame to adopt in auto mode: the dataset's remembered manual choice (FixedFrameMemory::remembered) when
  // still present in `frames`, else the map/world/odom heuristic (pickFixedFrame). Read-only w.r.t. the memory: seeding
  // never records, so there is no feedback loop with the setFixedFrame() recording path.
  [[nodiscard]] QString resolveAutoFixedFrame(const QList<pj::tf::FrameRow>& frames) const;
  void refreshFrameOverlayCombo();
  void onOverlayFramePicked(int index);
  void layoutFrameOverlayCombo();
  // Transforms each layer's source-frame worldBounds() into the current fixed frame, unions them and pushes the result
  // to the camera (adaptive near/far, framing). Cheap; called on layer changes and every refreshView(). Hidden layers
  // count; the camera is never fitted to it.
  void updateSceneBounds();
  // Re-evaluates each layer's frame-resolvability and emits layerWarningChanged on flips. force=false (the hot
  // live-ingest path only) returns early when neither the TF buffer revision nor the fixed frame changed since the last
  // full run; every other call site forces a full recompute.
  void recomputeOrphanStates(bool force = true);
  void updateLayerWarning(
      ObjectTopicId topic_id, const pj::scene3d::Scene3DLayer& layer,
      const std::unordered_set<std::string>& known_frames);
  [[nodiscard]] std::unordered_set<std::string> knownFrames() const;
  [[nodiscard]] bool isLocalLayerId(ObjectTopicId topic_id) const;
  ObjectTopicId allocateLocalLayerId();

  bool initial_content_framing_ = false;  // see frameInitialContent()
  PJ::TransformService* transform_service_ = nullptr;
  pj::scene3d::FixedFrameMemory* fixed_frame_memory_ = nullptr;
  pj::scene3d::SceneViewWidget* view_ = nullptr;
  std::shared_ptr<pj::tf::TransformBuffer> tf_buffer_;
  // tf_buffer_ is ensureLocalTransformBuffer()'s private buffer, not a dataset's.
  bool tf_buffer_is_local_ = false;
  // Dataset that owns tf_buffer_, cached at first bind so the live-samples slot drives incremental TF ingest.
  DatasetId dataset_id_ = 0;
  // Store topics this dock tracks, mapped to their owning dataset: render-layer topics (prepareTransformBufferForTopic)
  // AND config topics (handleSceneConfigTopic). Detects when the last topic of the TF-bound dataset is gone so the dock
  // can rebind tf_buffer_/dataset_id_ to a surviving dataset. Local robot layers (synthetic ids) are never recorded.
  std::unordered_map<uint32_t, DatasetId> scene_topic_datasets_;
  // The FrameTransforms config-topic subset of scene_topic_datasets_ keys (no render layer): a live TF-only dock (zero
  // layers) survives catalog churn and advances to the live edge solely because of this set.
  std::unordered_set<uint32_t> config_topics_;
  QMetaObject::Connection live_samples_conn_;
  // A progressive file load folds TF into the buffer incrementally (FileLoader emits this signal on each flush):
  // re-render at last_tracker_display_ so the scene fills as the file loads. Streaming uses ingestNewTransforms (no
  // signal) and the live-samples slot, so this fires on the file path only.
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

  QList<pj::tf::FrameRow> available_frames_;
  std::vector<std::string> fallback_frames_;
  FixedFrameMode fixed_frame_mode_ = FixedFrameMode::kAutoRoot;
  ComboBox* frame_overlay_combo_ = nullptr;
  // Top-right overlay controls, children of `this` rather than the view (z-order): the camera-model selector and a Home
  // (reset-to-default-view) button, left of the orientation gizmo.
  ComboBox* camera_model_combo_ = nullptr;
  SvgButton* home_button_ = nullptr;
  QLabel* rendering_warning_label_ = nullptr;

  /// Per-topic orphan/warning state (keyed by topicKey): recomputeOrphanStates() emits layerWarningChanged only when a
  /// layer's frame-resolvability flips.
  std::unordered_map<int64_t, OrphanSnapshot> orphan_states_;

  /// Change detection for recomputeOrphanStates(force=false): the TF buffer revision() and fixed frame at the last full
  /// recompute; the hot live-ingest path skips the orphan walk while both are unchanged.
  uint64_t last_orphan_revision_ = 0;
  QString last_orphan_fixed_frame_;
};

}  // namespace PJ
