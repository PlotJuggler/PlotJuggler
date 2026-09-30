// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "TopicDemandController.h"

#include <qwt_plot_curve.h>

#include <QTimer>
#include <algorithm>

#include "LayoutXml.h"
#include "PendingDisplayBinder.h"
#include "pj_datastore/engine.hpp"
#include "pj_plotting/DatastoreCurveAdapter.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/PointSeriesXY.h"
#include "pj_plotting/StateTransitionsDockWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "pj_scene_common/scene_dock_widget.h"

namespace PJ {

TopicDemandController::TopicDemandController(
    CatalogModel& catalog, TopicDemandTracker& tracker, DataProcessorService& processors,
    PendingDisplayBinder& pending_binder, DataEngine& engine, QObject* parent)
    : QObject(parent),
      catalog_(catalog),
      tracker_(tracker),
      processors_(processors),
      pending_binder_(pending_binder),
      engine_(engine) {
  // The one persistent catalog wiring this controller owns: drives preview
  // release-on-promotion, the advertise-burst census, and preview teardown.
  connect(&catalog_, &CatalogModel::itemsAdded, this, &TopicDemandController::onCatalogItemsAdded);
  connect(&catalog_, &CatalogModel::cleared, this, &TopicDemandController::onCatalogCleared);
  // Disowns a preview's fake-interest samples the moment a REAL reference
  // arrives for a preview-promoted topic (see onActiveTopicsChanged).
  connect(&tracker_, &TopicDemandTracker::activeTopicsChanged, this, &TopicDemandController::onActiveTopicsChanged);
}

TopicDemandController::~TopicDemandController() {
  // Balance every held preview reference on teardown (the timers are children of
  // this object and are destroyed by ~QObject).
  for (const auto& [id, timer] : active_previews_) {
    static_cast<void>(timer);
    tracker_.removeReference(id.first, id.second);
  }
}

void TopicDemandController::appendTopicRefs(
    std::vector<std::pair<DatasetId, QString>>& out, DatasetId dataset_id, const QString& topic_name,
    TopicId topic_id) const {
  const auto sources = processors_.sourceTopicsForOutput(topic_id);
  if (sources.empty()) {
    out.emplace_back(dataset_id, topic_name);
    return;
  }
  // A Data Processor output is not itself a streamed topic (no plugin owns that
  // name) — reference its resolved SOURCE(s) instead, per the v1 derived-input
  // resolution rule.
  for (const auto& [source_dataset_id, source_name] : sources) {
    out.emplace_back(source_dataset_id, QString::fromStdString(source_name));
  }
}

void TopicDemandController::registerPlot(PlotWidget* plot) {
  if (plot == nullptr || plot_topics_.count(plot) > 0) {
    return;  // idempotent: layout load/undo rewiring re-registers live plots
  }
  plot_topics_.emplace(plot, std::vector<std::pair<DatasetId, QString>>{});
  connect(plot, &PlotWidgetBase::curveListChanged, this, [this, plot]() { syncPlot(plot); });
  // A canvas drop named an advertised placeholder — register demand + a pending
  // bind instead of the (impossible, no storage id) normal add-curve path.
  connect(plot, &PlotWidget::placeholderCurveDropped, this, [this, plot](const QString& catalog_key) {
    const auto item = catalog_.itemDescriptor(catalog_key);
    if (item.has_value()) {
      handlePlaceholderPlotDrop(plot, item->dataset_id, item->topic_name);
    }
  });
  connect(plot, &QObject::destroyed, this, [this, plot]() {
    auto it = plot_topics_.find(plot);
    if (it == plot_topics_.end()) {
      return;
    }
    for (const auto& [dataset_id, topic_name] : it->second) {
      tracker_.removeReference(dataset_id, topic_name);
    }
    plot_topics_.erase(it);
  });
  syncPlot(plot);  // covers curves already present (e.g. restored before this call)
}

void TopicDemandController::syncPlot(PlotWidget* plot) {
  std::vector<std::pair<DatasetId, QString>> current;
  for (const auto& info : plot->curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    if (const auto* xy_series = dynamic_cast<const PointSeriesXY*>(info.curve->data())) {
      appendTopicRefs(
          current, xy_series->xSource().dataset_id, xy_series->xSource().topic_name, xy_series->xSource().topic_id);
      appendTopicRefs(
          current, xy_series->ySource().dataset_id, xy_series->ySource().topic_name, xy_series->ySource().topic_id);
      continue;
    }
    const auto item = catalog_.itemDescriptor(info.source_name);
    if (!item.has_value()) {
      continue;  // stale/unresolvable source_name — nothing to reference
    }
    const TopicId topic_id = isScalarField(*item) ? asScalarField(*item)->topic_id : 0;
    appendTopicRefs(current, item->dataset_id, item->topic_name, topic_id);
  }
  applyTopicDelta(std::move(current), plot_topics_[plot]);
}

void TopicDemandController::applyTopicDelta(
    std::vector<std::pair<DatasetId, QString>> current, std::vector<std::pair<DatasetId, QString>>& previous) {
  std::sort(current.begin(), current.end());
  std::sort(previous.begin(), previous.end());

  std::vector<std::pair<DatasetId, QString>> added;
  std::vector<std::pair<DatasetId, QString>> removed;
  // Multiset diff: current/previous may carry duplicates (two curves on the same
  // topic each hold their own reference), and std::set_difference over sorted
  // ranges subtracts per-multiplicity, so removing ONE curve on a shared topic
  // releases exactly one reference and leaves the other curve's intact.
  std::set_difference(current.begin(), current.end(), previous.begin(), previous.end(), std::back_inserter(added));
  std::set_difference(previous.begin(), previous.end(), current.begin(), current.end(), std::back_inserter(removed));

  for (const auto& [dataset_id, topic_name] : added) {
    tracker_.addReference(dataset_id, topic_name);
  }
  for (const auto& [dataset_id, topic_name] : removed) {
    tracker_.removeReference(dataset_id, topic_name);
  }
  previous = std::move(current);
}

std::optional<std::pair<DatasetId, QString>> TopicDemandController::resolveObjectTopic(ObjectTopicId topic_id) const {
  for (const CatalogItem& item : catalog_.items()) {
    if (const auto* object_topic = asObjectTopic(item);
        object_topic != nullptr && object_topic->object_topic_id == topic_id) {
      return std::make_pair(item.dataset_id, item.topic_name);
    }
  }
  return std::nullopt;
}

void TopicDemandController::registerSceneDock(SceneDockWidget* dock) {
  if (dock == nullptr || scene_layer_topics_.count(dock) > 0) {
    return;  // idempotent, mirroring registerPlot
  }
  scene_layer_topics_.emplace(dock, decltype(scene_layer_topics_)::mapped_type{});
  connect(dock, &SceneDockWidget::layerAdded, this, [this, dock](ObjectTopicId topic_id) {
    const auto resolved = resolveObjectTopic(topic_id);
    if (!resolved.has_value()) {
      return;
    }
    tracker_.addReference(resolved->first, resolved->second);
    scene_layer_topics_[dock][topic_id.id] = *resolved;
  });
  connect(dock, &SceneDockWidget::layerRemoved, this, [this, dock](ObjectTopicId topic_id) {
    auto dock_it = scene_layer_topics_.find(dock);
    if (dock_it == scene_layer_topics_.end()) {
      return;
    }
    auto topic_it = dock_it->second.find(topic_id.id);
    if (topic_it == dock_it->second.end()) {
      return;
    }
    tracker_.removeReference(topic_it->second.first, topic_it->second.second);
    dock_it->second.erase(topic_it);
  });
  connect(dock, &QObject::destroyed, this, [this, dock]() {
    auto dock_it = scene_layer_topics_.find(dock);
    if (dock_it == scene_layer_topics_.end()) {
      return;
    }
    for (const auto& [topic_ordinal, topic] : dock_it->second) {
      (void)topic_ordinal;
      tracker_.removeReference(topic.first, topic.second);
    }
    scene_layer_topics_.erase(dock_it);
  });
}

void TopicDemandController::registerStateTransitionsDock(StateTransitionsDockWidget* dock) {
  if (dock == nullptr || state_dock_topics_.count(dock) > 0) {
    return;  // idempotent, mirroring registerPlot
  }
  state_dock_topics_.emplace(dock, std::vector<std::pair<DatasetId, QString>>{});
  connect(dock->controller(), &StateTransitionsController::seriesListChanged, this, [this, dock]() {
    syncStateTransitionsDock(dock);
  });
  connect(dock, &QObject::destroyed, this, [this, dock]() {
    auto it = state_dock_topics_.find(dock);
    if (it == state_dock_topics_.end()) {
      return;
    }
    for (const auto& [dataset_id, topic_name] : it->second) {
      tracker_.removeReference(dataset_id, topic_name);
    }
    state_dock_topics_.erase(it);
  });
  syncStateTransitionsDock(dock);  // covers rows restored before this call
}

void TopicDemandController::syncStateTransitionsDock(StateTransitionsDockWidget* dock) {
  std::vector<std::pair<DatasetId, QString>> current;
  const auto displayed = dock->controller()->displayedTopics();
  current.reserve(static_cast<std::size_t>(displayed.size()));
  for (const auto& [dataset_id, topic_name] : displayed) {
    current.emplace_back(dataset_id, topic_name);
  }
  applyTopicDelta(std::move(current), state_dock_topics_[dock]);
}

void TopicDemandController::handlePlaceholderPlotDrop(
    PlotWidget* plot, DatasetId dataset_id, const QString& topic_name) {
  pending_binder_.addPendingCurve(plot, layout_xml::SeriesPath{topic_name, QString()}, dataset_id);
}

void TopicDemandController::handleSceneDockPlaceholderDrop(
    SceneDockWidget* dock, DatasetId dataset_id, const QString& topic_name, sdk::BuiltinObjectType object_type) {
  if (dock != nullptr && dock->deferTopicIntent(dataset_id, topic_name, object_type, topic_name)) {
    pending_binder_.addPendingSceneLayer(dock, topic_name, dataset_id);
  }
}

// --- bounded field-preview subscriptions ------------------------------------

void TopicDemandController::setPreviewTimeoutMsForTest(int timeout_ms) {
  preview_timeout_ms_ = timeout_ms;
}

void TopicDemandController::onCatalogItemsAdded(const std::vector<CatalogItem>& items) {
  // Release first (a promotion batch may also carry unrelated new placeholders),
  // then census the freshly-advertised scalar placeholders.
  releaseCompletedPreviews(items);
  runCensus(items);
}

void TopicDemandController::releaseCompletedPreviews(const std::vector<CatalogItem>& items) {
  if (active_previews_.empty()) {
    return;
  }
  std::vector<PreviewId> promoted;
  for (const CatalogItem& item : items) {
    if (asAdvertisedTopic(item) != nullptr) {
      continue;  // still a placeholder — the real sample has not landed yet
    }
    const PreviewId id{item.dataset_id, item.topic_name};
    if (active_previews_.count(id) > 0) {
      promoted.push_back(id);
    }
  }
  for (const PreviewId& id : promoted) {
    finishPreview(id.first, id.second);  // idempotent: duplicate field rows collapse to one release
    // The stored sample(s) are fake interest — flag them for disowning the
    // moment something REALLY subscribes (see onActiveTopicsChanged). UNLESS a
    // real hold (a pend completing, forced streaming) claimed the topic while
    // the preview was in flight: the topic stayed active through the preview's
    // release, so everything stored is really-requested history and a later
    // disown would wipe it.
    const auto active = tracker_.activeTopics(id.first);
    if (std::find(active.begin(), active.end(), id.second) == active.end()) {
      preview_promoted_pending_disown_.insert(id);
    }
  }
}

void TopicDemandController::onActiveTopicsChanged(DatasetId dataset_id, const std::vector<QString>& active_topics) {
  if (preview_promoted_pending_disown_.empty()) {
    return;
  }
  // Consume every pending entry for this dataset whose topic just became
  // actively referenced. The preview reference itself can't retrigger this:
  // the entry is only inserted AFTER the preview released on promotion.
  for (const QString& topic_name : active_topics) {
    const PreviewId id{dataset_id, topic_name};
    if (preview_promoted_pending_disown_.erase(id) > 0) {
      disownPreviewHistory(dataset_id, topic_name);
    }
  }
}

void TopicDemandController::disownPreviewHistory(DatasetId dataset_id, const QString& topic_name) {
  // Resolve the topic's scalar TopicId(s) through the catalog. One name maps to
  // one engine topic, but the scan tolerates catalog churn (fields all carry
  // the same topic_id; duplicates collapse via the sorted-unique pass).
  std::vector<TopicId> topic_ids;
  for (const CatalogItem& item : catalog_.items()) {
    if (item.dataset_id != dataset_id || item.topic_name != topic_name) {
      continue;
    }
    if (const auto* scalar = asScalarField(item); scalar != nullptr) {
      topic_ids.push_back(scalar->topic_id);
    }
  }
  std::sort(topic_ids.begin(), topic_ids.end());
  topic_ids.erase(std::unique(topic_ids.begin(), topic_ids.end()), topic_ids.end());
  // curveListChanged may have already populated plot caches from the preview.
  // Drop them synchronously before freeing chunks; no paint or event-loop turn
  // may intervene between invalidation and eviction.
  for (const auto& [plot, topics] : plot_topics_) {
    static_cast<void>(topics);
    for (const auto& info : plot->curveList()) {
      if (info.curve == nullptr) {
        continue;
      }
      if (auto* xy = dynamic_cast<PointSeriesXY*>(info.curve->data())) {
        if (std::binary_search(topic_ids.begin(), topic_ids.end(), xy->xSource().topic_id) ||
            std::binary_search(topic_ids.begin(), topic_ids.end(), xy->ySource().topic_id)) {
          xy->onDataCleared();
        }
      } else if (auto* adapter = dynamic_cast<DatastoreCurveAdapter*>(info.curve->data())) {
        if (std::binary_search(topic_ids.begin(), topic_ids.end(), adapter->source().topic_id)) {
          adapter->onDataCleared();
        }
      }
    }
  }
  for (const TopicId topic_id : topic_ids) {
    engine_.evictTopicHistory(topic_id);
  }
}

void TopicDemandController::runCensus(const std::vector<CatalogItem>& items) {
  for (const CatalogItem& item : items) {
    const auto* advertised = asAdvertisedTopic(item);
    if (advertised == nullptr || advertised->classification != sdk::BuiltinObjectType::kNone) {
      continue;  // real entry, or an object-classified placeholder (size-filtered out)
    }
    if (!catalog_.isPerTopicPauseCapable(item.dataset_id)) {
      continue;  // non-demand source / file dataset — census is meaningless there
    }
    if (censused_topics_.count({item.dataset_id, item.topic_name}) > 0) {
      continue;  // previewed once already this session — re-advertise must not re-preview
    }
    requestFieldPreview(item.dataset_id, item.topic_name);
  }
}

void TopicDemandController::requestFieldPreview(DatasetId dataset_id, const QString& topic_name) {
  if (!catalog_.isPerTopicPauseCapable(dataset_id) || previewInFlight(dataset_id, topic_name) ||
      topicHasRealEntry(dataset_id, topic_name)) {
    return;
  }
  const auto active = tracker_.activeTopics(dataset_id);
  if (std::find(active.begin(), active.end(), topic_name) != active.end()) {
    return;  // a real display/pend reference already holds this topic — don't disturb it
  }
  if (static_cast<int>(active_previews_.size()) < kMaxConcurrentPreviews) {
    startPreview(dataset_id, topic_name);
  } else {
    preview_queue_.push_back(PreviewId{dataset_id, topic_name});
  }
}

bool TopicDemandController::previewInFlight(DatasetId dataset_id, const QString& topic_name) const {
  const PreviewId id{dataset_id, topic_name};
  // The queue never exceeds one advertise burst's worth of topics — a linear
  // scan beats maintaining a membership mirror.
  return active_previews_.count(id) > 0 ||
         std::find(preview_queue_.begin(), preview_queue_.end(), id) != preview_queue_.end();
}

bool TopicDemandController::topicHasRealEntry(DatasetId dataset_id, const QString& topic_name) const {
  for (const CatalogItem& item : catalog_.items()) {
    if (item.dataset_id == dataset_id && item.topic_name == topic_name && asAdvertisedTopic(item) == nullptr) {
      return true;
    }
  }
  return false;
}

void TopicDemandController::startPreview(DatasetId dataset_id, const QString& topic_name) {
  tracker_.addReference(dataset_id, topic_name);
  auto* timer = new QTimer(this);
  timer->setSingleShot(true);
  timer->setInterval(preview_timeout_ms_);
  connect(timer, &QTimer::timeout, this, [this, dataset_id, topic_name]() { finishPreview(dataset_id, topic_name); });
  active_previews_.emplace(PreviewId{dataset_id, topic_name}, timer);
  timer->start();
}

void TopicDemandController::finishPreview(DatasetId dataset_id, const QString& topic_name) {
  const PreviewId id{dataset_id, topic_name};
  const auto it = active_previews_.find(id);
  if (it == active_previews_.end()) {
    return;  // promotion and timeout can race — the first one wins, the second no-ops
  }
  it->second->stop();
  it->second->deleteLater();
  active_previews_.erase(it);
  tracker_.removeReference(dataset_id, topic_name);
  censused_topics_.insert(id);
  pumpPreviewQueue();
}

void TopicDemandController::pumpPreviewQueue() {
  while (static_cast<int>(active_previews_.size()) < kMaxConcurrentPreviews && !preview_queue_.empty()) {
    const PreviewId id = preview_queue_.front();
    preview_queue_.pop_front();
    // Re-validate: while queued the topic may have promoted, gained a real
    // reference, lost its dataset's capability, or already started elsewhere.
    if (active_previews_.count(id) > 0 || !catalog_.isPerTopicPauseCapable(id.first) ||
        topicHasRealEntry(id.first, id.second)) {
      continue;
    }
    const auto active = tracker_.activeTopics(id.first);
    if (std::find(active.begin(), active.end(), id.second) != active.end()) {
      continue;
    }
    startPreview(id.first, id.second);
  }
}

void TopicDemandController::onCatalogCleared() {
  for (const auto& [id, timer] : active_previews_) {
    timer->stop();
    timer->deleteLater();
    tracker_.removeReference(id.first, id.second);
  }
  active_previews_.clear();
  preview_queue_.clear();
  censused_topics_.clear();
  preview_promoted_pending_disown_.clear();
}

}  // namespace PJ
