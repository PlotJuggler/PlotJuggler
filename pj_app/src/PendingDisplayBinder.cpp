// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PendingDisplayBinder.h"

#include <QDomNodeList>
#include <algorithm>
#include <utility>

#include "pj_custom_widgets/CustomWidgetDockWidget.h"
#include "pj_log/LogDockWidget.h"
#include "pj_plotting/PlotXml.h"
#include "pj_runtime/TopicDemandTracker.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
// Real data and advertised placeholders both count.
std::vector<DatasetId> datasetsNaming(const CatalogModel& catalog, const QString& topic_name) {
  std::vector<DatasetId> ids;
  if (topic_name.isEmpty()) {
    return ids;
  }
  for (const CatalogItem& item : catalog.items()) {
    if (item.topic_name == topic_name && std::find(ids.begin(), ids.end(), item.dataset_id) == ids.end()) {
      ids.push_back(item.dataset_id);
    }
  }
  return ids;
}
}  // namespace

PendingDisplayBinder::PendingDisplayBinder(CatalogModel& catalog, TopicDemandTracker* tracker)
    : catalog_(catalog), tracker_(tracker) {}

void PendingDisplayBinder::refreshDemandRefs(PendingDisplayEntry& entry) {
  if (tracker_ == nullptr) {
    return;
  }
  // Only datasets this entry does not reference yet gain one.
  const auto ensure_refs_for = [this, &entry](const QString& topic_name) {
    for (const DatasetId id : datasetsNaming(catalog_, topic_name)) {
      const auto ref = std::make_pair(id, topic_name);
      if (std::find(entry.demand_refs.begin(), entry.demand_refs.end(), ref) == entry.demand_refs.end()) {
        tracker_->addReference(id, topic_name);
        entry.demand_refs.push_back(ref);
      }
    }
  };
  ensure_refs_for(entry.path.topic);
  if (entry.isXY()) {
    ensure_refs_for(entry.x_path.topic);
  }
}

void PendingDisplayBinder::releaseDemandRefs(const PendingDisplayEntry& entry) {
  if (tracker_ == nullptr) {
    return;
  }
  for (const auto& [id, name] : entry.demand_refs) {
    tracker_->removeReference(id, name);
  }
}

// One demand-vs-intent match rule: same topic, and when the intent prefers a
// dataset the demand must have resolved to the same one.
static bool demandMatches(
    const SceneDockWidget::PendingRestoreDemand& demand, const QString& topic_name,
    const std::optional<DatasetId>& preferred_dataset) {
  return demand.topic_name == topic_name &&
         (!preferred_dataset.has_value() || demand.preferred_dataset == preferred_dataset);
}

void PendingDisplayBinder::collect(const QDomDocument& doc, const QHash<QString, PlotWidget*>& plots_by_state_id) {
  for (const PendingDisplayEntry& entry : entries_) {
    releaseDemandRefs(entry);
  }
  entries_.clear();

  const QDomNodeList plot_nodes = doc.elementsByTagName(u"plot"_s);
  for (int i = 0; i < plot_nodes.size(); ++i) {
    const QDomElement plot_element = plot_nodes.at(i).toElement();
    if (plot_element.isNull()) {
      continue;
    }
    PlotWidget* plot = plots_by_state_id.value(plot_element.attribute(u"id"_s), nullptr);
    if (plot == nullptr) {
      continue;
    }

    // A bound curve's slot holds its PlotWidget source name: the catalog key, an XY curve's saved title.
    auto saved_order = std::make_shared<QStringList>();
    for (QDomElement curve = plot_element.firstChildElement(u"curve"_s); !curve.isNull();
         curve = curve.nextSiblingElement(u"curve"_s)) {
      PendingDisplayEntry entry;
      entry.saved_order = saved_order;
      entry.saved_slot = saved_order->size();
      saved_order->push_back(QString());
      entry.plot = plot;
      entry.target = plot;
      entry.persistent_intent = plot_xml::isPendingIntent(curve);
      entry.preserve_viewport_on_completion = plot->hasSavedViewport() || !plot->curveList().empty();

      // layout_xml's reader, so the range-checked dataset-id parse matches the writer. XY curves carry x_/y_ pairs
      // (Y is the entry's primary path).
      if (const auto x_path = layout_xml::readXyXPath(curve)) {
        entry.x_path = *x_path;
        if (const auto y_path = layout_xml::readXyYPath(curve)) {
          entry.path = *y_path;
        }
        if (catalog_.resolveCurveKey(entry.x_path).has_value() && catalog_.resolveCurveKey(entry.path).has_value()) {
          saved_order->back() = curve.attribute(u"name"_s);
          continue;
        }
      } else if (const auto ts_path = layout_xml::readTimeSeriesPath(curve)) {
        entry.path = *ts_path;
        if (const auto key = catalog_.resolveCurveKey(entry.path)) {
          saved_order->back() = *key;
          continue;
        }
      } else {
        continue;
      }

      entry.curve_element = entry.curve_doc.importNode(curve, /*deep=*/true).toElement();
      entry.curve_doc.appendChild(entry.curve_element);
      if (entry.persistent_intent) {
        const auto identity =
            catalog_.resolveDatasetIdentity(entry.path.dataset_id, entry.path.dataset_source, entry.path.dataset_path);
        entry.preferred_dataset = identity.id;
        static_cast<void>(plot->rememberPendingCurveIntent(entry.curve_element, /*notify=*/false));
      }
      refreshDemandRefs(entry);
      watchTargetDestruction(plot);
      entries_.push_back(std::move(entry));
    }
  }
}

bool PendingDisplayBinder::hasEntryFor(
    PendingDisplayEntry::Kind kind, const QObject* target, const layout_xml::SeriesPath& path,
    std::optional<DatasetId> preferred_dataset) const {
  for (const PendingDisplayEntry& entry : entries_) {
    if (entry.kind == kind && entry.target == target && entry.path == path &&
        entry.preferred_dataset == preferred_dataset) {
      return true;
    }
  }
  return false;
}

void PendingDisplayBinder::addPendingCurve(
    PlotWidget* plot, const layout_xml::SeriesPath& path, std::optional<DatasetId> preferred_dataset) {
  if (plot == nullptr || path.topic.isEmpty()) {
    return;
  }
  // A second drop of the same placeholder must not stage a second intent: once the first binds every field, the
  // duplicate's addCurve() calls all return null, so it would wait forever holding its reference (and re-add ghost
  // curves after a manual delete on a later flush).
  layout_xml::SeriesPath qualified_path = path;
  if (preferred_dataset.has_value()) {
    if (qualified_path.dataset_id == 0) {
      qualified_path.dataset_id = *preferred_dataset;
    }
    if (qualified_path.dataset_source.isEmpty()) {
      qualified_path.dataset_source = catalog_.datasetSourceName(*preferred_dataset).value_or(QString{});
    }
    if (qualified_path.dataset_path.isEmpty()) {
      qualified_path.dataset_path = catalog_.datasetSourcePath(*preferred_dataset);
    }
  }
  if (hasEntryFor(PendingDisplayEntry::Kind::kCurve, plot, qualified_path, preferred_dataset)) {
    return;
  }
  PendingDisplayEntry entry;
  entry.plot = plot;
  entry.target = plot;
  entry.path = qualified_path;
  entry.preferred_dataset = preferred_dataset;
  entry.persistent_intent = true;
  entry.preserve_viewport_on_completion = plot->hasSavedViewport() || !plot->curveList().empty();
  entry.curve_element = entry.curve_doc.createElement(u"curve"_s);
  entry.curve_doc.appendChild(entry.curve_element);
  entry.curve_element.setAttribute(u"topic"_s, qualified_path.topic);
  entry.curve_element.setAttribute(u"field"_s, qualified_path.field);
  plot_xml::markPendingIntent(entry.curve_element);
  if (qualified_path.dataset_id != 0) {
    entry.curve_element.setAttribute(u"dataset_id"_s, QString::number(qualified_path.dataset_id));
  }
  if (!qualified_path.dataset_source.isEmpty()) {
    entry.curve_element.setAttribute(u"dataset_source"_s, qualified_path.dataset_source);
  }
  if (!qualified_path.dataset_path.isEmpty()) {
    entry.curve_element.setAttribute(u"dataset_path"_s, qualified_path.dataset_path);
  }
  static_cast<void>(plot->rememberPendingCurveIntent(entry.curve_element, /*notify=*/true));
  refreshDemandRefs(entry);
  watchTargetDestruction(plot);
  entries_.push_back(std::move(entry));
}

void PendingDisplayBinder::addPendingSceneLayer(
    SceneDockWidget* dock, const QString& topic_name, std::optional<DatasetId> preferred_dataset) {
  if (dock == nullptr || topic_name.isEmpty()) {
    return;
  }
  const auto owns_intent = [dock, topic_name, preferred_dataset]() {
    const auto demands = dock->pendingRestoreDemands();
    return std::any_of(
        demands.cbegin(), demands.cend(),
        [&topic_name, &preferred_dataset](const SceneDockWidget::PendingRestoreDemand& demand) {
          return demandMatches(demand, topic_name, preferred_dataset);
        });
  };
  if (!owns_intent()) {
    return;
  }
  // The dock's queue reports its completions through flush() passes, so no change signal.
  static_cast<void>(stageDockIntent(dock, topic_name, preferred_dataset, owns_intent));
}

void PendingDisplayBinder::addPendingCustomWidgetWire(
    CustomWidgetDockWidget* dock, const QString& topic_name, std::optional<DatasetId> preferred_dataset) {
  if (dock == nullptr || topic_name.isEmpty()) {
    return;
  }
  // Matched on the topic alone: the preferred dataset a pending wire reports moves as datasets come
  // and go, and the reference must stay until no wire of the dock waits on the topic.
  const auto waits_for_topic = [dock, topic_name]() { return dock->hasPendingTopic(topic_name); };
  if (!waits_for_topic()) {
    return;
  }
  // The dock's own retry resolves wires outside flush(), so its bindingsChanged re-checks them too.
  // watched_targets_ holds every dock staged before: one connection per dock.
  const bool connected = watched_targets_.contains(dock);
  if (stageDockIntent(dock, topic_name, preferred_dataset, waits_for_topic) && !connected) {
    connect(
        dock, &CustomWidgetDockWidget::bindingsChanged, this, [this, dock]() { releaseCompletedDockIntents(dock); });
  }
}

void PendingDisplayBinder::addPendingLogTopic(
    LogDockWidget* dock, const QString& topic_name, std::optional<DatasetId> preferred_dataset) {
  if (dock == nullptr || topic_name.isEmpty()) {
    return;
  }
  const auto waits_for_topic = [dock, topic_name]() { return dock->hasPendingTopic(topic_name); };
  if (!waits_for_topic()) {
    return;
  }
  const bool connected = watched_targets_.contains(dock);
  if (stageDockIntent(dock, topic_name, preferred_dataset, waits_for_topic) && !connected) {
    connect(dock, &LogDockWidget::topicsChanged, this, [this, dock]() { releaseCompletedDockIntents(dock); });
  }
}

bool PendingDisplayBinder::stageDockIntent(
    QObject* owner, const QString& topic_name, std::optional<DatasetId> preferred_dataset,
    std::function<bool()> still_pending) {
  if (owner == nullptr || topic_name.isEmpty() || !still_pending) {
    return false;
  }
  if (hasEntryFor(
          PendingDisplayEntry::Kind::kDockIntent, owner, layout_xml::SeriesPath{topic_name, QString()},
          preferred_dataset)) {
    return false;  // same dock, same topic: one pending intent is enough
  }
  PendingDisplayEntry entry;
  entry.kind = PendingDisplayEntry::Kind::kDockIntent;
  entry.intent_owner = owner;
  entry.still_pending = std::move(still_pending);
  entry.target = owner;
  entry.path = layout_xml::SeriesPath{topic_name, QString()};
  entry.preferred_dataset = preferred_dataset;
  refreshDemandRefs(entry);
  watchTargetDestruction(owner);
  entries_.push_back(std::move(entry));
  return true;
}

void PendingDisplayBinder::releaseCompletedDockIntents(const QObject* owner) {
  auto it = entries_.begin();
  while (it != entries_.end()) {
    if (it->kind == PendingDisplayEntry::Kind::kDockIntent && it->target == owner && dockIntentCompleted(*it)) {
      releaseDemandRefs(*it);
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
}

void PendingDisplayBinder::watchTargetDestruction(QObject* target) {
  if (watched_targets_.contains(target)) {
    return;
  }
  watched_targets_.insert(target);
  connect(target, &QObject::destroyed, this, [this](QObject* dead) {
    watched_targets_.remove(dead);
    // By the raw pointer recorded at staging: a QWidget target's QPointer is still non-null inside destroyed() (it
    // nulls only in ~QObject), so a targetIsNull() sweep would find nothing. Entries nulled by an earlier death are
    // swept too.
    auto it = entries_.begin();
    while (it != entries_.end()) {
      const bool owned_by_dead = it->target == dead;
      if (owned_by_dead || it->targetIsNull()) {
        releaseDemandRefs(*it);
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
  });
}

bool PendingDisplayBinder::dockIntentCompleted(const PendingDisplayEntry& entry) {
  return entry.intent_owner.isNull() || !entry.still_pending();
}

std::optional<QString> PendingDisplayBinder::resolveEntryPath(
    const layout_xml::SeriesPath& path, std::optional<DatasetId> preferred) const {
  // A preferred dataset binds ONLY itself while it lives: if it does not name the series yet, wait, never steal a
  // same-named field from a sibling. Once the id is gone (a stream reconnect mints a fresh one), the strict
  // resolution below binds a unique successor and stays pending under ambiguity.
  if (preferred.has_value()) {
    layout_xml::SeriesPath preferred_path = path;
    preferred_path.dataset_id = *preferred;
    if (preferred_path.dataset_source.isEmpty()) {
      preferred_path.dataset_source = catalog_.datasetSourceName(*preferred).value_or(QString{});
    }
    if (const auto key = catalog_.resolveCurveKey(preferred_path)) {
      return key;
    }
    if (catalog_.datasetSourceName(*preferred).has_value()) {
      return std::nullopt;
    }
  }
  return catalog_.resolveCurveKey(path);
}

std::optional<std::vector<QString>> PendingDisplayBinder::scalarKeysForTopic(
    const layout_xml::SeriesPath& path, std::optional<DatasetId> preferred) const {
  std::optional<DatasetId> selected_dataset;
  if (preferred.has_value() && catalog_.datasetSourceName(*preferred).has_value()) {
    selected_dataset = preferred;
  } else if (path.dataset_id != 0 || !path.dataset_source.isEmpty() || !path.dataset_path.isEmpty()) {
    const auto identity = catalog_.resolveDatasetIdentity(path.dataset_id, path.dataset_source, path.dataset_path);
    if (identity.ambiguous || !identity.id.has_value()) {
      return std::nullopt;
    }
    selected_dataset = identity.id;
  }

  QSet<DatasetId> materialized_datasets;
  std::vector<QString> keys;
  for (const CatalogItem& item : catalog_.items()) {
    if (item.topic_name != path.topic) {
      continue;
    }
    if (selected_dataset.has_value() && item.dataset_id != *selected_dataset) {
      continue;
    }
    if (isScalarField(item)) {
      materialized_datasets.insert(item.dataset_id);
      keys.push_back(item.key);
    } else if (asObjectTopic(item) != nullptr) {
      materialized_datasets.insert(item.dataset_id);
    }
  }
  if (selected_dataset.has_value()) {
    if (!materialized_datasets.contains(*selected_dataset)) {
      return std::nullopt;
    }
    return keys;
  }
  if (materialized_datasets.size() != 1) {
    return std::nullopt;
  }
  const DatasetId only_dataset = *materialized_datasets.cbegin();
  std::erase_if(keys, [&catalog = catalog_, only_dataset](const QString& key) {
    const auto descriptor = catalog.curveDescriptor(key);
    return !descriptor.has_value() || descriptor->dataset_id != only_dataset;
  });
  return keys;
}

int PendingDisplayBinder::flush(const QSet<QString>& topics) {
  if (entries_.empty()) {
    return 0;
  }

  const bool drain_pass = topics.isEmpty();
  QSet<PlotWidget*> touched;
  QSet<PlotWidget*> zoom_on_completion;
  QSet<PlotWidget*> preserve_viewport_on_completion;
  QHash<PlotWidget*, std::shared_ptr<QStringList>> saved_orders;
  int completed_intents = 0;

  auto it = entries_.begin();
  while (it != entries_.end()) {
    PendingDisplayEntry& entry = *it;
    if (entry.targetIsNull()) {
      releaseDemandRefs(entry);
      it = entries_.erase(it);
      continue;
    }

    // Datasets naming the topic may have appeared since staging (an advertise burst after a layout restore).
    refreshDemandRefs(entry);

    if (!drain_pass && !topics.contains(entry.path.topic) && (!entry.isXY() || !topics.contains(entry.x_path.topic))) {
      ++it;
      continue;
    }

    if (entry.kind == PendingDisplayEntry::Kind::kDockIntent) {
      if (dockIntentCompleted(entry)) {
        ++completed_intents;
        releaseDemandRefs(entry);
        it = entries_.erase(it);
      } else {
        ++it;
      }
      continue;
    }

    std::optional<QString> key = resolveEntryPath(entry.path, entry.preferred_dataset);
    std::optional<QString> x_key;
    if (entry.isXY()) {
      x_key = resolveEntryPath(entry.x_path, entry.preferred_dataset);
    }

    // An empty-field placeholder drop: one curve per scalar field, which no single descriptorForPath answer covers.
    if (!key.has_value() && !entry.isXY() && entry.path.field.isEmpty()) {
      const std::optional<std::vector<QString>> field_keys = scalarKeysForTopic(entry.path, entry.preferred_dataset);
      if (field_keys.has_value()) {
        PlotWidget* plot = entry.plot.data();
        const bool was_empty_without_saved_viewport = plot->curveList().empty() && !plot->hasSavedViewport();
        bool any_bound = false;
        for (const QString& field_key : *field_keys) {
          any_bound =
              plot->addCurveFromPending(field_key, entry.preserve_viewport_on_completion) != nullptr || any_bound;
        }
        if (any_bound) {
          touched.insert(plot);
          if (entry.preserve_viewport_on_completion) {
            preserve_viewport_on_completion.insert(plot);
          } else if (was_empty_without_saved_viewport) {
            zoom_on_completion.insert(plot);
          }
        }
        ++completed_intents;
        plot->forgetPendingCurveIntent(entry.curve_element, /*notify=*/false);
        releaseDemandRefs(entry);
        it = entries_.erase(it);
        continue;
      }
      ++it;
      continue;
    }

    if (!key.has_value() || (entry.isXY() && !x_key.has_value())) {
      ++it;
      continue;
    }

    if (entry.isXY()) {
      entry.curve_element.setAttribute(u"curve_x"_s, *x_key);
      entry.curve_element.setAttribute(u"curve_y"_s, *key);
    } else {
      entry.curve_element.setAttribute(u"name"_s, *key);
    }

    PlotWidget* plot = entry.plot.data();
    const bool was_empty_without_saved_viewport = plot->curveList().empty() && !plot->hasSavedViewport();
    const PlotWidget::CurveInfo* bound =
        plot->applyCurveElement(entry.curve_element, entry.preserve_viewport_on_completion);
    if (bound == nullptr) {
      ++it;
      continue;
    }

    if (entry.saved_order != nullptr) {
      (*entry.saved_order)[entry.saved_slot] = bound->source_name;
      saved_orders.insert(plot, entry.saved_order);
    }
    touched.insert(plot);
    if (entry.preserve_viewport_on_completion) {
      preserve_viewport_on_completion.insert(plot);
    } else if (was_empty_without_saved_viewport) {
      zoom_on_completion.insert(plot);
    }
    ++completed_intents;
    plot->forgetPendingCurveIntent(entry.curve_element, /*notify=*/false);
    releaseDemandRefs(entry);
    it = entries_.erase(it);
  }

  for (PlotWidget* plot : touched) {
    // Before the viewport: the saved window converts through the first curve's display offset.
    if (const std::shared_ptr<QStringList> order = saved_orders.value(plot)) {
      plot->restoreCurveOrder(*order);
    }
    // A marker scope saved against a dataset absent at restore follows the (possibly reminted) dataset its curves
    // just bound to.
    plot->resolvePendingMarkerScopes();
    // At restore xmlLoadState converted the saved window with display offset 0 (no dataset yet); re-apply it with
    // the live offset, so data fills the final window as it streams in. clear_after=false: the drain pass clears
    // the stash.
    if (plot->hasSavedViewport()) {
      plot->applySavedViewportOrZoom(/*clear_after=*/false);
    } else if (!preserve_viewport_on_completion.contains(plot) && zoom_on_completion.contains(plot)) {
      plot->zoomOut(/*emit_signal=*/false);
    }
  }
  return completed_intents;
}

QList<layout_xml::SeriesPath> PendingDisplayBinder::unresolved() const {
  QList<layout_xml::SeriesPath> paths;
  for (const PendingDisplayEntry& entry : entries_) {
    if (entry.kind != PendingDisplayEntry::Kind::kCurve || entry.plot.isNull()) {
      continue;
    }
    if (entry.persistent_intent) {
      continue;
    }
    // Only the half that still does not resolve: an XY entry may wait on one source, and listing a present series
    // as missing would mislead the prompt (as rebindCurveKeys reports).
    if (entry.isXY() && !catalog_.resolveCurveKey(entry.x_path).has_value()) {
      paths.push_back(entry.x_path);
    }
    if (!catalog_.resolveCurveKey(entry.path).has_value()) {
      paths.push_back(entry.path);
    }
  }
  return paths;
}

void PendingDisplayBinder::clear() {
  for (const PendingDisplayEntry& entry : entries_) {
    releaseDemandRefs(entry);
  }
  entries_.clear();
}

bool PendingDisplayBinder::empty() const {
  return entries_.empty();
}

int PendingDisplayBinder::size() const {
  return static_cast<int>(entries_.size());
}

}  // namespace PJ
