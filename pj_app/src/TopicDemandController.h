// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QObject>
#include <QString>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/Time.h"

class QTimer;

namespace PJ {

class DataEngine;
class CustomWidgetDockWidget;
class DataProcessorService;
class PendingDisplayBinder;
class PlotWidget;
class SceneDockWidget;
class StateTransitionsDockWidget;
class TopicDemandTracker;

// Thin glue between displayed widgets and TopicDemandTracker: resolves a
// widget's currently-displayed series/layers to (DatasetId, topic_name) and
// forwards add/removeReference calls. Owns no widgets and no business logic
// beyond that resolution — see TopicDemandTracker for the ref-counting and
// emission contract this class is a producer of.
//
// MainWindow wires each new PlotWidget / SceneDockWidget through
// registerPlot()/registerSceneDock() at creation time (see makeSceneDock /
// onPlotAdded); every reference this controller adds for a widget is released
// automatically when that widget is destroyed.
//
// Threading: GUI-thread only (same as TopicDemandTracker and CatalogModel).
class TopicDemandController : public QObject {
  Q_OBJECT
 public:
  /// Where a plot's view start goes: SessionManager::setSeriesVisibleStart in the app.
  using VisibleStartSink = std::function<void(DatasetId dataset_id, DisplaySeconds start)>;

  TopicDemandController(
      CatalogModel& catalog, TopicDemandTracker& tracker, DataProcessorService& processors,
      PendingDisplayBinder& pending_binder, DataEngine& engine, VisibleStartSink visible_start = {},
      QObject* parent = nullptr);
  ~TopicDemandController() override;

  TopicDemandController(const TopicDemandController&) = delete;
  TopicDemandController& operator=(const TopicDemandController&) = delete;

  // Starts tracking `plot`'s displayed curves. Diffs the flattened multiset of
  // (dataset, topic) pairs its curves resolve to (XY via PointSeriesXY's own
  // CurveDescriptor sources; a displayed Data Processor output additionally
  // references its resolved SOURCE topics — see
  // DataProcessorService::sourceTopicsForOutput) against the last-seen set on
  // every curveListChanged(), and add/removeReference's exactly the delta.
  // Call once per plot; safe to call on an already-populated plot (the initial
  // diff against an empty "last seen" set references everything already shown,
  // covering the layout-restore case where curves exist before this is wired).
  // The plot's view start is also each of its datasets' on-demand series visible
  // start (the VisibleStartSink), set before the demand on every
  // curve change, so a series it starts computes the visible range first, and on
  // every move of its time axis (PlotWidget::visibleXRangeChanged: a gesture, a
  // linked or programmatic zoom), so the last plot to move wins its datasets.
  void registerPlot(PlotWidget* plot);

  // Starts tracking `dock`'s scene layers via layerAdded/layerRemoved, resolved
  // to (dataset, topic) by a linear CatalogModel scan (no id index exists).
  // Call once per dock, before any layer can be added (i.e. before
  // xmlLoadState()/addTopic()) so no layerAdded is missed.
  void registerSceneDock(SceneDockWidget* dock);

  // Starts tracking a state-transitions strip's displayed discrete series: diffs
  // its (dataset, topic) set on every seriesListChanged, mirroring registerPlot
  // (the initial sync covers rows restored before this call). Idempotent.
  void registerStateTransitionsDock(StateTransitionsDockWidget* dock);

  // Same for a Custom Widget panel: holds one reference per distinct (dataset, topic) its resolved
  // wires read, diffed on every bindingsChanged() and released when the panel is destroyed.
  void registerCustomWidgetDock(CustomWidgetDockWidget* dock);

  // M3-UI placeholder drop path: `plot` (a curve-less or already-populated
  // PlotWidget) had an advertised SCALAR placeholder dropped on it. Stages a
  // pending bind via PendingDisplayBinder, which holds the demand reference
  // until the real topic materializes and the curve completes.
  void handlePlaceholderPlotDrop(PlotWidget* plot, DatasetId dataset_id, const QString& topic_name);

  // M3-UI placeholder drop path: an advertised OBJECT placeholder was dropped
  // onto an already-mounted `dock`. The dock stages the authoritative intent;
  // PendingDisplayBinder holds only its demand reference.
  void handleSceneDockPlaceholderDrop(
      SceneDockWidget* dock, DatasetId dataset_id, const QString& topic_name, sdk::BuiltinObjectType object_type);

  // Starts a bounded, self-releasing "preview" subscription for `topic_name`: a
  // demand reference is held just long enough for one real sample to land (which
  // promotes the placeholder to real per-field rows via CatalogModel's
  // storage-wins merge), then released — on promotion or a ~4 s timeout,
  // whichever comes first. Shared by both preview triggers (the automatic census
  // and the manual double-click peek). No-op when the dataset is not
  // per-topic-pause-capable, when the topic already has a real (non-placeholder)
  // entry, when a preview for it is already in flight/queued, or when it is
  // already in the tracker's active set (a real display/pend reference must not
  // be disturbed). Deliberately does NOT consult the census "already previewed"
  // memory, so a manual peek can always retry a topic the census skipped.
  void requestFieldPreview(DatasetId dataset_id, const QString& topic_name);

  // Test seam: shortens the preview timeout so a timeout-release test need not
  // wait the production ~4 s. Applies to previews started after the call.
  void setPreviewTimeoutMsForTest(int timeout_ms);

 private:
  // Rebuilds `plot`'s current topic multiset and diffs it against
  // plot_topics_[plot], add/removeReference-ing exactly the delta.
  void syncPlot(PlotWidget* plot);
  // Tells the session where `plot`'s time view starts, for each dataset among `topics`.
  void reportVisibleStart(PlotWidget* plot, const std::vector<std::pair<DatasetId, QString>>& topics);
  // Same delta sync for a state-transitions strip, against state_dock_topics_.
  void syncStateTransitionsDock(StateTransitionsDockWidget* dock);
  // Same delta sync for a Custom Widget panel, against custom_dock_topics_.
  void syncCustomWidgetDock(CustomWidgetDockWidget* dock);
  // The shared tail of both syncs: multiset-diff `current` against `previous`
  // (a reference into the per-widget map), add/removeReference the delta, and
  // store `current` as the new previous.
  void applyTopicDelta(
      std::vector<std::pair<DatasetId, QString>> current, std::vector<std::pair<DatasetId, QString>>& previous);
  // Appends (dataset_id, topic_name) plus, when topic_id names a displayed Data
  // Processor output, its resolved source topics too (v1 derived-input
  // resolution — see DataProcessorService::sourceTopicsForOutput).
  void appendTopicRefs(
      std::vector<std::pair<DatasetId, QString>>& out, DatasetId dataset_id, const QString& topic_name,
      TopicId topic_id) const;
  // In-place catalog scan: ObjectTopicId -> (dataset, topic_name), or nullopt if
  // no live object-topic entry matches (evicted between the signal and this
  // lookup — treated as "nothing to reference").
  [[nodiscard]] std::optional<std::pair<DatasetId, QString>> resolveObjectTopic(ObjectTopicId topic_id) const;

  // itemsAdded handler for the preview machinery: releases previews whose topic
  // just promoted (path (a)), then auto-previews any newly-advertised scalar
  // placeholders (census).
  void onCatalogItemsAdded(const std::vector<CatalogItem>& items);
  // Census pass: previews every advertised, kNone-classified placeholder in
  // `items` on a per-topic-pause-capable dataset that has not been previewed
  // this session — object-classified topics (pointclouds/images) are excluded by
  // classification, which acts as the size filter.
  void runCensus(const std::vector<CatalogItem>& items);
  // Preview release path (a): for each in-flight preview whose topic appears in
  // `items` as a real (non-placeholder) entry, release its reference now.
  void releaseCompletedPreviews(const std::vector<CatalogItem>& items);
  // Records the in-flight preview, adds its demand reference, and arms the
  // single-shot timeout timer. Caller has already validated eligibility.
  void startPreview(DatasetId dataset_id, const QString& topic_name);
  // Releases an in-flight preview (reference + timer), marks it previewed for
  // the session, and starts the next queued preview. Idempotent for an unknown
  // (dataset, topic).
  void finishPreview(DatasetId dataset_id, const QString& topic_name);
  // Starts queued previews until the concurrency cap is reached, re-validating
  // each (state may have changed while it waited).
  void pumpPreviewQueue();
  // Releases every held preview reference and forgets all preview/census state.
  void onCatalogCleared();
  [[nodiscard]] bool previewInFlight(DatasetId dataset_id, const QString& topic_name) const;
  // True once a real (non-advertised) catalog entry for the topic exists, so a
  // preview would be pointless (its fields are already materialized).
  [[nodiscard]] bool topicHasRealEntry(DatasetId dataset_id, const QString& topic_name) const;

  CatalogModel& catalog_;
  TopicDemandTracker& tracker_;
  DataProcessorService& processors_;
  PendingDisplayBinder& pending_binder_;
  // Storage engine, used ONLY to disown preview-originated samples
  // (evictTopicHistory) — all normal demand flow goes through the tracker.
  DataEngine& engine_;
  VisibleStartSink visible_start_;

  // Last-seen flattened (dataset, topic) multiset per tracked plot — see
  // syncPlot(). Erased when the plot is destroyed.
  std::unordered_map<PlotWidget*, std::vector<std::pair<DatasetId, QString>>> plot_topics_;
  // Per-dock live scene-layer references this controller added, keyed by the
  // layer's ObjectTopicId, so layerRemoved releases exactly what layerAdded
  // added regardless of catalog churn in between. Erased when the dock is
  // destroyed (each entry's reference released first).
  std::unordered_map<SceneDockWidget*, std::unordered_map<uint32_t, std::pair<DatasetId, QString>>> scene_layer_topics_;
  // Last-seen (dataset, topic) set per tracked state-transitions strip — see
  // syncStateTransitionsDock(). Erased when the strip is destroyed.
  std::unordered_map<StateTransitionsDockWidget*, std::vector<std::pair<DatasetId, QString>>> state_dock_topics_;
  // Last-seen (dataset, topic) set per tracked Custom Widget panel — see syncCustomWidgetDock().
  std::unordered_map<CustomWidgetDockWidget*, std::vector<std::pair<DatasetId, QString>>> custom_dock_topics_;

  // --- bounded field-preview subscriptions (census + double-click peek) ---
  // Preview identity is (DatasetId, topic_name), matching TopicDemandTracker.
  using PreviewId = std::pair<DatasetId, QString>;

  // Applies the preview-history disown (see class doc): when a topic in
  // preview_promoted_pending_disown_ gains a REAL reference, its stored
  // fake-interest samples are hidden via DataEngine::evictTopicHistory and the
  // entry is consumed. [main-thread]
  void onActiveTopicsChanged(DatasetId dataset_id, const std::vector<QString>& active_topics);
  // Raises the retention floor past every stored sample of the topic's scalar
  // TopicId(s), resolved through the catalog. The discovered columns stay
  // registered, so the field rows survive.
  void disownPreviewHistory(DatasetId dataset_id, const QString& topic_name);
  // At most this many previews hold a demand reference at once; the census
  // staggers the rest through preview_queue_.
  static constexpr int kMaxConcurrentPreviews = 8;
  // In-flight previews → their owned single-shot timeout timer.
  std::map<PreviewId, QTimer*> active_previews_;
  // Previews waiting for a concurrency slot (FIFO). Bounded by one advertise
  // burst, so membership checks scan it linearly.
  std::deque<PreviewId> preview_queue_;
  // Topics the census has previewed this session (marked on preview completion),
  // so a re-advertise does not re-preview a quiet/timed-out topic. Cleared when
  // the catalog empties. The manual peek bypasses this set.
  std::set<PreviewId> censused_topics_;
  // Preview-promoted topics whose fake-interest samples are still stored and
  // must be disowned the moment a real reference arrives. Entries are consumed
  // on disown; a topic that is never really subscribed keeps its lone preview
  // sample (the Value column shows the connect-time reading).
  std::set<PreviewId> preview_promoted_pending_disown_;
  int preview_timeout_ms_ = 4000;
};

}  // namespace PJ
