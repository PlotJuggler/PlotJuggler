// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/AppSession.h"

#include <QLoggingCategory>
#include <algorithm>
#include <functional>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveColorRegistry.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/MarkerTopics.h"
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
          std::make_unique<ExtensionCatalogService>(
              std::move(extensions_dir), std::move(sink), std::move(static_plugins))) {
  // Forget remembered curve colors whenever the catalog empties (data cleared
  // or replaced), matching PJ3's per-PlotData COLOR_HINT lifetime so reopening
  // fresh data restarts palette rotation from the first color. The registry is
  // owned by SessionManager; AppSession just wires its session-scoped clear.
  // Collapse the transport to the empty state on a full clear: stop playback and
  // reset the range/cursor (resetPlaybackToEmpty also re-arms the first-seed
  // snap, so the next load starts fresh — range AND playhead snap to new data).
  QObject::connect(catalog_model_.get(), &CatalogModel::cleared, this, [this]() {
    session_manager_->curveColorRegistry().clear();
    resetPlaybackToEmpty();
  });
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

  // Bounds come from the CATALOG-VISIBLE items at per-topic granularity: the
  // engine retains removed/trashed topics' data (append-only tombstones), and
  // a hidden topic must not stretch the timeline even while sibling topics
  // keep its dataset visible.
  //
  // Multi-field topics surface one catalog item per field; bound each
  // underlying topic once.
  std::unordered_set<TopicId> seen_topics;
  std::unordered_set<uint32_t> seen_object_topics;
  for (const CatalogItem& item : catalog_model_->items()) {
    Timestamp raw_min = 0;
    Timestamp raw_max = 0;
    if (const ScalarFieldPayload* scalar = asScalarField(item)) {
      if (!seen_topics.insert(scalar->topic_id).second) {
        continue;
      }
      const auto metadata = reader.getMetadata(scalar->topic_id);
      if (!metadata.has_value() || metadata->total_row_count == 0) {
        continue;
      }
      raw_min = metadata->time_range_min;
      raw_max = metadata->time_range_max;
    } else if (const ObjectTopicPayload* object = asObjectTopic(item)) {
      if (!seen_object_topics.insert(object->object_topic_id.id).second) {
        continue;
      }
      if (object_store.entryCount(object->object_topic_id) == 0) {
        continue;
      }
      std::tie(raw_min, raw_max) = object_store.timeRange(object->object_topic_id);
    } else {
      continue;
    }
    visit(item.dataset_id, raw_min, raw_max);
  }
}

std::optional<PJ::Range<PJ::Timestamp>> AppSession::datasetRawTimeRange(DatasetId dataset_id) const {
  // RAW (pre-offset) union over THIS dataset's catalog-visible topics. The
  // caller applies the display offset; the Timeline bars want raw bounds.
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
  // While actively following a live stream (streaming + playing), scope the range to
  // that dataset's tip ONLY — exactly as the live-ingest path does. Unioning in
  // far-away file datasets would push range_max past the live edge; the cursor is
  // pinned to range_max (onTick), so that would jolt the handle/needle off the tip and
  // back (the jitter). When paused, fall through to the full union so the user can
  // scrub/align against ALL loaded data.
  if (active_streaming_dataset_id_ != 0 && playback_engine_->isPlaying()) {
    if (const auto range = session_manager_->datasetDisplayRange(active_streaming_dataset_id_); range.has_value()) {
      playback_engine_->setRange(*range);
      return range->min;
    }
    // Live-follow that hasn't produced its first sample yet: leave the transport
    // alone rather than collapsing a running stream to the empty state below.
    return std::nullopt;
  }
  // Union the bounds in DISPLAY-relative seconds, converting each item with
  // its dataset's OWN offset, so the playback axis matches what the plots
  // render (display_time = raw_time - offset) rather than the absolute epoch.
  // Range only — never currentTime, never playback_seeded_ — for the live
  // Timeline drag path. setRange re-clamps the playhead; an in-range scrub
  // position is preserved.
  std::optional<DisplaySeconds> new_min;
  std::optional<DisplaySeconds> new_max;
  forEachVisibleRawRange([&](DatasetId dataset_id, Timestamp raw_min, Timestamp raw_max) {
    const DisplayOffset offset = session_manager_->displayOffset(dataset_id);
    const DisplaySeconds ds_min = rawToDisplaySeconds(raw_min, offset);
    const DisplaySeconds ds_max = rawToDisplaySeconds(raw_max, offset);
    new_min = new_min ? std::min(*new_min, ds_min) : ds_min;
    new_max = new_max ? std::max(*new_max, ds_max) : ds_max;
  });
  if (!new_min) {
    // No time-bearing rows. Distinguish a GENUINE empty (the catalog has no
    // items — last dataset removed / full clear) from a TRANSIENT one (a
    // replacing reload detaches a dataset's chunks, so its still-visible topics
    // momentarily hold 0 rows): only collapse the transport when the catalog is
    // truly empty, so a reload doesn't flicker the range to [0,0] and pause
    // mid-load.
    if (catalog_model_->isEmpty()) {
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
  playback_seeded_ = false;
}

std::optional<AppSession::MergePlan> AppSession::planMerge(const std::vector<DatasetId>& selected) const {
  if (selected.size() < 2) {
    return std::nullopt;
  }

  // Anchor = the dataset whose DISPLAYED start is earliest (leftmost on the
  // timeline): min(raw_min - displayOffset). Datasets with no time-bearing data
  // can't be positioned, so they're ignored for the anchor choice.
  std::optional<DatasetId> anchor;
  std::optional<DisplaySeconds> anchor_display_min;
  for (const DatasetId id : selected) {
    const auto raw = datasetRawTimeRange(id);
    if (!raw.has_value()) {
      continue;
    }
    // Compare displayed starts through the canonical display seam (raw - offset),
    // matching recomputeRange rather than hand-subtracting bare ns.
    const DisplaySeconds display_min = rawToDisplaySeconds(raw->min, session_manager_->displayOffset(id));
    if (!anchor.has_value() || display_min < *anchor_display_min) {
      anchor = id;
      anchor_display_min = display_min;
    }
  }
  if (!anchor.has_value()) {
    return std::nullopt;  // none of the selected datasets carry data
  }

  // Every other selected dataset shifts into the anchor's raw frame by the
  // relative display offset, so it lands where the user arranged it while the
  // merged dataset keeps the anchor's absolute clock. The shift is a Duration
  // difference of the two offsets; lower it to int64 ns only at the engine seam.
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
  // Mirror ObjectStore::mergeDatasets' grouping so the gate catches EVERY type clash, not just
  // source-vs-anchor: the established type for a name is the anchor's, or — when the anchor lacks it —
  // the FIRST source (in plan order) to contribute that name, which becomes the fuse destination. A
  // later contributor with the same name but a different canonical type is the conflict, whether it
  // disagrees with the anchor or with an earlier source-only contributor. The payload format must
  // match as well: the fused entries are all decoded by the destination topic's parser.
  struct Kind {
    sdk::BuiltinObjectType type{};
    std::string format;
  };
  const auto kindOf = [](const ObjectTopicDescriptor& descriptor) {
    return Kind{
        objectTypeFromMetadata(descriptor.metadata_json), objectPayloadFormatFromMetadata(descriptor.metadata_json)};
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
      if (inserted || (source_kind.type == it->second.type && source_kind.format == it->second.format)) {
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

  // Engine mutation + adapter invalidation + re-index live in SessionManager.
  // nullopt means the engine rejected the merge and left the store untouched —
  // bail BEFORE mutating the catalog, or the sources would vanish from the UI
  // while their data still lives in the store (catalog/store desync).
  if (!session_manager_->mergeDatasets(plan->anchor, plan->sources).has_value()) {
    return 0;
  }

  // Catalog: relabel the anchor "<name>_merged" and drop the consumed pieces.
  QString anchor_label;
  for (const auto& [id, label] : catalog_model_->datasets()) {
    if (id == plan->anchor) {
      anchor_label = label;
      break;
    }
  }
  if (!anchor_label.isEmpty()) {
    catalog_model_->setDatasetDisplayName(plan->anchor, anchor_label + u"_merged"_s);
  }
  for (const auto& source : plan->sources) {
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
  // The range update AND the display-min for the first-seed snap come from the
  // single shared scan in recomputeRange (it can grow on an additional file or
  // shrink on a removed/trashed/shorter reload) — no second forEachVisibleRawRange
  // pass to keep in lockstep. recomputeRange never touches currentTime; the
  // first-seed snap below is the only place that does.
  const std::optional<DisplaySeconds> new_min = recomputeRange();
  if (!new_min) {
    // No visible data: recomputeRange already collapsed the transport to the
    // empty state (paused, [0,0], cursor 0). Nothing left to snap.
    return false;
  }

  if (!playback_seeded_) {
    // First load (or first after the catalog emptied): also snap currentTime to
    // the data minimum so the user lands at the start of the data.
    playback_engine_->setCurrentTime(*new_min);
    playback_seeded_ = true;
  }
  return true;
}

bool AppSession::focusPlaybackOnDatasets(const std::vector<DatasetId>& datasets) {
  if (datasets.empty()) {
    return false;
  }
  const DataReader reader = session_manager_->createReader();
  const ObjectStore& object_store = session_manager_->objectStore();

  // Scalar series first: receive-time stamped, they ARE the user-visible
  // window of an import. Object topics (tf, markers, pointclouds) often carry
  // payload-embedded stamps that legitimately predate the import window
  // (tf_static = node start, latched markers = creation time), so they only
  // define the range when the import contains no scalar data at all. Bounds
  // are tracked in DISPLAY-relative seconds, converting each item with its
  // dataset's own offset (display_time = raw_time - offset) so the focused
  // range matches the rendered axis — same boundary contract as
  // seedPlaybackFromSession.
  std::optional<DisplaySeconds> scalar_min;
  std::optional<DisplaySeconds> scalar_max;
  std::optional<DisplaySeconds> object_min;
  std::optional<DisplaySeconds> object_max;

  for (const DatasetId dataset_id : datasets) {
    const DisplayOffset offset = session_manager_->displayOffset(dataset_id);
    for (const TopicId topic_id : reader.listTopics(dataset_id)) {
      const auto metadata = reader.getMetadata(topic_id);
      if (!metadata.has_value() || metadata->total_row_count == 0) {
        continue;
      }
      const DisplaySeconds ds_min = rawToDisplaySeconds(metadata->time_range_min, offset);
      const DisplaySeconds ds_max = rawToDisplaySeconds(metadata->time_range_max, offset);
      scalar_min = scalar_min ? std::min(*scalar_min, ds_min) : ds_min;
      scalar_max = scalar_max ? std::max(*scalar_max, ds_max) : ds_max;
    }
    for (const ObjectTopicId object_topic_id : object_store.listTopics(dataset_id)) {
      // Marker snapshots sit at a sentinel timestamp (0), off the data clock;
      // folding them into the playback focus would stretch it back to the epoch.
      if (isMarkerObjectTopic(object_store.descriptor(object_topic_id).topic_name)) {
        continue;
      }
      if (object_store.entryCount(object_topic_id) == 0) {
        continue;
      }
      const auto [o_min, o_max] = object_store.timeRange(object_topic_id);
      const DisplaySeconds ds_min = rawToDisplaySeconds(o_min, offset);
      const DisplaySeconds ds_max = rawToDisplaySeconds(o_max, offset);
      object_min = object_min ? std::min(*object_min, ds_min) : ds_min;
      object_max = object_max ? std::max(*object_max, ds_max) : ds_max;
    }
  }

  const std::optional<DisplaySeconds> t_min = scalar_min ? scalar_min : object_min;
  const std::optional<DisplaySeconds> t_max = scalar_min ? scalar_max : object_max;
  if (!t_min) {
    return false;
  }
  playback_engine_->setRange(DisplayRange{*t_min, *t_max});
  // Snap the cursor only on the first focus or when it fell outside the new
  // range. A progressive import re-focuses on every notify as it grows — the
  // range must follow, but yanking a cursor the user scrubbed/played back to
  // the import's start on each tick would fight them.
  const DisplaySeconds cursor = playback_engine_->currentTime();
  if (!playback_seeded_ || cursor < *t_min || cursor > *t_max) {
    playback_engine_->setCurrentTime(*t_min);
  }
  playback_seeded_ = true;
  return true;
}

}  // namespace PJ
