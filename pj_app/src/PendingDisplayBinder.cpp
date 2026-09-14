// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PendingDisplayBinder.h"

#include <QDomNodeList>
#include <algorithm>
#include <utility>

#include "pj_plotting/PlotXml.h"
#include "pj_runtime/TopicDemandTracker.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
// Every dataset that currently names `topic_name` in the catalog — real data or
// an advertised placeholder both count (see PendingDisplayBinder's class doc: a
// pending bind is a demand reference, and referencing every candidate dataset is
// the safe superset when the name is ambiguous across datasets).
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

std::optional<QString> resolveSeriesPath(const CatalogModel& catalog, const layout_xml::SeriesPath& path) {
  // Thin adapter over the shared catalog resolver: unpack the SeriesPath's
  // qualifiers into CatalogModel::resolveCurveKey, which owns the whole three-tier
  // algorithm (qualified exact-id/path-fallback + unqualified unique-match) so this
  // and PlotWidget's clipboard rebind never drift.
  return catalog.resolveCurveKey(path.dataset_id, path.dataset_source, path.dataset_path, path.topic, path.field);
}

std::optional<CurveDescriptor> resolveSeriesDescriptor(
    const CatalogModel& catalog, const layout_xml::SeriesPath& path) {
  if (const std::optional<QString> key = resolveSeriesPath(catalog, path); key.has_value()) {
    return catalog.curveDescriptor(*key);
  }
  return std::nullopt;
}

PendingDisplayBinder::PendingDisplayBinder(CatalogModel& catalog, TopicDemandTracker* tracker)
    : catalog_(catalog), tracker_(tracker) {}

void PendingDisplayBinder::refreshDemandRefs(PendingDisplayEntry& entry) {
  if (tracker_ == nullptr) {
    return;
  }
  // Idempotent: only datasets not yet referenced by this entry gain a reference.
  // Called at stage time AND on every flush() pass, because the topic may become
  // nameable only later — a layout restored before its stream connects stages
  // entries while NO dataset names the topic yet; the reference must attach the
  // moment the advertise burst lands, or the topic never subscribes and the
  // entry waits forever.
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

    for (QDomElement curve = plot_element.firstChildElement(u"curve"_s); !curve.isNull();
         curve = curve.nextSiblingElement(u"curve"_s)) {
      PendingDisplayEntry entry;
      entry.plot = plot;
      entry.target = plot;
      entry.persistent_intent = plot_xml::isPendingIntent(curve);
      entry.preserve_viewport_on_completion = plot->hasSavedViewport() || !plot->curveList().empty();

      // Read curve attributes through layout_xml's single reader so the
      // range-checked dataset-id parse stays consistent with the layout writer.
      // XY curves carry x_/y_ pairs (Y is the entry's primary path); a
      // time-series curve carries the plain topic/field pair.
      if (const auto x_path = layout_xml::readXyXPath(curve)) {
        entry.x_path = *x_path;
        if (const auto y_path = layout_xml::readXyYPath(curve)) {
          entry.path = *y_path;
        }
        if (resolveSeriesPath(catalog_, entry.x_path).has_value() &&
            resolveSeriesPath(catalog_, entry.path).has_value()) {
          continue;
        }
      } else if (const auto ts_path = layout_xml::readTimeSeriesPath(curve)) {
        entry.path = *ts_path;
        if (resolveSeriesPath(catalog_, entry.path).has_value()) {
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
    const QObject* entry_target = entry.kind == PendingDisplayEntry::Kind::kCurve
                                      ? static_cast<const QObject*>(entry.plot.data())
                                      : static_cast<const QObject*>(entry.scene_dock.data());
    if (entry.kind == kind && entry_target == target && entry.path == path &&
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
  // Double-dropping the same placeholder must not stage a second intent: on
  // promotion the first entry binds every field, the duplicate's addCurve()
  // calls all return null, and it would wait forever holding its demand
  // reference (and re-add ghost curves after a manual delete on a later flush).
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
  const auto demands = dock->pendingRestoreDemands();
  const bool dock_owns_intent = std::any_of(
      demands.cbegin(), demands.cend(),
      [&topic_name, &preferred_dataset](const SceneDockWidget::PendingRestoreDemand& demand) {
        return demandMatches(demand, topic_name, preferred_dataset);
      });
  if (!dock_owns_intent) {
    return;
  }
  if (hasEntryFor(
          PendingDisplayEntry::Kind::kSceneLayer, dock, layout_xml::SeriesPath{topic_name, QString()},
          preferred_dataset)) {
    return;  // same dock, same topic — one pending layer is enough
  }
  PendingDisplayEntry entry;
  entry.kind = PendingDisplayEntry::Kind::kSceneLayer;
  entry.scene_dock = dock;
  entry.target = dock;
  entry.path = layout_xml::SeriesPath{topic_name, QString()};
  entry.preferred_dataset = preferred_dataset;
  refreshDemandRefs(entry);
  watchTargetDestruction(dock);
  entries_.push_back(std::move(entry));
}

void PendingDisplayBinder::watchTargetDestruction(QObject* target) {
  if (watched_targets_.contains(target)) {
    return;
  }
  watched_targets_.insert(target);
  connect(target, &QObject::destroyed, this, [this](QObject* dead) {
    watched_targets_.remove(dead);
    // Match the dying widget by the raw pointer recorded at staging: for
    // QWidget-derived targets the QPointer is still non-null inside destroyed()
    // (it nulls only for the QObject dtor layer), so a targetIsNull() sweep would
    // find nothing here. Entries already nulled by an earlier death are swept too.
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

bool PendingDisplayBinder::sceneEntryCompleted(const PendingDisplayEntry& entry) const {
  if (entry.scene_dock.isNull()) {
    return true;
  }
  const auto demands = entry.scene_dock->pendingRestoreDemands();
  return std::none_of(demands.cbegin(), demands.cend(), [&entry](const SceneDockWidget::PendingRestoreDemand& demand) {
    return demandMatches(demand, entry.path.topic, entry.preferred_dataset);
  });
}

std::optional<QString> PendingDisplayBinder::resolveEntryPath(
    const layout_xml::SeriesPath& path, std::optional<DatasetId> preferred) const {
  // A preferred dataset (an interactive drop's own placeholder) binds ONLY its
  // exact dataset while that dataset lives: if it does not yet name the series,
  // wait for it — never steal a same-named field from a sibling. Only when the
  // id is gone entirely (stream reconnects mint fresh DatasetIds) does the entry
  // fall through to the strict resolution below, which binds a unique successor
  // and stays pending under ambiguity.
  if (preferred.has_value()) {
    layout_xml::SeriesPath preferred_path = path;
    preferred_path.dataset_id = *preferred;
    if (preferred_path.dataset_source.isEmpty()) {
      preferred_path.dataset_source = catalog_.datasetSourceName(*preferred).value_or(QString{});
    }
    if (const auto key = catalog_.resolveCurveKey(
            preferred_path.dataset_id, preferred_path.dataset_source, preferred_path.dataset_path, preferred_path.topic,
            preferred_path.field)) {
      return key;
    }
    if (catalog_.datasetSourceName(*preferred).has_value()) {
      return std::nullopt;
    }
  }
  return resolveSeriesPath(catalog_, path);
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
  int completed_intents = 0;

  auto it = entries_.begin();
  while (it != entries_.end()) {
    PendingDisplayEntry& entry = *it;
    if (entry.targetIsNull()) {
      releaseDemandRefs(entry);
      it = entries_.erase(it);
      continue;
    }

    // The catalog may have gained datasets naming this entry's topic(s) since it
    // was staged (advertise burst after a layout restore) — attach the demand
    // references those datasets now warrant, or the topic never subscribes.
    refreshDemandRefs(entry);

    if (!drain_pass && !topics.contains(entry.path.topic) && (!entry.isXY() || !topics.contains(entry.x_path.topic))) {
      ++it;
      continue;
    }

    if (entry.kind == PendingDisplayEntry::Kind::kSceneLayer) {
      if (sceneEntryCompleted(entry)) {
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

    // Empty-field placeholder drop whose topic just materialized: bind one curve
    // per scalar field (mirrors dropping the real topic node). Handled here
    // because no single descriptorForPath answer exists for "all of them".
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
    if (plot->applyCurveElement(entry.curve_element, entry.preserve_viewport_on_completion) == nullptr) {
      ++it;
      continue;
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
    // A hidden marker scope saved against a dataset that was not loaded at
    // restore time follows the (possibly reminted) dataset its curves just bound to.
    plot->resolvePendingMarkerScopes();
    // Frame to the layout-saved window now that the dataset (hence its display offset)
    // exists: at restore xmlLoadState ran against an empty catalog (offset 0), so its
    // absolute->display conversion was wrong. applySavedViewportOrZoom re-applies the
    // saved range with the live offset — pinning the final window up front so data
    // fills into it like streaming — and falls back to zoomOut when there is no saved
    // range. clear_after=false: keep the stash; the drain pass clears it.
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
    // Report only the half/halves that still don't resolve: an XY entry can be
    // pending because just one source is missing, and listing a present series as
    // "missing" would mislead the prompt (matches rebindCurveKeys' reporting).
    if (entry.isXY() && !resolveSeriesPath(catalog_, entry.x_path).has_value()) {
      paths.push_back(entry.x_path);
    }
    if (!resolveSeriesPath(catalog_, entry.path).has_value()) {
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
