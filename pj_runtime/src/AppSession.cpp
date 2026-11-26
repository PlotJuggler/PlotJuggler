// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/AppSession.h"

#include <QLoggingCategory>
#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveColorRegistry.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/DerivedTopics.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/TopicDemandTracker.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcAppSession, "pj.runtime.app_session")
}  // namespace

CurveColorRegistry& AppSession::curveColorRegistry() const {
  return session_manager_->curveColorRegistry();
}

AppSession::AppSession(QObject* parent) : AppSession(QString{}, parent) {}

AppSession::AppSession(QString extensions_dir, QObject* parent)
    : AppSession(std::move(extensions_dir), DiagnosticSink{}, parent) {}

AppSession::AppSession(QString extensions_dir, DiagnosticSink sink, QObject* parent)
    : AppSession(std::move(extensions_dir), std::move(sink), StaticPluginSet{}, parent) {}

AppSession::AppSession(QString extensions_dir, DiagnosticSink sink, StaticPluginSet static_plugins, QObject* parent)
    : QObject(parent),
      session_manager_(std::make_unique<SessionManager>()),
      playback_engine_(std::make_unique<PlaybackEngine>()),
      catalog_model_(std::make_unique<CatalogModel>(session_manager_.get())),
      topic_demand_tracker_(std::make_unique<TopicDemandTracker>()),
      extension_catalog_(
          std::make_unique<ExtensionCatalogService>(std::move(extensions_dir), sink, std::move(static_plugins))) {
  // A transform replay that fails after its create returned reports here, the
  // same notifications the create error itself would have reached.
  session_manager_->dataProcessorService().setDiagnosticSink(sink);
  // Forget remembered curve colors whenever the catalog empties (data cleared or replaced), so fresh data restarts
  // palette rotation; the registry is SessionManager's, AppSession wires the session-scoped clear. Also collapse the
  // transport to the empty state: resetPlaybackToEmpty re-arms the first-seed snap, so the next load snaps range AND
  // playhead to the new data.
  QObject::connect(catalog_model_.get(), &CatalogModel::cleared, this, [this]() {
    session_manager_->curveColorRegistry().clear();
    resetPlaybackToEmpty();
  });
  // Marker generators read their inputs through the catalog: session state, not panel state (a layout restores
  // generators before any toolbox exists). A key names a series the way the producer and the overlay do
  // (markerSeriesKey), so a generator yields the same markers in the GUI and headless.
  BatchEngine& batch_engine = session_manager_->batchEngine();
  // A marker rule's scheduled run that fails reaches the same notifications.
  batch_engine.setDiagnosticSink(std::move(sink));
  batch_engine.setResolver(
      [this](
          DatasetId dataset, const std::string& key,
          const BatchEngine::Window& window) -> std::optional<BatchEngine::ResolvedSeries> {
        const std::optional<ScalarFieldPayload> scalar = catalog_model_->findScalarForMarkerKey(dataset, key);
        if (!scalar.has_value()) {
          return std::nullopt;
        }
        // A generator reads the whole series, so a transform output still replaying in
        // slices is completed first.
        session_manager_->dataEngine().awaitCompleteSeries(scalar->topic_id);
        const DataReader reader = session_manager_->createReader();
        Expected<SeriesReader> series = reader.series(scalar->topic_id, scalar->column_index);
        if (!series.has_value()) {
          return std::nullopt;
        }
        return readResolvedSeries(*series, window);
      });
  // For `all_datasets` generators: every loaded dataset.
  batch_engine.setDatasetLister([this] { return catalog_model_->datasetIds(); });
  // Marker rules re-run automatically only on a dataset something consumes: a
  // demand-capable stream while a widget displays one of its topics, any other
  // dataset (a file, a stream that sends everything) always.
  batch_engine.setDemandPredicate([this](DatasetId dataset_id) {
    return !catalog_model_->isPerTopicPauseCapable(dataset_id) || topic_demand_tracker_->hasAnyReference(dataset_id);
  });
  // A display reference is the demand; an active-set change also covers a stream that
  // stopped (no longer pause-capable, so demanded from then on).
  QObject::connect(
      topic_demand_tracker_.get(), &TopicDemandTracker::referenceDemandChanged, &batch_engine,
      &BatchEngine::demandChanged);
  QObject::connect(
      topic_demand_tracker_.get(), &TopicDemandTracker::activeTopicsChanged, &batch_engine,
      &BatchEngine::demandChanged);

  // On-demand recipes read scalar series, strings included, through the
  // catalog, follow the playhead while something displays one of their outputs, and are requested where the cursor is
  // parked when they are installed or become displayed; a finding, at its pin, once per install. Connected before the
  // scene docks' own slots, so a request precedes the docks' read of the same tick.
  session_manager_->setScalarSeriesLookup(
      [this](DatasetId dataset, const std::string& name) -> std::optional<std::pair<TopicId, std::size_t>> {
        const std::optional<ScalarFieldPayload> scalar = catalog_model_->findScalarForMarkerKey(dataset, name, true);
        if (!scalar) {
          return std::nullopt;
        }
        return std::pair<TopicId, std::size_t>{scalar->topic_id, scalar->column_index};
      });
  // While the transport follows the live edge (playing, held at the range's end), the live pass requests the recipes
  // of every streaming dataset (SessionManager::notifyIngest): the playhead sits at the newest sample, not at each
  // recipe's inputs'. When it starts following again, the live pass requests them at once, nothing new needed.
  const auto following = [this] { return playback_engine_->isPlaying() && playback_engine_->holdsAtRangeMax(); };
  QObject::connect(playback_engine_.get(), &PlaybackEngine::currentTimeChanged, this, [this, following] {
    const OnDemandService& service = session_manager_->onDemandService();
    const bool live_owns = following();
    for (const std::string& key : service.demandedKeys()) {
      if (!live_owns || !session_manager_->isLive(service.graph().find(key)->dataset)) {
        requestOnDemandAtPlayhead(key);
      }
    }
  });
  QObject::connect(playback_engine_.get(), &PlaybackEngine::playingChanged, this, [this, following] {
    if (!following()) {
      return;
    }
    OnDemandService& service = session_manager_->onDemandService();
    std::set<DatasetId> datasets;
    for (const std::string& key : service.demandedKeys()) {
      datasets.insert(service.graph().find(key)->dataset);
    }
    for (const DatasetId dataset : datasets) {
      if (session_manager_->isLive(dataset)) {
        service.restartLiveEdge(dataset);
      }
    }
  });
  // A nested refill starts over partial data: only the first one's extent is what the timeline showed.
  QObject::connect(session_manager_.get(), &SessionManager::refillStarting, this, [this](DatasetId dataset) {
    if (session_manager_->refillOpen(dataset)) {
      return;
    }
    if (const auto range = datasetRawTimeRange(dataset)) {
      refill_held_raw_ranges_[dataset] = *range;
    } else {
      refill_held_raw_ranges_.erase(dataset);
    }
  });
  QObject::connect(session_manager_.get(), &SessionManager::refillClosed, this, [this](DatasetId dataset) {
    refill_held_raw_ranges_.erase(dataset);
    recomputeRange();
  });
  // An offset change at a parked playhead moves the raw time the demanded recipes are requested at.
  QObject::connect(
      session_manager_.get(), qOverload<DatasetId>(&SessionManager::displayOffsetChanged), this,
      [this](DatasetId dataset) {
        for (const std::string& key : session_manager_->onDemandService().demandedKeys(dataset)) {
          requestOnDemandAtPlayhead(key);
        }
      });
  QObject::connect(session_manager_.get(), qOverload<>(&SessionManager::displayOffsetChanged), this, [this] {
    for (const std::string& key : session_manager_->onDemandService().demandedKeys()) {
      requestOnDemandAtPlayhead(key);
    }
  });
  QObject::connect(session_manager_.get(), &SessionManager::onDemandRecipeInstalled, this, [this](const QString& key) {
    const OnDemandService& service = session_manager_->onDemandService();
    const std::string name = key.toStdString();
    const RecipeGraph::Node* node = service.graph().find(name);
    if (node != nullptr && (node->recipe.pinned_t || service.isDemanded(name))) {
      requestOnDemandAtPlayhead(name);
    }
    if (node != nullptr) {
      demandOnDemandRawInputs(node->dataset);
    }
  });
  // A removal, and a re-binding on another dataset (a merge), retire topics: the tiers they left are stale.
  QObject::connect(
      session_manager_.get(), &SessionManager::objectTopicsChanged, this, [this] { refreshOnDemandRawInputs(); });
  QObject::connect(
      topic_demand_tracker_.get(), &TopicDemandTracker::activeTopicsChanged, this,
      [this](DatasetId dataset, const std::vector<QString>& active_topics) {
        OnDemandService& service = session_manager_->onDemandService();
        const std::vector<std::string> before = service.demandedKeys(dataset);
        std::vector<std::string> names;
        names.reserve(active_topics.size());
        for (const QString& topic : active_topics) {
          names.push_back(topic.toStdString());
        }
        service.setDemand(dataset, std::move(names));
        const std::vector<std::string> after = service.demandedKeys(dataset);
        for (const std::string& key : after) {
          if (!std::ranges::binary_search(before, key)) {
            requestOnDemandAtPlayhead(key);
          }
        }
        for (const std::string& key : before) {
          if (!std::ranges::binary_search(after, key)) {
            service.withdraw(key);
          }
        }
        demandOnDemandRawInputs(dataset);  // its own change comes back here once, with the same demanded keys
      });
}

void AppSession::demandOnDemandRawInputs(DatasetId dataset) {
  std::vector<QString> topics;
  const DataReader reader = session_manager_->createReader();
  for (const RecipeGraph::Binding& input : session_manager_->onDemandService().displayedRawInputs(dataset)) {
    if (input.object_type != sdk::BuiltinObjectType::kNone) {
      topics.push_back(QString::fromStdString(input.name));
    } else if (const auto scalar = catalog_model_->findScalarForMarkerKey(dataset, input.name, true)) {
      if (const auto metadata = reader.getMetadata(scalar->topic_id)) {
        topics.push_back(QString::fromStdString(metadata->name));
      }
    }
  }
  if (topics.empty()) {
    raw_input_datasets_.erase(dataset);
  } else {
    raw_input_datasets_.insert(dataset);
  }
  topic_demand_tracker_->setDerivedInputTopics(dataset, topics);
}

void AppSession::refreshOnDemandRawInputs() {
  for (const DatasetId dataset : std::set<DatasetId>(raw_input_datasets_)) {
    demandOnDemandRawInputs(dataset);
  }
}

void AppSession::requestOnDemandAtPlayhead(const std::string& key) {
  OnDemandService& service = session_manager_->onDemandService();
  if (const RecipeGraph::Node* node = service.graph().find(key)) {
    const DisplayOffset offset = session_manager_->displayOffset(node->dataset);
    service.requestAt(key, displaySecondsToRaw(playback_engine_->currentTime(), offset));
  }
}

AppSession::~AppSession() {
  topic_demand_tracker_.reset();
  catalog_model_.reset();
  session_manager_.reset();
  playback_engine_.reset();
  extension_catalog_.reset();
}

void AppSession::forEachVisibleRawRange(
    const std::function<void(DatasetId dataset_id, Timestamp raw_min, Timestamp raw_max)>& visit) const {
  const DataReader reader = session_manager_->createReader();
  const ObjectStore& object_store = session_manager_->objectStore();

  // Bounds come from the CATALOG-VISIBLE items per topic: the engine retains removed/trashed topics' data (append-only
  // tombstones), and a hidden topic must not stretch the timeline even while sibling topics keep its dataset visible.
  // Multi-field topics surface one catalog item per field; each underlying topic is bounded once.
  std::unordered_set<TopicId> seen_topics;
  std::unordered_set<uint32_t> seen_object_topics;
  std::unordered_map<DatasetId, bool> static_references;
  // Unordered walk: every caller folds the visits into a min/max or a set, and this runs on every ingest notification,
  // where items()' copy and collated sort of the whole catalog takes about a second at ~100k series.
  catalog_model_->forEachItem([&](const CatalogItem& item) {
    auto [reference, inserted] = static_references.try_emplace(item.dataset_id, false);
    if (inserted) {
      reference->second = session_manager_->isStaticReferenceDataset(item.dataset_id);
    }
    if (reference->second) {
      return;
    }
    Timestamp raw_min = 0;
    Timestamp raw_max = 0;
    if (const ScalarFieldPayload* scalar = asScalarField(item)) {
      if (!seen_topics.insert(scalar->topic_id).second) {
        return;
      }
      const auto metadata = reader.getMetadata(scalar->topic_id);
      if (!metadata.has_value() || metadata->total_row_count == 0) {
        return;
      }
      raw_min = metadata->time_range_min;
      raw_max = metadata->time_range_max;
    } else if (const ObjectTopicPayload* object = asObjectTopic(item)) {
      if (!seen_object_topics.insert(object->object_topic_id.id).second) {
        return;
      }
      // A marker set or an on-demand result is host-published off the data clock (a finding sits at
      // kFindingTimestamp): it never stretches a range.
      if (object_store.entryCount(object->object_topic_id) == 0 ||
          isOffClockTopic(object_store.descriptor(object->object_topic_id))) {
        return;
      }
      std::tie(raw_min, raw_max) = object_store.timeRange(object->object_topic_id);
    } else {
      return;
    }
    visit(item.dataset_id, raw_min, raw_max);
  });
}

std::optional<PJ::Range<PJ::Timestamp>> AppSession::datasetRawTimeRange(DatasetId dataset_id) const {
  // RAW (pre-offset) union over THIS dataset's catalog-visible topics; the caller applies the display offset.
  std::optional<Timestamp> raw_min;
  std::optional<Timestamp> raw_max;
  forEachVisibleRawRange([&](DatasetId item_dataset, Timestamp topic_min, Timestamp topic_max) {
    if (item_dataset != dataset_id) {
      return;
    }
    raw_min = raw_min ? std::min(*raw_min, topic_min) : topic_min;
    raw_max = raw_max ? std::max(*raw_max, topic_max) : topic_max;
  });
  if (!raw_min) {
    return std::nullopt;  // no time-bearing data for this dataset
  }
  return PJ::Range<Timestamp>{*raw_min, *raw_max};
}

std::optional<DisplaySeconds> AppSession::recomputeRange() {
  // While following a live stream (streaming + playing), scope the range to that dataset's tip ONLY, as the live-ingest
  // path does: unioning in far-away file datasets would push range_max past the live edge and, the cursor being pinned
  // to range_max (onTick), jolt the needle off the tip and back. When paused, fall through to the full union so the
  // user can scrub against ALL loaded data.
  if (active_streaming_dataset_id_ != 0 && playback_engine_->isPlaying()) {
    if (const auto range = session_manager_->datasetDisplayRange(active_streaming_dataset_id_); range.has_value()) {
      playback_engine_->setRange(*range);
      return range->min;
    }
    // Live-follow without a first sample yet: leave the transport alone rather than collapse a running stream to the
    // empty state below.
    return std::nullopt;
  }
  // Union the bounds in DISPLAY-relative seconds, each item converted with its own dataset's offset, so the axis
  // matches what the plots render (display_time = raw_time - offset). Range only: never currentTime or
  // seeded_datasets_. setRange re-clamps the playhead; an in-range scrub position survives.
  std::optional<DisplaySeconds> new_min;
  std::optional<DisplaySeconds> new_max;
  forEachVisibleRawRange([&](DatasetId dataset_id, Timestamp raw_min, Timestamp raw_max) {
    const DisplayOffset offset = session_manager_->displayOffset(dataset_id);
    const DisplaySeconds ds_min = rawToDisplaySecondsAtOrAfter(raw_min, offset);
    const DisplaySeconds ds_max = rawToDisplaySecondsAtOrAfter(raw_max, offset);
    new_min = new_min ? std::min(*new_min, ds_min) : ds_min;
    new_max = new_max ? std::max(*new_max, ds_max) : ds_max;
  });
  // A reloading dataset is empty until its refill closes: without its held extent the range would shrink to the other
  // datasets and clamp the playhead there for good. One removed or merged away meanwhile no longer counts.
  const std::vector<DatasetId> shown =
      refill_held_raw_ranges_.empty() ? std::vector<DatasetId>{} : catalog_model_->datasetIds();
  for (const auto& [dataset_id, raw] : refill_held_raw_ranges_) {
    if (!session_manager_->refillOpen(dataset_id) || std::find(shown.begin(), shown.end(), dataset_id) == shown.end()) {
      continue;
    }
    const DisplayOffset offset = session_manager_->displayOffset(dataset_id);
    const DisplaySeconds ds_min = rawToDisplaySecondsAtOrAfter(raw.min, offset);
    const DisplaySeconds ds_max = rawToDisplaySecondsAtOrAfter(raw.max, offset);
    new_min = new_min ? std::min(*new_min, ds_min) : ds_min;
    new_max = new_max ? std::max(*new_max, ds_max) : ds_max;
  }
  if (!new_min) {
    // Static references alone have no transport. Otherwise keep the range across a transient empty while a replacing
    // reload detaches its chunks, so playback does not pause or flicker mid-load.
    // An empty catalog counts as references only.
    bool only_references = true;
    catalog_model_->forEachItem([&](const CatalogItem& item) {
      only_references = only_references && session_manager_->isStaticReferenceDataset(item.dataset_id);
    });
    if (only_references) {
      resetPlaybackToEmpty();
    }
    return std::nullopt;
  }
  playback_engine_->setRange(DisplayRange{*new_min, *new_max});
  return new_min;
}

void AppSession::resetPlaybackToEmpty() {
  // Order: stop the clock and drop live hold BEFORE zeroing the range, so no
  // in-flight tick clamps against a half-reset engine. The empty [0,0] range is
  // the signal the TimelineWidget uses to disable the transport.
  playback_engine_->pause();
  playback_engine_->setHoldAtRangeMax(false);
  playback_engine_->setRangeAndCurrentTime(DisplayRange{DisplaySeconds{0.0}, DisplaySeconds{0.0}}, DisplaySeconds{0.0});
  seeded_datasets_.clear();
}

std::optional<AppSession::MergePlan> AppSession::planMerge(const std::vector<DatasetId>& selected) const {
  if (selected.size() < 2) {
    return std::nullopt;
  }

  // Anchor = the dataset whose DISPLAYED start is earliest: min(raw_min - displayOffset). Datasets without
  // time-bearing data cannot be positioned and are ignored.
  std::optional<DatasetId> anchor;
  std::optional<DisplaySeconds> anchor_display_min;
  for (const DatasetId id : selected) {
    const auto raw = datasetRawTimeRange(id);
    if (!raw.has_value()) {
      continue;
    }
    // Compared through the display seam (raw - offset), matching recomputeRange.
    const DisplaySeconds display_min = rawToDisplaySeconds(raw->min, session_manager_->displayOffset(id));
    if (!anchor.has_value() || display_min < *anchor_display_min) {
      anchor = id;
      anchor_display_min = display_min;
    }
  }
  if (!anchor.has_value()) {
    return std::nullopt;  // none of the selected datasets carry data
  }

  // Every other selected dataset shifts into the anchor's raw frame by the relative display offset, so it lands where
  // the user arranged it while the merged dataset keeps the anchor's absolute clock. The shift is a Duration
  // difference of the two offsets, lowered to int64 ns only at the engine seam.
  const Duration anchor_offset = session_manager_->displayOffset(*anchor).value;
  std::vector<DatasetMergeSource> sources;
  for (const DatasetId id : selected) {
    if (id == *anchor) {
      continue;
    }
    const Duration relative_shift = anchor_offset - session_manager_->displayOffset(id).value;
    sources.push_back(
        DatasetMergeSource{.dataset_id = id, .raw_shift_ns = static_cast<Timestamp>(relative_shift.count())});
  }
  if (sources.empty()) {
    return std::nullopt;
  }

  return MergePlan{.anchor = *anchor, .sources = std::move(sources)};
}

std::vector<ObjectMergeConflict> AppSession::objectMergeConflicts(const std::vector<DatasetId>& selected) const {
  const std::optional<MergePlan> plan = planMerge(selected);
  if (!plan.has_value()) {
    return {};
  }

  const ObjectStore& object_store = session_manager_->objectStore();
  // Mirror ObjectStore::mergeDatasets' grouping so the gate catches EVERY type clash: the established type for a name
  // is the anchor's or, when the anchor lacks it, the FIRST source (in plan order) contributing it, which becomes the
  // fuse destination. A later contributor with a different canonical type conflicts, whether with the anchor or an
  // earlier source-only contributor. The payload format must match too: the destination topic's parser decodes every
  // fused entry.
  struct Kind {
    sdk::BuiltinObjectType type{};
    std::string format;
    // Merged marker sets keep the anchor's description, so it must match too (the description only: direct writes may
    // carry custom metadata fields).
    std::optional<MarkerTopicInfo> marker;
  };
  const auto kindOf = [](const ObjectTopicDescriptor& descriptor) {
    return Kind{
        objectTypeFromMetadata(descriptor.metadata_json), objectPayloadFormatFromMetadata(descriptor.metadata_json),
        markerTopicInfo(descriptor.metadata_json)};
  };
  std::unordered_map<std::string, Kind> kind_by_name;
  for (const ObjectTopicId object_topic_id : object_store.listTopics(plan->anchor)) {
    const ObjectTopicDescriptor descriptor = object_store.descriptor(object_topic_id);
    kind_by_name.emplace(descriptor.topic_name, kindOf(descriptor));
  }

  std::vector<ObjectMergeConflict> conflicts;
  for (const DatasetMergeSource& source : plan->sources) {
    for (const ObjectTopicId object_topic_id : object_store.listTopics(source.dataset_id)) {
      const ObjectTopicDescriptor descriptor = object_store.descriptor(object_topic_id);
      Kind source_kind = kindOf(descriptor);
      const auto [it, inserted] = kind_by_name.emplace(descriptor.topic_name, source_kind);
      if (inserted || (source_kind.type == it->second.type && source_kind.format == it->second.format &&
                       source_kind.marker == it->second.marker)) {
        continue;  // first contributor for this name (sets the destination kind), or a match.
      }
      conflicts.push_back(
          ObjectMergeConflict{
              .topic_name = descriptor.topic_name,
              .source_dataset_id = source.dataset_id,
              .anchor_type = it->second.type,
              .source_type = source_kind.type,
              .anchor_format = it->second.format,
              .source_format = std::move(source_kind.format),
              .marker_description_differs = source_kind.marker != it->second.marker,
          });
    }
  }
  return conflicts;
}

DatasetId AppSession::mergeDatasets(const std::vector<DatasetId>& selected) {
  std::optional<MergePlan> plan = planMerge(selected);
  if (!plan.has_value()) {
    return 0;
  }

  if (active_streaming_dataset_id_ != 0 &&
      std::find(selected.begin(), selected.end(), active_streaming_dataset_id_) != selected.end()) {
    qCWarning(lcAppSession) << "mergeDatasets: refused: selected dataset is actively streaming"
                            << active_streaming_dataset_id_;
    return 0;
  }
  if (!objectMergeConflicts(selected).empty()) {
    qCWarning(lcAppSession) << "mergeDatasets: refused: object-topic canonical-type conflict";
    return 0;
  }

  // Engine mutation, adapter invalidation and re-index live in SessionManager. nullopt means the engine rejected the
  // merge and left the store untouched: bail BEFORE mutating the catalog, or the sources would vanish from the UI
  // while their data still lives in the store.
  if (!session_manager_->mergeDatasets(plan->anchor, plan->sources).has_value()) {
    return 0;
  }

  // Catalog: relabel the anchor "<name>_merged" and drop the consumed pieces.
  if (const std::optional<QString> anchor_label = catalog_model_->datasetLabel(plan->anchor)) {
    catalog_model_->setDatasetDisplayName(plan->anchor, *anchor_label + u"_merged"_s);
  }
  for (const auto& source : plan->sources) {
    if (seeded_datasets_.contains(source.dataset_id)) {
      seeded_datasets_.insert(plan->anchor);
    }
    catalog_model_->removeDataset(source.dataset_id);
  }
  catalog_model_->rebuildFromDatastore();

  // The merged extent changed; refresh the playback range (range only).
  recomputeRange();

  QList<DatasetId> consumed;
  for (const DatasetMergeSource& source : plan->sources) {
    consumed.push_back(source.dataset_id);
  }
  emit datasetsMerged(plan->anchor, consumed);
  return plan->anchor;
}

bool AppSession::seedPlaybackFromSession() {
  // Range and the first-seed display-min both come from the one scan in recomputeRange (it grows on an additional
  // file, shrinks on a removed/trashed/shorter reload). recomputeRange never touches currentTime; the first-seed snap
  // below is the only place that does.
  const std::optional<DisplaySeconds> new_min = recomputeRange();
  if (!new_min) {
    // No visible data: recomputeRange already collapsed the transport to the empty state. Nothing to snap.
    return false;
  }

  const std::vector<DatasetId> shown = catalog_model_->datasetIds();
  // A followed stream joins the seed set when it starts, before its first sample: it holds the playhead only once it
  // has data, so a stream that never received any cannot keep stale seeding alive.
  const bool seed_left = std::any_of(shown.begin(), shown.end(), [this](DatasetId id) {
    return seeded_datasets_.contains(id) &&
           (id != active_streaming_dataset_id_ || session_manager_->datasetDisplayRange(id).has_value());
  });
  if (!seed_left) {
    playback_engine_->setCurrentTime(*new_min);
    seeded_datasets_.clear();
    forEachVisibleRawRange([this](DatasetId dataset_id, Timestamp, Timestamp) { seeded_datasets_.insert(dataset_id); });
  }
  return true;
}

bool AppSession::focusPlaybackOnDatasets(const std::vector<DatasetId>& datasets) {
  if (datasets.empty()) {
    return false;
  }
  const DataReader reader = session_manager_->createReader();
  const ObjectStore& object_store = session_manager_->objectStore();

  // Scalar series first: receive-time stamped, they ARE the user-visible window of an import. Object topics (tf,
  // markers, pointclouds) often carry payload-embedded stamps that predate the import (tf_static = node start, latched
  // markers = creation time), so they define the range only when the import has no scalar data at all. Bounds are in
  // DISPLAY-relative seconds, each item converted with its dataset's own offset, so the focused range matches the
  // rendered axis (the boundary contract of seedPlaybackFromSession).
  std::optional<DisplaySeconds> scalar_min;
  std::optional<DisplaySeconds> scalar_max;
  std::optional<DisplaySeconds> object_min;
  std::optional<DisplaySeconds> object_max;
  std::set<DatasetId> bounded;

  for (const DatasetId dataset_id : datasets) {
    if (session_manager_->isStaticReferenceDataset(dataset_id)) {
      continue;
    }
    const DisplayOffset offset = session_manager_->displayOffset(dataset_id);
    for (const TopicId topic_id : reader.listTopics(dataset_id)) {
      const auto metadata = reader.getMetadata(topic_id);
      if (!metadata.has_value() || metadata->total_row_count == 0) {
        continue;
      }
      const DisplaySeconds ds_min = rawToDisplaySecondsAtOrAfter(metadata->time_range_min, offset);
      const DisplaySeconds ds_max = rawToDisplaySecondsAtOrAfter(metadata->time_range_max, offset);
      scalar_min = scalar_min ? std::min(*scalar_min, ds_min) : ds_min;
      scalar_max = scalar_max ? std::max(*scalar_max, ds_max) : ds_max;
      bounded.insert(dataset_id);
    }
    for (const ObjectTopicId object_topic_id : object_store.listTopics(dataset_id)) {
      if (isOffClockTopic(object_store.descriptor(object_topic_id))) {
        continue;
      }
      if (object_store.entryCount(object_topic_id) == 0) {
        continue;
      }
      const auto [o_min, o_max] = object_store.timeRange(object_topic_id);
      const DisplaySeconds ds_min = rawToDisplaySecondsAtOrAfter(o_min, offset);
      const DisplaySeconds ds_max = rawToDisplaySecondsAtOrAfter(o_max, offset);
      object_min = object_min ? std::min(*object_min, ds_min) : ds_min;
      object_max = object_max ? std::max(*object_max, ds_max) : ds_max;
      bounded.insert(dataset_id);
    }
  }

  const std::optional<DisplaySeconds> t_min = scalar_min ? scalar_min : object_min;
  const std::optional<DisplaySeconds> t_max = scalar_min ? scalar_max : object_max;
  if (!t_min) {
    return false;
  }
  // Snap the cursor to the start on the first focus, or when a focus on new datasets finds it outside their range. A
  // progressive import re-focuses on every notify as it grows, and between notifies the range is the union of all data:
  // a cursor scrubbed past the import's tip is then only clamped, never yanked back to the start. The cursor is read
  // before setRange, which clamps it into the range.
  const DisplaySeconds cursor = playback_engine_->currentTime();
  const bool refocus = std::ranges::all_of(bounded, [this](DatasetId id) { return seeded_datasets_.contains(id); });
  if (seeded_datasets_.empty() || (!refocus && (cursor < *t_min || cursor > *t_max))) {
    playback_engine_->setRangeAndCurrentTime(DisplayRange{*t_min, *t_max}, *t_min);
    seeded_datasets_.clear();
  } else {
    playback_engine_->setRange(DisplayRange{*t_min, *t_max});
  }
  seeded_datasets_.insert(bounded.begin(), bounded.end());
  return true;
}

}  // namespace PJ
