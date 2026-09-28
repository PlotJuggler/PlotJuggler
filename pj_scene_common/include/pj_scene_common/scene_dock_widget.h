#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
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
#include "pj_base/time.hpp"  // PJ::Timepoint, fromRaw/toRaw
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/IDataWidget.h"
#include "pj_runtime/IObjectViewer.h"
#include "pj_scene_common/layer_factory.h"
#include "pj_scene_common/scene_layer.h"

namespace PJ {

class SessionManager;

/// Backend-agnostic dock base that owns an ordered stack of scene layers.
///
/// Subclasses provide the concrete scene view, accepted object types, layer
/// context, and the hook that maps the ordered layer list into the renderer.
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

  /// Retries scene-layer restores that were deferred because their dataset/topic
  /// was not available yet. An empty filter drains all pending entries.
  virtual int retryPendingRestores(const QSet<QString>& topic_names);
  [[nodiscard]] virtual QStringList unresolvedPendingRestores() const;
  /// Returns unresolved saved elements that block an exact restore. Interactive
  /// advertised-topic intents are durable workspace state and are excluded.
  [[nodiscard]] QStringList unresolvedBlockingPendingRestores() const;
  virtual void clearPendingRestores();
  void clearBlockingPendingRestores();
  /// Base<->family payload convention: attributes named `dataset_id` /
  /// `source_dataset_id` anywhere in a deferred payload are dataset
  /// references — discardPendingRestoresFor* honors exactly these.
  void discardPendingRestoresForDataset(DatasetId dataset_id);
  void discardPendingRestoresForTopic(DatasetId dataset_id, const QString& topic_name);

  struct PendingRestoreDemand {
    QString topic_name;
    std::optional<DatasetId> preferred_dataset;
  };
  [[nodiscard]] std::vector<PendingRestoreDemand> pendingRestoreDemands() const;
  /// Resolver-free emptiness probe — pendingRestoreDemands() runs the identity
  /// ladder per element, so use this for a plain yes/no question.
  [[nodiscard]] bool hasPendingRestoreDemands() const;

  /// Records a not-yet-materialized advertised object drop in the dock-owned
  /// deferred queue. The binder may hold demand for it but cannot complete it.
  bool deferTopicIntent(
      DatasetId dataset_id, const QString& topic_name, sdk::BuiltinObjectType object_type,
      const QString& display_name = {});

  /// Optional override of the object-identity policy (tests, custom hosts).
  /// The callback must reject ambiguity. Without it, resolution defaults to
  /// the session's ambiguity-safe ladder; without a session, restores pend.
  using ObjectDatasetResolver =
      std::function<std::optional<DatasetId>(DatasetId, const QString&, const QString&, const QString&)>;
  void setObjectDatasetResolver(ObjectDatasetResolver resolver);

  /// True when the current replay permanently rejected an element or payload.
  [[nodiscard]] bool workspaceRestoreFailed() const noexcept {
    return restore_failure_count_ != 0;
  }

  /// IObjectViewer: non-virtual template method. Runs pruneEvictedObjects() (the
  /// family-specific eviction sweep) and then applies the keep-if-never-populated
  /// rule centrally, so a never-populated dock survives and only an evicted-to-
  /// empty one resets. `final` so subclasses customize pruneEvictedObjects(), not
  /// this — the keep rule can't be forgotten by an override.
  bool revalidateObjects() final;

  /// Accepts a topic as either a render layer or a scene-wide config topic.
  bool addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);

  void removeTopic(ObjectTopicId topic_id);
  /// Un-hiding also re-delivers the last tracker time to the layer (hidden
  /// layers receive no tracker ticks), so it catches up to the playhead instead
  /// of repainting the geometry it held at the moment of hiding.
  void setLayerVisible(ObjectTopicId topic_id, bool visible);

  /// Applies a partial or complete draw order, appending omitted layers after it.
  void reorderLayers(const std::vector<ObjectTopicId>& ordered_topic_ids);

  /// Returns layer snapshots in draw order.
  [[nodiscard]] std::vector<SceneLayerInfo> layers() const;

  /// Returns a non-owning pointer to the layer for a topic, or nullptr.
  [[nodiscard]] ISceneLayer* layerFor(ObjectTopicId topic_id) const;

  /// Stores the SessionManager pointer and notifies the subclass via the
  /// virtual override. Subclasses that need to extend session wiring (e.g. to
  /// reconnect a live-samples slot) must override this and call the base first —
  /// the base stores the pointer and subclass wiring depends on it being set.
  virtual void setSessionManager(SessionManager* session);

 signals:
  void layerAdded(ObjectTopicId topic_id);
  void layerRemoved(ObjectTopicId topic_id);
  void layerVisibilityChanged(ObjectTopicId topic_id, bool visible);
  void layerWarningChanged(ObjectTopicId topic_id, bool warn, QString reason);
  /// Emitted whenever the dock-owned deferred queue changes.
  void pendingRestoresChanged();
  /// Emitted for committed, persistent user mutations, never during restore.
  void workspaceChanged();

 protected:
  enum class DeferredElementKind { kRenderLayer, kConfigTopic };
  /// Registry used by subclasses to register their supported layer types.
  [[nodiscard]] LayerFactory& layerFactory();
  [[nodiscard]] const LayerFactory& layerFactory() const;
  [[nodiscard]] SessionManager* sessionManager() const;

  /// XML root tag for this scene family. Defaults to "scene".
  [[nodiscard]] virtual QString xmlTag() const;

  /// Direct child tags accepted by this family before destructive replay.
  [[nodiscard]] virtual bool acceptsStateChildTag(const QString& tag) const;

  /// Creates the scene widget hosted by this dock; called lazily by the base.
  virtual QWidget* createSceneView() = 0;

  /// Creates the context passed to newly attached layers.
  virtual std::unique_ptr<SceneLayerContext> makeContext() = 0;

  /// Filters object topics before layer construction.
  [[nodiscard]] virtual bool acceptsObjectType(sdk::BuiltinObjectType object_type) const = 0;

  [[nodiscard]] virtual bool acceptsDeferredObjectType(sdk::BuiltinObjectType object_type) const {
    return acceptsObjectType(object_type);
  }

  [[nodiscard]] virtual DeferredElementKind deferredElementKind(sdk::BuiltinObjectType /*object_type*/) const {
    return DeferredElementKind::kRenderLayer;
  }

  /// Allows subclasses to consume scene-wide config topics without adding a layer.
  virtual bool handleSceneConfigTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);

  /// Reconciles the concrete view with the current draw order.
  ///
  /// Pointers are owned by this dock and remain valid until the next removal or
  /// clear operation.
  virtual void syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) = 0;

  /// Requests a view repaint after layer state changes.
  virtual void refreshView();

  /// Per-tick render fingerprint of scene content drawn by the VIEW itself rather
  /// than by a layer (e.g. a 3D family's TF axis triads + parent-connection lines,
  /// which pose the whole frame forest at the tracker time). Folded into
  /// trackerRenderKey() alongside the per-layer keys so the repaint gate also
  /// coalesces — and, crucially, does NOT freeze — that view-owned content. Default
  /// 0 (no view-level content); override to contribute. MUST be cheap and decode-free.
  [[nodiscard]] virtual uint64_t viewRenderKey(PJ::Timepoint /*time*/) const {
    return 0;
  }

  /// Forces the next onTrackerTime() to repaint regardless of the fingerprint. The
  /// base calls it on every structural change to the layer set (add/remove/visibility/
  /// clear). A subclass that repaints OUTSIDE the tracker gate (e.g. a live-edge
  /// streaming push that drives the layers directly) should also call it after that
  /// paint, so the gate's last-painted key never lags the pixels on screen.
  void invalidateTrackerRenderKey() {
    have_render_key_ = false;
  }

  /// Lets subclasses trigger a full view reconcile (re-push the ordered layer
  /// list through syncViewLayers) after a scene-wide state change — e.g. a
  /// fixed-frame change that the subclass's syncViewLayers override fans out to
  /// every layer. The subclass owns what reconciliation means; the base just
  /// exposes the private no-arg syncViewLayers() that rebuilds the order.
  void reconcileViewLayers() {
    syncViewLayers();
  }

  /// Last tracker time forwarded to the layers (ns, already clamped by
  /// clampToLayerRange), or nullopt when no tracker tick (or live nudge) has
  /// arrived yet — 0 is a valid timestamp, so absence is explicit. For derived
  /// docks that drive an additional consumer off the same clock (the 3D view's
  /// render time, the 2D composite seed).
  [[nodiscard]] std::optional<int64_t> lastTrackerNs() const {
    return last_tracker_.has_value() ? std::optional<int64_t>{PJ::toRaw(*last_tracker_)} : std::nullopt;
  }

  /// Records an externally-derived tracker time (e.g. a live-ingest nudge to
  /// the data edge) as the seed for future layer additions and rebuild seeding,
  /// without driving the layers (the caller already did). Takes a raw int64-ns
  /// (the spine edge) and lifts it to a Timepoint for internal storage.
  void noteTrackerTime(int64_t time_ns) {
    last_tracker_ = PJ::fromRaw(time_ns);
  }

  /// Removes every layer: re-points the concrete view off the old layers
  /// (syncViewLayers with an empty order) while they are still alive, then
  /// detaches and destroys them. Concrete docks whose scene view holds raw
  /// layer pointers (e.g. the 3D view's layer list) MUST call this from their
  /// own destructor, while virtual dispatch still reaches their overrides —
  /// the base destructor cannot reconcile the view for them.
  void clearLayers();

  /// Forces the lazily-created scene view to exist now (no-op once created).
  /// The base normally defers view creation to a queued singleShot; restore
  /// paths that apply view state synchronously (fixed frame, camera pose) must
  /// force the view first, or those writes are silently dropped while view_ is
  /// still null.
  void ensureSceneViewCreated();

  /// The source label of a dataset (file path / capture name), or empty when the
  /// id is unknown. Persisted alongside the raw DatasetId so a saved layout can
  /// re-resolve a layer across sessions where load order assigned a different id.
  [[nodiscard]] static QString datasetSourceName(const SessionManager* session, DatasetId dataset_id);

  /// Resolves through the host-injected ambiguity-safe object identity policy.
  [[nodiscard]] std::optional<DatasetId> resolveObjectDataset(
      DatasetId saved_id, const QString& saved_source, const QString& saved_path, const QString& topic_name) const;

  /// The dataset whose display-offset represents this dock's time domain when
  /// onTrackerTime() recovers the absolute tracker instant. The base scans render
  /// layers and returns the first non-zero dataset_id (else 0). Subclasses that own
  /// time-bearing topics with NO render layer (e.g. a TF-only 3D dock, whose
  /// FrameTransforms topic is config, not a layer) override to supply their dataset
  /// when the base scan returns 0 — without it the offset defaults to zero and
  /// display seconds are mistaken for absolute. Backend-agnostic: the base only
  /// forwards the id to session_->displayOffset().
  [[nodiscard]] virtual DatasetId representativeDatasetId() const;

  /// Family-specific eviction sweep for revalidateObjects(): drop layers (and, in
  /// Scene3D, config topics) whose ObjectStore topic was evicted, and return
  /// whether any live content remains. The non-virtual revalidateObjects() owns
  /// the keep-if-never-populated rule, so an override here only prunes — it must
  /// NOT re-implement the never-populated short-circuit.
  virtual bool pruneEvictedObjects();

  /// Inserts an already-constructed layer under `topic_id`, running the same
  /// attach/wire/register/notify pipeline addLayer() uses after factory
  /// creation. For family-owned layers that never flow through the
  /// LayerFactory (e.g. the 3D family's trails; synthetic robot models DO go
  /// through their registered factory creator). Skips the config-topic consult
  /// and the cross-dataset clock guard — such a layer has no store descriptor
  /// to check. Returns false (destroying the layer) when the id is taken or
  /// attach() fails.
  bool insertLayer(ObjectTopicId topic_id, std::unique_ptr<ISceneLayer> layer);

  /// Whether this dock has ever held content (a render layer or a scene-config
  /// topic), live or restored. Latched true on the first add, never cleared. Lets
  /// revalidateObjects() distinguish an intentionally-empty dock (click-created /
  /// restored empty — keep it) from one whose content was all evicted (reset it).
  [[nodiscard]] bool everHadContent() const noexcept {
    return ever_had_content_;
  }

 protected:
  struct PendingRestoreElement {
    QDomDocument document;
    QString topic_name;
  };

  struct PendingRestoreSnapshot {
    std::vector<PendingRestoreElement> elements;
    uint64_t restore_failure_count = 0;
    bool ever_had_content = false;
  };

  /// Per-element restore hook the pending-retry loop (retryPendingRestores) calls for
  /// each deferred element. The base restores a <layer>; a widget family overrides this
  /// to dispatch other element kinds (e.g. 3D's <config_topic>). Returns false to keep
  /// the element pending for a later retry.
  virtual bool restoreOnePending(const QDomElement& element);

  /// Stashes an XML element whose dataset/topic is not available yet, so a later
  /// retryPendingRestores re-attempts it. Protected so a derived family can defer its
  /// own elements into the one shared pending queue.
  void rememberPendingRestore(const QDomElement& element, QString topic_name = {});
  [[nodiscard]] PendingRestoreSnapshot capturePendingRestoreState() const;
  void restorePendingRestoreState(const PendingRestoreSnapshot& snapshot);
  void appendPendingRestoreElements(QDomDocument& doc, QDomElement& root) const;
  void markWorkspaceRestoreFailed();
  void resetWorkspaceRestoreStatus();
  void notifyWorkspaceChanged();

  [[nodiscard]] QScopedValueRollback<bool> beginWorkspaceRestore() {
    return QScopedValueRollback<bool>(restoring_state_, true);
  }

 private:
  /// Result of an add attempt, separating the two outcomes addTopic's bool used
  /// to conflate ("layer created" vs "consumed as a scene-config topic").
  enum class AddOutcome { kLayerAdded, kConsumedAsConfig, kRejected };

  /// Removes matching queue entries and emits pendingRestoresChanged once.
  void erasePendingRestoresIf(const std::function<bool(const PendingRestoreElement&)>& predicate);

  [[nodiscard]] PJ::Timepoint clampToLayerRange(PJ::Timepoint time) const;
  [[nodiscard]] std::vector<ISceneLayer*> orderedLayerPtrs() const;
  void syncViewLayers();
  /// Destructor-safe teardown: detaches and destroys layers WITHOUT touching
  /// the (pure virtual) view reconciliation. Only for ~SceneDockWidget.
  void destroyLayersUnsynced();
  void recordLayerVisibility(ObjectTopicId topic_id, bool visible);

  /// Orchestrates adding a topic as a render layer or scene-config topic.
  AddOutcome addLayer(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);
  /// Connects a layer's signals to the dock's reconcile/notify slots.
  void wireLayerSignals(ISceneLayer* layer, ObjectTopicId topic_id);
  /// Records a constructed layer in the draw order and seeds its tracker time.
  void registerLayer(int64_t key, std::unique_ptr<ISceneLayer> layer);
  /// Delivers the current playhead to a layer, clamped to that layer's own
  /// range (a spanning layer bounds both ends; a latched one-shot layer keeps
  /// its lone stamp). Used both when a layer is first registered and when it is
  /// un-hidden, since hidden layers receive no tracker ticks and would otherwise
  /// repaint the geometry they held at the moment of hiding. No-op when no
  /// tracker tick has arrived yet and the layer has no usable range.
  void seedLayerTrackerTime(ISceneLayer* layer);
  /// Returns false only when the saved dataset/topic is not available yet and
  /// the caller should keep this XML element pending for a later retry.
  [[nodiscard]] bool restoreLayerElement(const QDomElement& layer_el);

  /// Order-independent combined renderKey() of all visible layers at `time` — the
  /// per-tick repaint-coalescing fingerprint (see onTrackerTime). Order-independent
  /// (XOR of per-layer mixes) so an unordered_map rehash can't spuriously flip it.
  /// Folds in viewRenderKey() for non-layer view content.
  [[nodiscard]] uint64_t trackerRenderKey(PJ::Timepoint time) const;

  std::unordered_map<int64_t, std::unique_ptr<ISceneLayer>> layers_;
  std::vector<int64_t> draw_order_;
  std::vector<PendingRestoreElement> pending_restore_elements_;
  LayerFactory factory_;
  SessionManager* session_ = nullptr;
  ObjectDatasetResolver object_dataset_resolver_;
  std::optional<PJ::Timepoint> last_tracker_;
  QWidget* scene_view_ = nullptr;
  std::unordered_map<int64_t, bool> layer_visibility_cache_;
  // Latched true the first time any layer or config topic is added (see
  // everHadContent()); never cleared, so an evicted-to-empty dock stays
  // distinguishable from a never-populated one.
  bool ever_had_content_ = false;
  bool restoring_state_ = false;
  uint64_t restore_failure_count_ = 0;

  // Per-tick repaint coalescing (see onTrackerTime). last_render_key_ is the
  // trackerRenderKey() of the most recently painted tracker frame; have_render_key_
  // is false until the first paint and after any structural change, forcing a paint.
  uint64_t last_render_key_ = 0;
  bool have_render_key_ = false;
};

}  // namespace PJ
