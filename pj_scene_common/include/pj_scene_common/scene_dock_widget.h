#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QMetaObject>
#include <QScopedValueRollback>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/time.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/IDataWidget.h"
#include "pj_runtime/IObjectViewer.h"
#include "pj_scene_common/layer_factory.h"
#include "pj_scene_common/scene_layer.h"

namespace PJ {

class SessionManager;
}  // namespace PJ

class QLabel;

namespace PJ {

/// Backend-agnostic dock base that owns an ordered stack of scene layers. Subclasses provide the concrete scene
/// view, accepted object types, layer context, and the hook that maps the ordered layer list into the renderer.
/// Design: docs/ARCHITECTURE.md.
class SceneDockWidget : public QWidget, public IDataWidget, public IObjectViewer {
  Q_OBJECT
 public:
  explicit SceneDockWidget(QWidget* parent = nullptr);
  ~SceneDockWidget() override;

  QWidget* widget() override;
  void onTrackerTime(double time) override;
  bool tryAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  /// Retries restores deferred because their dataset or topic was not available yet. An empty filter retries all
  /// pending entries.
  virtual int retryPendingRestores(const QSet<QString>& topic_names);
  [[nodiscard]] virtual QStringList unresolvedPendingRestores() const;
  /// Unresolved saved elements that block an exact restore; advertised-topic intents are durable workspace state
  /// and are excluded.
  [[nodiscard]] QStringList unresolvedBlockingPendingRestores() const;
  [[nodiscard]] QStringList unresolvedRestores() const override {
    return unresolvedBlockingPendingRestores();
  }
  virtual void clearPendingRestores();
  void clearBlockingPendingRestores();

  /// Ends a restore: settles the dataset choice of a layout that mixed datasets, then makes the last
  /// retryPendingRestores. The dock shows one dataset; the winner is the dataset it already shows, else the first
  /// saved dataset that is loaded. The pending elements of every other saved dataset are dropped with a visible
  /// reason (topicRefused) without counting as a failed restore; the winner's own stay pending until their topics
  /// arrive. With nothing loaded there is no winner and the first dataset to resolve afterwards wins. The shell
  /// calls it after a whole layout load, DockWidget after a paste or family switch.
  void finishRestore() override;
  /// Attributes named `dataset_id` / `source_dataset_id` anywhere in a deferred payload are dataset references;
  /// these discards match exactly those.
  void discardPendingRestoresForDataset(DatasetId dataset_id);
  void discardPendingRestoresForTopic(DatasetId dataset_id, const QString& topic_name);

  struct PendingRestoreDemand {
    QString topic_name;
    std::optional<DatasetId> preferred_dataset;
  };
  [[nodiscard]] std::vector<PendingRestoreDemand> pendingRestoreDemands() const;
  /// Emptiness probe without the per-element identity resolution pendingRestoreDemands() runs.
  [[nodiscard]] bool hasPendingRestoreDemands() const;

  /// Queues an advertised object drop whose data has not arrived. The topic-demand binder may hold demand for it
  /// but cannot complete it.
  bool deferTopicIntent(
      DatasetId dataset_id, const QString& topic_name, sdk::BuiltinObjectType object_type,
      const QString& display_name = {});

  /// Overrides the object-identity policy (tests, custom hosts); it must reject ambiguity. Without it the session's
  /// ambiguity-safe ladder resolves; without a session, restores stay pending.
  using ObjectDatasetResolver =
      std::function<std::optional<DatasetId>(DatasetId, const QString&, const QString&, const QString&)>;
  void setObjectDatasetResolver(ObjectDatasetResolver resolver);

  /// Runs pruneEvictedObjects(), then keeps a dock that never held content (or has pending restores): only a dock
  /// whose content was all evicted resets. `final` so an override cannot drop that rule; customize
  /// pruneEvictedObjects().
  bool revalidateObjects() final;

  /// Accepts a topic as a render layer or a scene-wide config topic (handleSceneConfigTopic).
  bool addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);

  /// Removes `topic_id`'s layer, or the config topic it is (removeSceneConfigTopic).
  /// False when the dock holds neither.
  bool removeTopic(ObjectTopicId topic_id);
  /// removeTopic at a caller's request rather than by eviction: a dock it leaves empty
  /// counts as never populated, so revalidateObjects keeps it instead of resetting it.
  bool detachTopic(ObjectTopicId topic_id);
  /// Un-hiding re-delivers the last tracker time to the layer (hidden layers get no ticks), so it catches up to
  /// the playhead instead of showing the geometry it held when hidden.
  void setLayerVisible(ObjectTopicId topic_id, bool visible);

  /// Applies a partial or complete draw order, appending omitted layers after it.
  void reorderLayers(const std::vector<ObjectTopicId>& ordered_topic_ids);

  /// Returns layer snapshots in draw order.
  [[nodiscard]] std::vector<SceneLayerInfo> layers() const;

  /// The scene-wide config topics the dock consumes without a layer (a 3D view's FrameTransforms); with layers(),
  /// everything it shows.
  [[nodiscard]] virtual std::vector<SceneLayerInfo> configTopics() const {
    return {};
  }

  /// The reason of the latest foreign-dataset refusal, cleared by the read. Unlike topicRefused it is never
  /// coalesced, so a caller adding topics one at a time reads each refusal's own reason.
  [[nodiscard]] QString takeRefusalReason() override {
    return std::exchange(last_refusal_reason_, {});
  }

  /// Returns a non-owning pointer to the layer for a topic, or nullptr.
  [[nodiscard]] ISceneLayer* layerFor(ObjectTopicId topic_id) const;

  /// Stores the session pointer and wires the derived-topic signals. An override calls the base first: its own
  /// wiring reads the stored pointer.
  virtual void setSessionManager(SessionManager* session);

  /// The row status of a layer on a derived topic (DerivedTopicPresentation::status); empty for any other layer.
  [[nodiscard]] QString layerStatus(ObjectTopicId topic_id) const;
  /// The dock's status strip: "computing (behind 0.4 s)" with the largest lag of its derived layers while one of
  /// them computes, "computing…" while none has a lag yet; empty otherwise.
  [[nodiscard]] QString derivedStatus() const;

 signals:
  void layerAdded(ObjectTopicId topic_id);
  void layerRemoved(ObjectTopicId topic_id);
  /// What configTopics() returns changed, during a restore too. A family that keeps config topics emits it.
  void configTopicsChanged();
  void layerVisibilityChanged(ObjectTopicId topic_id, bool visible);
  /// Emitted after reorderLayers() changed the draw order; read layers() for it.
  void layersReordered();
  void layerWarningChanged(ObjectTopicId topic_id, bool warn, QString reason);
  /// layerStatus(topic_id) changed.
  void layerStatusChanged(ObjectTopicId topic_id, QString status);
  /// Emitted whenever the dock-owned deferred queue changes.
  void pendingRestoresChanged();
  /// Emitted for committed, persistent user mutations, never during restore.
  void workspaceChanged();
  /// A topic the user (or a saved layout) asked for was refused, with the reason in user-facing words. Every
  /// refusal except a plain "unsupported type" reports here, so the shell can surface it. Refusals within one
  /// event-loop turn emit once.
  void topicRefused(const QString& reason);
  /// A restore dropped one saved element while the rest of the dock restored (a value a payload reader refuses, a
  /// layer whose topic holds another object type); `line` names the element and why. The shell reports it as a
  /// layout warning. A skip also sets takeRefusalReason(), so the MCP widget_state tool refuses a `set` that would
  /// drop an element.
  void restoreElementSkipped(const QString& line);

 protected:
  enum class DeferredElementKind { kRenderLayer, kConfigTopic };
  /// Where subclasses register their layer types.
  [[nodiscard]] LayerFactory& layerFactory();
  [[nodiscard]] const LayerFactory& layerFactory() const;
  [[nodiscard]] SessionManager* sessionManager() const;

  /// XML root tag of this scene family; "scene" by default.
  [[nodiscard]] virtual QString xmlTag() const;

  /// Direct child tags this family accepts; xmlLoadState rejects an unknown one before it clears anything.
  [[nodiscard]] virtual bool acceptsStateChildTag(const QString& tag) const;

  /// Creates the scene widget the dock hosts; called lazily (see ensureSceneViewCreated).
  virtual QWidget* createSceneView() = 0;

  /// The context passed to newly attached layers.
  virtual std::unique_ptr<SceneLayerContext> makeContext() = 0;

  /// The object types this family builds layers for.
  [[nodiscard]] virtual bool acceptsObjectType(sdk::BuiltinObjectType object_type) const = 0;

  [[nodiscard]] virtual bool acceptsDeferredObjectType(sdk::BuiltinObjectType object_type) const {
    return acceptsObjectType(object_type);
  }

  [[nodiscard]] virtual DeferredElementKind deferredElementKind(sdk::BuiltinObjectType /*object_type*/) const {
    return DeferredElementKind::kRenderLayer;
  }

  /// Lets a family consume a topic as scene-wide config without a layer; true when it did.
  virtual bool handleSceneConfigTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);
  /// Drops a config topic handleSceneConfigTopic took; false when `topic_id` is not one.
  virtual bool removeSceneConfigTopic(ObjectTopicId /*topic_id*/) {
    return false;
  }

  /// Reconciles the concrete view with the draw order. The pointers are owned by the dock and valid until the next
  /// removal or clear.
  virtual void syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) = 0;

  /// Invalidates the view's content after layer state changes (a view that retains its last frame must re-render).
  /// A family drives its per-tick view work (render-time push, scene bounds) from here, not from onTrackerTime, so
  /// it is gated together with the repaint.
  virtual void refreshView();

  /// Render fingerprint of content the view draws itself, outside any layer (the 3D view's TF triads and
  /// connection lines, which pose the whole frame forest at the tracker time). Folded into the repaint gate with
  /// the layer keys: without it that content freezes whenever every layer key is unchanged. 0 (none) by default;
  /// must be cheap and decode-free.
  [[nodiscard]] virtual uint64_t viewRenderKey(PJ::Timepoint /*time*/) const {
    return 0;
  }

  /// Forces the next onTrackerTime() to repaint whatever the fingerprint. The base calls it on every change to the
  /// layer set (add, remove, visibility, clear). A subclass that paints outside the tracker gate (a live-edge push
  /// that drives the layers directly) calls it after that paint, or the gate's last-painted key lags the pixels on
  /// screen.
  void invalidateTrackerRenderKey() {
    have_render_key_ = false;
  }

  /// Re-pushes the ordered layer list through syncViewLayers after a scene-wide state change (a fixed-frame change
  /// the subclass's override fans out to every layer).
  void reconcileViewLayers() {
    syncViewLayers();
  }

  /// Last tracker time forwarded to the layers (absolute ns, already clamped to the layers' ranges), nullopt until
  /// a tick or noteTrackerTime arrives (0 is a valid timestamp). For docks that drive another consumer off the same
  /// clock (the 3D view's render time, the 2D composite seed).
  [[nodiscard]] std::optional<int64_t> lastTrackerNs() const {
    return last_tracker_.has_value() ? std::optional<int64_t>{PJ::toRaw(*last_tracker_)} : std::nullopt;
  }

  /// Records a tracker time the caller derived itself (a live-ingest nudge to the data edge) as the seed for later
  /// layer additions, without driving the layers (the caller already did). Absolute ns.
  void noteTrackerTime(int64_t time_ns) {
    last_tracker_ = PJ::fromRaw(time_ns);
  }

  /// The range the dock clamps the tracker to and folds into the live edge for `layer`: a derived topic's inputs'
  /// range (SessionManager::derivedTopicInputRange; inverted for a finding, which bounds nothing), else
  /// `layer.timeRange()`.
  [[nodiscard]] PJ::Range<PJ::Timepoint> clockRangeOf(const ISceneLayer& layer) const;
  /// The newest raw clock instant among the visible layers' clockRangeOf, or nullopt when none has data.
  [[nodiscard]] std::optional<int64_t> visibleLayersLiveEdge() const;

  /// Removes every layer: re-points the view off them (syncViewLayers with an empty order) while they are alive,
  /// so it releases its GPU state for each, then detaches and destroys them. A concrete dock whose view holds raw
  /// layer pointers (the 3D view's layer list) calls this from its own destructor: the base destructor cannot
  /// reconcile the view (pure virtual call), and the view would dereference freed layers.
  void clearLayers();

  /// Creates the scene view now (no-op once created). The base defers creation to a queued single-shot; restore
  /// paths that apply view state synchronously (fixed frame, camera pose) call this first, or the writes are dropped
  /// while the view does not exist.
  void ensureSceneViewCreated();

  /// The source label of a dataset (file path or capture name), empty for an unknown id. Persisted beside the raw
  /// DatasetId so a saved layout re-resolves across sessions where load order assigned a different id.
  [[nodiscard]] static QString datasetSourceName(const SessionManager* session, DatasetId dataset_id);

  /// Resolves a saved dataset identity through the injected or session policy; nullopt while it is not loaded or
  /// is ambiguous.
  [[nodiscard]] std::optional<DatasetId> resolveObjectDataset(
      DatasetId saved_id, const QString& saved_source, const QString& saved_path, const QString& topic_name) const;

  /// Records the distinct datasets a saved scene state names, in document order, so datasetForSavedElement can
  /// make the first one win. Family xmlLoadState overrides call it once per load, right after clearing the pending
  /// queue. Only <layer>/<config_topic> children with a dataset identity (a non-zero id, a source or a path) and a
  /// topic name count.
  void recordSavedDatasets(const QDomElement& state);

  /// True when a saved element names its dataset (by id, source or path).
  [[nodiscard]] static bool hasDatasetIdentity(const QDomElement& element);

  /// How a saved element binds to a live dataset during a restore.
  struct SavedElementDataset {
    /// The live dataset, or empty while the element must stay pending.
    std::optional<DatasetId> dataset;
    /// The element belongs to a dataset other than the restore's choice: it is consumed
    /// (dropped, reported through topicRefused) instead of restored or failed.
    bool refused = false;
  };

  /// resolveObjectDataset for a saved <layer>/<config_topic> restore. In a layout naming several datasets, no
  /// element of a later dataset binds before the first saved dataset has resolved; once it has, elements of any
  /// other are refused. Runtime pending intents and single-dataset layouts are not gated. The element's dataset_id
  /// must be well-formed (callers reject it before binding).
  [[nodiscard]] SavedElementDataset datasetForSavedElement(const QDomElement& element);

  /// The dataset whose display offset onTrackerTime() adds back to recover the absolute tracker instant, and the
  /// dock's one dataset for refuseForeignTopic. The base returns the first non-zero dataset of the render layers,
  /// else 0. A dock whose time-bearing topics are not layers (a TF-only 3D dock) overrides it; with 0 the offset
  /// is taken as zero and display seconds are read as absolute.
  [[nodiscard]] virtual DatasetId representativeDatasetId() const;

  /// Drops layers (a family also its config topics) whose ObjectStore topic was evicted and returns whether any
  /// live content remains. revalidateObjects() owns the never-populated rule: do not repeat it here.
  virtual bool pruneEvictedObjects();

  /// Refuses `topic_id` when its dataset differs from the one the dock shows: reports through topicRefused and
  /// returns true. A dock has one display clock, so arrival order must not silently decide whose wins. An unknown
  /// topic or an empty dock never refuses. Restore paths use it too, dropping the foreign element instead of
  /// failing the restore.
  bool refuseForeignTopic(ObjectTopicId topic_id);

  /// Inserts an already-built layer under `topic_id` through the attach, wire, register and notify steps addLayer()
  /// runs after factory creation. For family-owned layers not backed by a store topic (the 3D family's trails),
  /// under a family-allocated synthetic id. Skips the config-topic consult and the foreign-dataset check (no store
  /// descriptor to check). False, destroying the layer, when the id is taken or attach() fails. Synthetic robot models
  /// do not use this: they go through addTopic() and their registered factory creator.
  bool insertLayer(ObjectTopicId topic_id, std::unique_ptr<ISceneLayer> layer);

  /// Whether the dock has held content (a render layer or a config topic): latched on the first add, cleared only
  /// when detachTopic empties it. Distinguishes an intentionally empty dock (keep) from one whose content was all
  /// evicted (reset).
  [[nodiscard]] bool everHadContent() const noexcept {
    return ever_had_content_;
  }

 protected:
  struct PendingRestoreElement {
    QDomDocument document;
    QString topic_name;
  };

  /// One dataset a saved scene state names.
  struct SavedDataset {
    DatasetId id = 0;
    QString source;
    QString path;
    /// A topic the state names in this dataset; disambiguates a same-file remint when the
    /// dataset is resolved without a specific element at hand. Not part of the identity.
    QString topic_name;

    /// Same dataset: id, source and path agree.
    [[nodiscard]] bool operator==(const SavedDataset& other) const {
      return id == other.id && source == other.source && path == other.path;
    }
    /// How the saved state called it, for messages: source, else path, else "#id".
    [[nodiscard]] QString label() const;
  };

  /// Restores one deferred element for retryPendingRestores. The base restores a <layer>; a family overrides it for
  /// other kinds (3D's <config_topic>). False keeps the element pending.
  virtual bool restoreOnePending(const QDomElement& element);

  /// Queues an element whose dataset or topic is not available yet for a later retryPendingRestores; a family
  /// defers its own elements into the same queue. Duplicates (same tag, topic and dataset identity) are ignored.
  void rememberPendingRestore(const QDomElement& element, QString topic_name = {});
  void appendPendingRestoreElements(QDomDocument& doc, QDomElement& root) const;
  /// Drops a saved element that cannot be restored and emits restoreElementSkipped with `reason` (the first
  /// rejection a reporting reader gave, when there is one); the dock's other elements still restore.
  void skipRestoreElement(const QDomElement& element, const QString& reason);
  [[nodiscard]] static QString duplicateTopicReason();
  void notifyWorkspaceChanged();

  [[nodiscard]] QScopedValueRollback<bool> beginWorkspaceRestore() {
    return QScopedValueRollback<bool>(restoring_state_, true);
  }

 private:
  /// Result of an add attempt: a layer created, a topic consumed as scene-wide config, or neither.
  enum class AddOutcome { kLayerAdded, kConsumedAsConfig, kRejected };

  /// Refuses a topic of `incoming_dataset` when `bound_dataset` is already shown. A zero on either side means "no
  /// dataset yet" and never refuses.
  bool refuseForeignDataset(DatasetId incoming_dataset, DatasetId bound_dataset);

  /// Logs and emits topicRefused (coalesced per event-loop turn) for a topic of `incoming_label` that the dock
  /// showing `bound_label` cannot take.
  void reportForeignDataset(const QString& bound_label, const QString& incoming_label);

  /// Source label of a live dataset, "#id" when unnamed.
  [[nodiscard]] QString datasetLabel(DatasetId dataset_id) const;

  /// The dataset a saved element names; nullopt when its dataset_id is malformed. An absent one reads as 0 ("by
  /// source or path only").
  [[nodiscard]] static std::optional<SavedDataset> savedDatasetOf(const QDomElement& element);

  /// The dataset choice of finishRestore: drops the pending elements of the losing saved datasets; the winner's
  /// own stay pending, whether or not their topic has arrived.
  void settleRestoreDataset();

  /// A derived result landed: moves the visible layers of `topics` to the dock's time again (refreshAt), so a
  /// result at the dock's time shows without a tracker tick. Each layer requests its own repaint.
  void refreshDerivedLayers(const std::vector<ObjectTopicId>& topics);
  /// Re-reads the derived presentation of `topic_id`'s layer (nothing for a raw one): its warning part, its row
  /// status (layerStatusChanged on a change) and the dock's status strip. Never per frame.
  void syncDerivedPresentation(ObjectTopicId topic_id);
  /// syncDerivedPresentation of every derived layer that shows something: its text depends on the display offset.
  void resyncDerivedPresentations();
  void updateDerivedStatus();

  /// Removes matching queue entries and emits pendingRestoresChanged once.
  void erasePendingRestoresIf(const std::function<bool(const PendingRestoreElement&)>& predicate);

  [[nodiscard]] PJ::Timepoint clampToLayerRange(PJ::Timepoint time) const;
  [[nodiscard]] std::vector<ISceneLayer*> orderedLayerPtrs() const;
  void syncViewLayers();
  /// Detaches and destroys layers without the pure-virtual view reconciliation; for ~SceneDockWidget only.
  void destroyLayersUnsynced();
  void recordLayerVisibility(ObjectTopicId topic_id, bool visible);

  /// Adds a topic as a render layer or a scene-config topic.
  AddOutcome addLayer(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);

  void wireLayerSignals(ISceneLayer* layer, ObjectTopicId topic_id);
  /// Adds the layer to the draw order and seeds its tracker time.

  void registerLayer(int64_t key, std::unique_ptr<ISceneLayer> layer);
  /// Delivers the current playhead to a layer, clamped to that layer's own range, when it is registered and when it
  /// is un-hidden. Without a tracker tick yet the layer gets its first frame; a layer with no range is left alone.
  void seedLayerTrackerTime(ISceneLayer* layer);
  /// False only when the saved dataset or topic is not available yet, or an unqualified element is ambiguous
  /// (uniqueObjectTopic): the caller keeps the element pending.
  [[nodiscard]] bool restoreLayerElement(const QDomElement& layer_el);

  /// The combined renderKey() of the visible layers at `time`, plus viewRenderKey(): the repaint-coalescing
  /// fingerprint. Order-independent (XOR of mixed per-layer keys), so an unordered_map rehash cannot flip it.
  [[nodiscard]] uint64_t trackerRenderKey(PJ::Timepoint time) const;

  std::unordered_map<int64_t, std::unique_ptr<ISceneLayer>> layers_;
  std::vector<int64_t> draw_order_;
  std::vector<PendingRestoreElement> pending_restore_elements_;
  LayerFactory factory_;
  SessionManager* session_ = nullptr;
  ObjectDatasetResolver object_dataset_resolver_;
  // Datasets named by the layout being restored, in document order; emptied by settleRestoreDataset.
  std::vector<SavedDataset> saved_datasets_;
  std::optional<PJ::Timepoint> last_tracker_;
  QWidget* scene_view_ = nullptr;
  /// The presentation of each derived layer that shows something beside its geometry, by topicKey.
  std::unordered_map<int64_t, DerivedTopicPresentation> derived_presentations_;
  QLabel* derived_status_label_ = nullptr;  ///< the status strip under the view, hidden while empty
  QMetaObject::Connection derived_published_conn_;
  QMetaObject::Connection derived_state_conn_;
  QMetaObject::Connection derived_offset_conn_;
  QMetaObject::Connection derived_dataset_offset_conn_;
  std::unordered_map<int64_t, bool> layer_visibility_cache_;
  bool ever_had_content_ = false;
  bool restoring_state_ = false;
  // Set by the first refusal of an event-loop turn, cleared by a zero-delay timer.
  bool refusal_reported_ = false;
  QString last_refusal_reason_;
  uint64_t skipped_restores_ = 0;  // only compared before/after one pending retry

  // Repaint gate: the key of the last painted tracker frame; have_render_key_ is false until the first paint and
  // after any layer-set change.
  uint64_t last_render_key_ = 0;
  bool have_render_key_ = false;
};

}  // namespace PJ
