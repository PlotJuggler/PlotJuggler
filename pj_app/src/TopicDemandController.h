// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QObject>
#include <QSet>
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
class QWidget;

namespace PJ {

class DataEngine;
class CustomWidgetDockWidget;
class DataProcessorService;
class LogDockWidget;
class PendingDisplayBinder;
class PlotWidget;
class SceneDockWidget;
class StateTransitionsDockWidget;
class TopicDemandTracker;

// Resolves what each registered widget displays to (DatasetId, topic_name) and holds one TopicDemandTracker
// reference per displayed item (the tracker owns the ref-counting and emission contract). Every reference added for
// a widget is released when that widget is destroyed. Also runs the bounded field previews of advertised topics, and
// answers the curve list's "which catalog keys are displayed" (displayedKeys / displayedSeriesChanged).
//
// Threading: GUI thread only, like TopicDemandTracker and CatalogModel.
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

  // On every curveListChanged(), diffs the multiset of (dataset, topic) its curves resolve to (both sources of an XY
  // curve; a Data Processor output stands for its source topics) against the last one, and adds/removes exactly the
  // delta. Idempotent; an already-populated plot (layout restore) is referenced at once. The plot's view start is
  // also its datasets' on-demand visible start (VisibleStartSink): set before the demand on every curve change, so a
  // series it starts computes the visible range first, and on every move of its time axis, so the last plot to move
  // wins its datasets.
  void registerPlot(PlotWidget* plot);

  // Tracks `dock`'s layers through layerAdded/layerRemoved. Call before any layer can be added (before
  // xmlLoadState()/addTopic()): a missed layerAdded is never referenced.
  void registerSceneDock(SceneDockWidget* dock);

  // registerPlot's delta sync for a state-transitions strip, on every seriesListChanged. Idempotent.
  void registerStateTransitionsDock(StateTransitionsDockWidget* dock);

  // Same for a Custom Widget panel: holds one reference per distinct (dataset, topic) its resolved
  // wires read, diffed on every bindingsChanged() and released when the panel is destroyed.
  void registerCustomWidgetDock(CustomWidgetDockWidget* dock);

  // Same for a Log panel: one reference per (dataset, topic) it shows, diffed on every topicsChanged().
  void registerLogDock(LogDockWidget* dock);

  // An advertised SCALAR placeholder was dropped on `plot`: PendingDisplayBinder holds the demand reference until
  // the real topic materializes and the curve completes.
  void handlePlaceholderPlotDrop(PlotWidget* plot, DatasetId dataset_id, const QString& topic_name);

  // An advertised OBJECT placeholder was dropped on a mounted `dock`: the dock stages the intent,
  // PendingDisplayBinder holds only its demand reference.
  void handleSceneDockPlaceholderDrop(
      SceneDockWidget* dock, DatasetId dataset_id, const QString& topic_name, sdk::BuiltinObjectType object_type);

  // An advertised log placeholder was dropped on a Log panel: the panel keeps the topic pending and binds it when it
  // materializes, PendingDisplayBinder holds its demand reference meanwhile.
  void handleLogDockPlaceholderDrop(LogDockWidget* dock, DatasetId dataset_id, const QString& topic_name);

  // A bounded, self-releasing subscription: holds a demand reference until one real sample promotes the placeholder
  // to per-field rows, or until the preview timeout, whichever comes first. Used by the automatic census and the
  // manual double-click peek. No-op when the dataset is not per-topic-pause-capable, the topic already has a real
  // entry, a preview for it is in flight or queued, or it is already active (a real reference must not be
  // disturbed). Ignores the census memory, so a manual peek can retry a topic the census skipped.
  void requestFieldPreview(DatasetId dataset_id, const QString& topic_name);

  /// Catalog keys of everything displayed by the registered widgets inside `root` (none when null): plot curves
  /// (both sources of an XY curve), scene layers and config topics, state-transitions rows, resolved Custom Widget
  /// wires and Log panel topics. Pending items (placeholder drops, unresolved wires) are not included. Read from the
  /// widgets now, so it is only as fresh as the last displayedSeriesChanged().
  [[nodiscard]] QSet<QString> displayedKeys(const QWidget* root) const;

  // Applies to previews started after the call.
  void setPreviewTimeoutMsForTest(int timeout_ms);

 signals:
  /// What displayedKeys() returns may have changed: a registered widget gained or lost a series or
  /// was destroyed, or the catalog listed new items (an object layer can exist before its key).
  void displayedSeriesChanged();

 private:
  void syncPlot(PlotWidget* plot);
  // Tells the session where `plot`'s time view starts, for each dataset among `topics`; nothing for an XY plot.
  void reportVisibleStart(PlotWidget* plot, const std::vector<std::pair<DatasetId, QString>>& topics);
  void syncStateTransitionsDock(StateTransitionsDockWidget* dock);
  void syncCustomWidgetDock(CustomWidgetDockWidget* dock);
  void syncLogDock(LogDockWidget* dock);
  // Multiset diff: two curves on one topic hold two references, so removing one curve releases exactly one.
  // Stores `current` into `previous`.
  void applyTopicDelta(
      std::vector<std::pair<DatasetId, QString>> current, std::vector<std::pair<DatasetId, QString>>& previous);
  // Appends (dataset_id, topic_name), or, when topic_id is a Data Processor output, its source topics instead
  // (DataProcessorService::sourceTopicsForOutput): no plugin streams an output's name.
  void appendTopicRefs(
      std::vector<std::pair<DatasetId, QString>>& out, DatasetId dataset_id, const QString& topic_name,
      TopicId topic_id) const;
  // nullopt when the catalog no longer lists the topic (evicted before the signal arrived): nothing to reference.
  [[nodiscard]] std::optional<std::pair<DatasetId, QString>> resolveObjectTopic(ObjectTopicId topic_id) const;

  // Releases the previews whose topic just promoted, then previews new scalar placeholders (census).
  void onCatalogItemsAdded(const std::vector<CatalogItem>& items);
  // Previews every advertised kNone-classified placeholder on a per-topic-pause-capable dataset not yet previewed
  // this session. The classification is the size filter: object topics (point clouds, images) are never previewed.
  void runCensus(const std::vector<CatalogItem>& items);
  // Releases each in-flight preview whose topic appears in `items` as a real (non-placeholder) entry.
  void releaseCompletedPreviews(const std::vector<CatalogItem>& items);
  // The caller has already checked eligibility.
  void startPreview(DatasetId dataset_id, const QString& topic_name);
  // Releases the reference and timer, marks the topic previewed for the session, starts the next queued preview.
  // A no-op for an unknown preview, so promotion and timeout may both call it.
  void finishPreview(DatasetId dataset_id, const QString& topic_name);
  // Re-validates each queued preview: its state may have changed while it waited.
  void pumpPreviewQueue();
  // Releases every held preview reference and forgets all preview and census state.
  void onCatalogCleared();
  [[nodiscard]] bool previewInFlight(DatasetId dataset_id, const QString& topic_name) const;
  // A real (non-advertised) entry means the fields are already materialized: no preview needed.
  [[nodiscard]] bool topicHasRealEntry(DatasetId dataset_id, const QString& topic_name) const;

  CatalogModel& catalog_;
  TopicDemandTracker& tracker_;
  DataProcessorService& processors_;
  PendingDisplayBinder& pending_binder_;
  // Only to disown preview samples (evictTopicHistory); demand goes through the tracker.
  DataEngine& engine_;
  VisibleStartSink visible_start_;

  // Last-seen (dataset, topic) multiset per plot, i.e. the references it holds.
  std::unordered_map<PlotWidget*, std::vector<std::pair<DatasetId, QString>>> plot_topics_;
  // The reference each layer holds, keyed by ObjectTopicId, so layerRemoved releases exactly what layerAdded added
  // whatever the catalog did in between.
  std::unordered_map<SceneDockWidget*, std::unordered_map<uint32_t, std::pair<DatasetId, QString>>> scene_layer_topics_;
  std::unordered_map<StateTransitionsDockWidget*, std::vector<std::pair<DatasetId, QString>>> state_dock_topics_;
  std::unordered_map<CustomWidgetDockWidget*, std::vector<std::pair<DatasetId, QString>>> custom_dock_topics_;
  std::unordered_map<LogDockWidget*, std::vector<std::pair<DatasetId, QString>>> log_dock_topics_;

  // Same identity as a TopicDemandTracker reference.
  using PreviewId = std::pair<DatasetId, QString>;

  // When a topic in preview_promoted_pending_disown_ gains a real reference, hides its preview samples
  // (disownPreviewHistory) and consumes the entry.
  void onActiveTopicsChanged(DatasetId dataset_id, const std::vector<QString>& active_topics);
  // Raises the retention floor past every stored sample of the topic (DataEngine::evictTopicHistory). The columns
  // stay registered, so the field rows survive.
  void disownPreviewHistory(DatasetId dataset_id, const QString& topic_name);
  // Previews holding a reference at once; the rest wait in preview_queue_.
  static constexpr int kMaxConcurrentPreviews = 8;
  std::map<PreviewId, QTimer*> active_previews_;
  // FIFO, bounded by one advertise burst, so membership checks scan it linearly.
  std::deque<PreviewId> preview_queue_;
  // Previewed this session, so a re-advertise does not re-preview a quiet or timed-out topic. Cleared with the
  // catalog; the manual peek ignores it.
  std::set<PreviewId> censused_topics_;
  // Promoted by a preview, its preview samples still stored: disowned the moment a real reference arrives. A topic
  // never really subscribed keeps its preview sample (the Value column shows the connect-time reading).
  std::set<PreviewId> preview_promoted_pending_disown_;
  int preview_timeout_ms_ = 4000;
};

}  // namespace PJ
