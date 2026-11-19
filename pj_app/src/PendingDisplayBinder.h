// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QDomDocument>
#include <QDomElement>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "LayoutXml.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_scene_common/scene_dock_widget.h"

namespace PJ {

class CustomWidgetDockWidget;
class LogDockWidget;
class TopicDemandTracker;

// One staged display intent waiting for its topic to materialize: a plot curve (kCurve, from a layout restore or a
// placeholder drop; the binder completes it) or a dock-owned intent (kDockIntent: a scene layer in its dock's
// deferred queue, a Custom Widget wire waiting to resolve; the dock completes it, the binder only holds the demand
// reference).
struct PendingDisplayEntry {
  enum class Kind { kCurve, kDockIntent };

  Kind kind = Kind::kCurve;
  QPointer<PlotWidget> plot;       ///< kCurve target; null means the plot was destroyed.
  QPointer<QObject> intent_owner;  ///< kDockIntent target; null means the dock was destroyed.
  /// kDockIntent: true while the owner still holds the intent. Call only while the owner is alive.
  std::function<bool()> still_pending;
  /// The staged widget as a plain QObject*, for identity only: the destroyed() sweep matches on it, because inside
  /// destroyed() the widget is already a bare QWidget and QPointer<T>::data() would be an invalid downcast.
  QObject* target = nullptr;
  layout_xml::SeriesPath path;    ///< Topic (+field for curves; XY: the Y source).
  layout_xml::SeriesPath x_path;  ///< Non-empty topic means this curve entry is XY.
  QDomDocument curve_doc;         ///< kCurve: owns the detached <curve> clone.
  QDomElement curve_element;      ///< kCurve: element imported into curve_doc.
  // One demand reference per (dataset, topic name) the entry's path(s) currently name; empty without a tracker.
  std::vector<std::pair<DatasetId, QString>> demand_refs;
  // The dataset an interactive drop was staged against (layout-restore entries have none). Resolution binds only
  // this dataset while it lives, and falls back to the load-order scan once it is gone (a stream reconnect mints a
  // fresh id).
  std::optional<DatasetId> preferred_dataset;
  /// An unresolved curve saved with pending_intent="true": real workspace state, so the drain prompt must not
  /// discard it.
  bool persistent_intent = false;
  /// Captured at staging: true, a restored or populated plot keeps its viewport when the late curve lands; false, a
  /// fresh empty plot auto-fits its first data (re-checked against the plot's live state at completion).
  bool preserve_viewport_on_completion = false;

  [[nodiscard]] bool isXY() const {
    return kind == Kind::kCurve && !x_path.topic.isEmpty();
  }

  [[nodiscard]] bool targetIsNull() const {
    return kind == Kind::kCurve ? plot.isNull() : intent_owner.isNull();
  }
};

// GUI-thread registry of display intents whose topics were not in the catalog when staged: plot curves from a
// layout restore (collect()) or a placeholder drop (addPendingCurve()), plus demand references for intents a dock
// owns and completes itself (scene layers in its deferred queue, unresolved Custom Widget wires). It owns no widgets
// or dock intents.
//
// A pending intent is a demand reference: a demand-capable source transmits a topic only while something references
// it (TopicDemandTracker), so without one the topic never subscribes and the entry waits forever. Every entry takes a
// reference for its topic(s) when staged and releases it exactly once: on completion, on its target widget's
// destruction (eagerly, so a dead pend does not keep a quiet stream's topic subscribed), or on clear()/collect().
// Without a `tracker` (unit tests) there is no demand tracking.
class PendingDisplayBinder : public QObject {
 public:
  explicit PendingDisplayBinder(CatalogModel& catalog, TopicDemandTracker* tracker = nullptr);

  // Stores the layout's unresolved per-plot curves as detached DOM clones, replacing every staged entry (its
  // references released). Callers then re-register demand for the rebuilt docks' queues.
  void collect(const QDomDocument& doc, const QHash<QString, PlotWidget*>& plots_by_state_id);

  // Stages one drop of an advertised scalar placeholder on `plot`, leaving other entries alone. An empty
  // `path.field` means every scalar field the topic turns out to have (a placeholder has no fields yet): one curve
  // per field, as a real topic-node drop gives. `preferred_dataset` scopes resolution when two datasets advertise the
  // same topic name.
  void addPendingCurve(
      PlotWidget* plot, const layout_xml::SeriesPath& path, std::optional<DatasetId> preferred_dataset = std::nullopt);

  // Tracks demand for one intent already in `dock`'s deferred queue (ignored when missing). Released once the queue
  // completes or drops it; the binder never calls addTopic().
  void addPendingSceneLayer(
      SceneDockWidget* dock, const QString& topic_name, std::optional<DatasetId> preferred_dataset);

  // Tracks demand for `dock`'s pending wires on `topic_name` (one of its pendingDemands(); others are ignored). The
  // dock resolves its wires itself; the reference is released once no pending wire names the topic, checked on
  // every bindingsChanged() and flush().
  void addPendingCustomWidgetWire(
      CustomWidgetDockWidget* dock, const QString& topic_name, std::optional<DatasetId> preferred_dataset);

  // The same for a Log panel's pending topics, re-checked on every topicsChanged() and flush().
  void addPendingLogTopic(LogDockWidget* dock, const QString& topic_name, std::optional<DatasetId> preferred_dataset);

  // Attempts the entries whose topics arrived; an empty set is the drain pass and tries all. Returns the intents
  // COMPLETED, not curves added: a topic that materializes with no numeric field fulfills its intent.
  [[nodiscard]] int flush(const QSet<QString>& topics);

  // For the missing-curve prompt: omits dead plots, persistent intents and dock intents (a dock reports its own).
  [[nodiscard]] QList<layout_xml::SeriesPath> unresolved() const;

  void clear();
  [[nodiscard]] bool empty() const;
  [[nodiscard]] int size() const;

 private:
  // References every dataset that currently names the entry's topic(s), real or advertised: the safe superset when
  // the name is ambiguous. Idempotent, and run on every flush() too: a layout restored before its stream connects
  // stages entries while no dataset names the topic, and the reference must attach when the advertise burst lands.
  void refreshDemandRefs(PendingDisplayEntry& entry);
  // Releases exactly the references refreshDemandRefs recorded for this entry.
  void releaseDemandRefs(const PendingDisplayEntry& entry);
  // CatalogModel::resolveCurveKey honouring `preferred` (see PendingDisplayEntry::preferred_dataset).
  [[nodiscard]] std::optional<QString> resolveEntryPath(
      const layout_xml::SeriesPath& path, std::optional<DatasetId> preferred) const;
  // Keys of every scalar field of the topic in one resolved dataset. nullopt: absent or ambiguous; empty: the topic
  // has no numeric field, which fulfills the intent.
  [[nodiscard]] std::optional<std::vector<QString>> scalarKeysForTopic(
      const layout_xml::SeriesPath& path, std::optional<DatasetId> preferred) const;
  // `still_pending` is asked on every flush() pass that names the topic. False when the arguments are empty or an
  // identical intent (same owner, topic and preferred dataset) is already staged.
  bool stageDockIntent(
      QObject* owner, const QString& topic_name, std::optional<DatasetId> preferred_dataset,
      std::function<bool()> still_pending);
  // Releases the kDockIntent entries of `owner` it no longer holds.
  void releaseCompletedDockIntents(const QObject* owner);
  [[nodiscard]] static bool dockIntentCompleted(const PendingDisplayEntry& entry);
  // The dedup gate for interactive drops.
  [[nodiscard]] bool hasEntryFor(
      PendingDisplayEntry::Kind kind, const QObject* target, const layout_xml::SeriesPath& path,
      std::optional<DatasetId> preferred_dataset) const;
  // Once per target. Lazy QPointer checks are not enough: on a quiet stream no flush runs, and a dead pend's
  // reference would keep its topic subscribed forever.
  void watchTargetDestruction(QObject* target);

  CatalogModel& catalog_;
  TopicDemandTracker* tracker_ = nullptr;
  std::vector<PendingDisplayEntry> entries_;
  // Targets with a destroyed() hook. A hook left after the last entry completes finds nothing to release.
  QSet<QObject*> watched_targets_;
};

}  // namespace PJ
