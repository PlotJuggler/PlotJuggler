// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/SessionManager.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QMetaType>
#include <QString>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/MarkerService.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_scripting/filter_catalogue.h"
#include "pj_scripting/script_engine.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcSession, "pj.runtime.session")
// Off by default; enable with QT_LOGGING_RULES="pj.runtime.ingest.cancel=true"
// to time a stop from click to terminal.
Q_LOGGING_CATEGORY(lcCancel, "pj.runtime.ingest.cancel", QtWarningMsg)

// How long a producer may take to honour a stop before we say so.
constexpr int kStopWatchdogMs = 2000;

// Topic names of the `changed` topics — passed to MarkerService::recomputeForChangedInputs
// so only generators whose input series key ("topic/field") starts with a changed topic
// re-run, instead of re-running every generator on every commit. Accepts std::vector or
// QVector<TopicId>.
template <typename Container>
std::vector<std::string> changedTopicNames(const DataReader& reader, const Container& changed) {
  std::vector<std::string> names;
  for (const TopicId id : changed) {
    if (const std::optional<TopicMetadata> md = reader.getMetadata(id); md.has_value()) {
      names.push_back(md->name);
    }
  }
  return names;
}
}  // namespace

QString SessionManager::normalizedSourcePath(const QString& path) {
  if (path.isEmpty()) {
    return {};
  }
  // Non-file source identities (for example a browser upload lease) are
  // already logical, session-scoped identifiers. Treating them as QFileInfo
  // paths would prepend the process working directory and destroy identity.
  if (path.contains(u"://"_s)) {
    return path;
  }
  const QFileInfo info(path);
  const QString canonical = info.canonicalFilePath();
  return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

// Session-wide cap on ingest-seeded resident object payloads. Large enough to
// cover the live-edge window a scene chases during a progressive load; small
// against the app's overall footprint.
constexpr size_t kResidentPayloadPoolBytes = 256ULL * 1024 * 1024;

SessionManager::SessionManager(QObject* parent)
    : QObject(parent), ingest_taps_(std::make_shared<ObjectIngestTapRegistry>()) {
  // Register types for queued signal connections.
  static const auto dummy_ingest_token = qRegisterMetaType<PJ::IngestToken>("PJ::IngestToken");
  static const auto dummy_ingest_outcome = qRegisterMetaType<PJ::IngestOutcome>("PJ::IngestOutcome");
  (void)dummy_ingest_token;
  (void)dummy_ingest_outcome;

  // Bounded resident window for ingest-seeded object payloads: live-edge object
  // pulls during/after a file load read the bytes the ingest already fetched,
  // instead of re-fetching (for MCAP: re-decompressing a chunk) from the source.
  resident_payload_pool_ = std::make_shared<ResidentPayloadPool>(kResidentPayloadPoolBytes);
  object_store_.setResidentPayloadPool(resident_payload_pool_);

  // data_engine_ is already alive (member init precedes the ctor body), so the
  // processor service can bind its DerivedEngine to it.
  processor_service_ = std::make_unique<DataProcessorService>(*this);

  // Whole-series object generators publish markers into object_store_ (the object
  // engine never touches data_engine_). The real series resolver is injected by the
  // shell once the catalog exists (setResolver); this stub, which resolves nothing,
  // only covers the window before that. It must stay a window: a generator restored
  // from a saved layout runs the moment it is re-created, so a shell that injected
  // late would silently run every restored rule against no data.
  marker_service_ = std::make_unique<MarkerService>(
      object_store_, [](DatasetId, const std::string&) { return std::optional<MarkerService::ResolvedSeries>{}; });
  // An all-datasets set is kept in the display frame and shifted per dataset, so it
  // lands at the same display instant everywhere; only the alignment shift matters
  // (globalTimeReference is uniform and cancels out).
  marker_service_->setDisplayOffsetResolver(
      [this](DatasetId dataset_id) { return static_cast<Timestamp>(sourceDisplayOffset(dataset_id).value.count()); });

  // Install the bundled Luau filter catalogue so applied/restored filters resolve
  // to Luau classes. The resource is embedded in the app; it is absent only in a
  // headless unit test, where that binary installs its own catalogue if it needs
  // filters (there is no native C++ builtin fallback after M9).
  auto catalogue = std::make_shared<scripting::FilterCatalogue>(scripting::makeLuauEngine());
  if (QFile f(u":/filters/builtin_filters.luau"_s); f.open(QIODevice::ReadOnly)) {
    if (auto added = catalogue->addBundledSource(f.readAll().toStdString(), "bundled"); !added.has_value()) {
      qCWarning(lcSession) << "filter catalogue load failed:" << QString::fromStdString(added.error());
    }
  }
  // Install only a non-empty catalogue: a headless binary without the embedded
  // resource simply has no filters rather than an empty registry.
  if (!catalogue->entries().empty()) {
    processor_service_->setFilterCatalogue(std::move(catalogue));
  }
}

SessionManager::~SessionManager() = default;

DataReader SessionManager::createReader() const {
  return data_engine_.createReader();
}

void SessionManager::setDatasetSourcePath(DatasetId dataset_id, QString path) {
  path = normalizedSourcePath(path);
  if (path.isEmpty()) {
    dataset_source_paths_.erase(dataset_id);
  } else {
    dataset_source_paths_.insert_or_assign(dataset_id, std::move(path));
  }
}

QString SessionManager::datasetSourcePath(DatasetId dataset_id) const {
  const auto it = dataset_source_paths_.find(dataset_id);
  return it != dataset_source_paths_.end() ? it->second : QString{};
}

void SessionManager::attachSourceRecord(DatasetId dataset_id, SourceRecord record) {
  dataset_source_records_.insert_or_assign(dataset_id, std::move(record));
}

const SourceRecord* SessionManager::sourceRecord(DatasetId dataset_id) const {
  const auto it = dataset_source_records_.find(dataset_id);
  return it != dataset_source_records_.end() ? &it->second : nullptr;
}

void SessionManager::detachSourceRecord(DatasetId dataset_id) {
  dataset_source_records_.erase(dataset_id);
}

Status SessionManager::validateDatasetMetadata(const QString& json) {
  constexpr qsizetype kMaxBytes = 1024 * 1024;
  constexpr int kMaxDepth = 32;
  constexpr int kMaxNodes = 16384;
  const QByteArray bytes = json.toUtf8();
  if (bytes.size() > kMaxBytes) {
    return PJ::unexpected("metadata document exceeds 1 MiB");
  }
  QJsonParseError parse_error{};
  const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse_error);
  if (parse_error.error != QJsonParseError::NoError) {
    return PJ::unexpected("metadata is not valid JSON: " + parse_error.errorString().toStdString());
  }
  if (!document.isObject()) {
    return PJ::unexpected("metadata document must be a JSON object");
  }
  int nodes = 0;
  // Iterative walk: a hostile depth-32 document must not become stack depth.
  std::vector<std::pair<QJsonValue, int>> pending;
  pending.emplace_back(document.object(), 1);
  while (!pending.empty()) {
    const auto [value, depth] = pending.back();
    pending.pop_back();
    if (depth > kMaxDepth) {
      return PJ::unexpected("metadata nesting exceeds depth 32");
    }
    if (++nodes > kMaxNodes) {
      return PJ::unexpected("metadata exceeds 16384 values");
    }
    if (value.isObject()) {
      const QJsonObject object = value.toObject();
      for (auto it = object.begin(); it != object.end(); ++it) {
        pending.emplace_back(it.value(), depth + 1);
      }
    } else if (value.isArray()) {
      for (const QJsonValue& element : value.toArray()) {
        pending.emplace_back(element, depth + 1);
      }
    }
  }
  return PJ::okStatus();
}

Status SessionManager::setDatasetMetadata(DatasetId dataset_id, const QString& json, QString plugin_id) {
  const QString trimmed = json.trimmed();
  if (trimmed.isEmpty() || trimmed == u"{}"_s) {
    dataset_metadata_.erase(dataset_id);
    return PJ::okStatus();
  }
  if (auto status = validateDatasetMetadata(json); !status) {
    return status;
  }
  dataset_metadata_.insert_or_assign(dataset_id, DatasetMetadata{std::move(plugin_id), json});
  return PJ::okStatus();
}

const DatasetMetadata* SessionManager::datasetMetadata(DatasetId dataset_id) const {
  const auto it = dataset_metadata_.find(dataset_id);
  return it != dataset_metadata_.end() ? &it->second : nullptr;
}

void SessionManager::clearDatasetMetadata(DatasetId dataset_id) {
  dataset_metadata_.erase(dataset_id);
}

void SessionManager::pinDatasetResource(DatasetId dataset_id, std::shared_ptr<void> resource) {
  if (resource != nullptr) {
    dataset_resources_[dataset_id].push_back(std::move(resource));
  }
}

DatasetIdentityResolution SessionManager::resolveDatasetIdentity(
    DatasetId saved_id, const QString& saved_source, const QString& saved_path) const {
  return resolveDatasetIdentity(saved_id, saved_source, saved_path, SourceRecord{});
}

DatasetIdentityResolution SessionManager::resolveDatasetIdentity(
    DatasetId saved_id, const QString& saved_source, const QString& saved_path,
    const SourceRecord& saved_record) const {
  // normalizedSourcePath canonicalizes an existing file but falls back to the
  // cleaned-absolute form once the file is gone. For a plain (non-symlinked) path
  // both forms coincide, so a deleted-on-disk source still compares equal to its
  // registered path here. (A source under a symlinked directory that is stored
  // canonical and later deleted cannot be reconciled by string comparison — see the
  // note on resolveSeriesPath; that residual corner is out of scope.)
  const QString normalized_saved_path = normalizedSourcePath(saved_path);
  const auto source_matches = [&saved_source](const DatasetInfo* info) {
    return info != nullptr && (saved_source.isEmpty() || QString::fromStdString(info->source_name) == saved_source);
  };
  const auto path_matches = [this, &normalized_saved_path](DatasetId id) {
    return normalized_saved_path.isEmpty() || datasetSourcePath(id) == normalized_saved_path;
  };
  const bool record_supplied = !saved_record.provider_id.isEmpty() || !saved_record.source_identity.isEmpty() ||
                               !saved_record.descriptor_json.isEmpty();
  // Full-record agreement: (provider_id, source_identity) is only the FAST
  // path — the identity digest could collide or drift — so trust additionally
  // requires the full canonical descriptor to confirm, i.e. byte-equal
  // descriptor_json. An unsupplied record (all fields empty) constrains nothing,
  // which keeps every record-less caller's resolution byte-identical.
  const auto record_matches = [this, &saved_record, record_supplied](DatasetId id) {
    if (!record_supplied) {
      return true;
    }
    const auto it = dataset_source_records_.find(id);
    return it != dataset_source_records_.end() && it->second.provider_id == saved_record.provider_id &&
           it->second.source_identity == saved_record.source_identity &&
           it->second.descriptor_json == saved_record.descriptor_json;
  };

  if (saved_id != 0) {
    const DatasetInfo* exact = data_engine_.getDataset(saved_id);
    if (source_matches(exact) && path_matches(saved_id) && record_matches(saved_id)) {
      return DatasetIdentityResolution{.id = saved_id};
    }
  }

  // Record tier: a unique confirmed record match outranks the path/name tiers
  // (provenance is more durable than either label). An identity hit whose
  // descriptor bytes differ is NOT a match (record_matches above) and falls
  // through; several confirmed matches are ambiguous — never guess.
  if (record_supplied) {
    std::optional<DatasetId> record_match;
    for (const auto& entry : dataset_source_records_) {
      const DatasetId candidate = entry.first;
      if (!record_matches(candidate) || data_engine_.getDataset(candidate) == nullptr) {
        continue;
      }
      if (record_match.has_value()) {
        return DatasetIdentityResolution{.id = std::nullopt, .ambiguous = true};
      }
      record_match = candidate;
    }
    if (record_match.has_value()) {
      return DatasetIdentityResolution{.id = record_match};
    }
  }

  // No portable qualifier means there is no safe remint fallback. This keeps a
  // legacy numeric-only identity same-session-only.
  if (normalized_saved_path.isEmpty() && saved_source.isEmpty()) {
    return {};
  }

  std::optional<DatasetId> match;
  for (const DatasetId candidate : data_engine_.listDatasets()) {
    const DatasetInfo* info = data_engine_.getDataset(candidate);
    if (!source_matches(info) || !path_matches(candidate)) {
      continue;
    }
    if (match.has_value()) {
      return DatasetIdentityResolution{.id = std::nullopt, .ambiguous = true};
    }
    match = candidate;
  }
  return DatasetIdentityResolution{.id = match};
}

DatasetIdentityResolution SessionManager::resolveObjectDatasetIdentity(
    DatasetId saved_id, const QString& saved_source, const QString& saved_path,
    const QString& object_topic_name) const {
  const DatasetIdentityResolution ordinary = resolveDatasetIdentity(saved_id, saved_source, saved_path);
  if (ordinary.id.has_value() || saved_path.isEmpty() || object_topic_name.isEmpty()) {
    return ordinary;
  }
  const QString normalized_path = normalizedSourcePath(saved_path);
  std::optional<DatasetId> match;
  for (const ObjectTopicId topic_id : object_store_.listTopics()) {
    const ObjectTopicDescriptor descriptor = object_store_.descriptor(topic_id);
    if (QString::fromStdString(descriptor.topic_name) != object_topic_name ||
        datasetSourcePath(descriptor.dataset_id) != normalized_path) {
      continue;
    }
    if (match.has_value() && *match != descriptor.dataset_id) {
      return DatasetIdentityResolution{.id = std::nullopt, .ambiguous = true};
    }
    match = descriptor.dataset_id;
  }
  return DatasetIdentityResolution{.id = match, .ambiguous = ordinary.ambiguous && !match.has_value()};
}

Timestamp SessionManager::datasetDomainDisplayOffset(DatasetId dataset_id) const {
  // Base shift from the dataset's TimeDomain. Live lookup via the time-domain
  // map: the dataset's own time_domain is a snapshot from createDataset, so
  // reading its display_offset directly would go stale after setDisplayOffset.
  Timestamp offset_ns = 0;
  if (const DatasetInfo* dataset = data_engine_.getDataset(dataset_id);
      dataset != nullptr && dataset->time_domain.id != 0) {
    if (const TimeDomain* domain = data_engine_.getTimeDomain(dataset->time_domain.id)) {
      offset_ns = domain->display_offset;
    }
  }
  return offset_ns;
}

DisplayOffset SessionManager::sourceDisplayOffset(DatasetId dataset_id) const {
  // The per-source ALIGNMENT shift only (display_time = raw_time - offset): the
  // dataset's TimeDomain offset, which is exactly what the Source Timeline edits.
  // The global "Use time offset" reference is NOT folded in here (see
  // displayOffset), so a Timeline drag round-trips without double-counting and
  // the bars stay put when the global frame is toggled.
  return DisplayOffset{Duration{datasetDomainDisplayOffset(dataset_id)}};
}

DisplayOffset SessionManager::displayOffset(DatasetId dataset_id) const {
  // Total display shift for the display axis = per-source alignment + the global
  // "Use time offset" origin. Summed here (not stored together) so toggling the
  // global frame never disturbs the per-source alignment.
  return DisplayOffset{sourceDisplayOffset(dataset_id).value + Duration{globalTimeReference()}};
}

std::optional<DisplaySeconds> SessionManager::displayTimeForSource(DatasetId dataset_id, int64_t absolute_ns) const {
  if (dataset_id == 0 || data_engine_.getDataset(dataset_id) == nullptr) {
    return std::nullopt;
  }
  return rawToDisplaySeconds(absolute_ns, displayOffset(dataset_id));
}

Timestamp SessionManager::globalTimeReference() const {
  // Zero (absolute display) when the toggle is off. When on, the earliest raw
  // sample ever observed across ALL datasets, applied uniformly. Per-dataset
  // mins are pinned so live retention cannot slide the display origin forward.
  if (!use_time_offset_) {
    return 0;
  }
  if (!global_min_cache_.has_value()) {
    Timestamp global_min = std::numeric_limits<Timestamp>::max();
    bool found = false;
    for (const DatasetId dataset_id : data_engine_.listDatasets()) {
      // Trust the warm per-dataset pin: consult dataset_min_cache_ first so a
      // dataset just refreshed by refreshDatasetMinTimestampsForTopics resolves to
      // a hash-map hit, and an untouched dataset is never rescanned. This whole
      // loop trusts the pins: any path that can RAISE a dataset's min
      // (removeDataset/evict/clear/refill/refreshDatasetTimeReference) must
      // invalidate that dataset's pin AND reset this global memo, or the recompute
      // would read a stale-low origin.
      if (const auto it = dataset_min_cache_.find(dataset_id); it != dataset_min_cache_.end()) {
        global_min = std::min(global_min, it->second);
        found = true;
      } else if (const auto bounds = datasetRawBounds(dataset_id); bounds.has_value()) {
        // Genuinely cold (unpinned) dataset: pin it in this one scan (rememberDataset-
        // MinTimestamp only lowers, matching the pin's monotone-down invariant). An
        // empty dataset (no bounds) never contributes a spurious 0 origin.
        global_min = std::min(global_min, rememberDatasetMinTimestamp(dataset_id, bounds->first));
        found = true;
      }
    }
    global_min_cache_ = found ? global_min : 0;
  }
  return *global_min_cache_;
}

std::optional<std::pair<Timestamp, Timestamp>> SessionManager::datasetRawBounds(DatasetId dataset_id) const {
  // The one scalar(DataEngine) + object(ObjectStore) time-bounds union. Shared by
  // datasetDisplayRange (needs both ends) and datasetMinTimestamp (min only).
  const DataReader reader = createReader();
  Timestamp t_min = std::numeric_limits<Timestamp>::max();
  Timestamp t_max = std::numeric_limits<Timestamp>::min();
  bool found = false;
  for (const TopicId topic_id : reader.listTopics(dataset_id)) {
    const auto metadata = reader.getMetadata(topic_id);
    if (metadata.has_value() && metadata->total_row_count > 0) {
      t_min = std::min(t_min, metadata->time_range_min);
      t_max = std::max(t_max, metadata->time_range_max);
      found = true;
    }
  }
  for (const ObjectTopicId object_topic_id : object_store_.listTopics(dataset_id)) {
    // Reserved marker snapshot topics ("__markers__/...") are stored at a sentinel
    // timestamp (0), not on the data clock — including them would drag the dataset
    // origin to the epoch and stretch playback across decades.
    if (isMarkerObjectTopic(object_store_.descriptor(object_topic_id).topic_name)) {
      continue;
    }
    if (object_store_.entryCount(object_topic_id) > 0) {
      const auto [object_min, object_max] = object_store_.timeRange(object_topic_id);
      t_min = std::min(t_min, object_min);
      t_max = std::max(t_max, object_max);
      found = true;
    }
  }
  if (!found) {
    return std::nullopt;
  }
  return std::pair{t_min, t_max};
}

Timestamp SessionManager::datasetMinTimestamp(DatasetId dataset_id) const {
  // Pinned earliest sample for the dataset's relative-time frame. It is cached
  // because displayOffset() is a hot path, but unlike a normal bounds cache it
  // intentionally does not move forward when streaming retention raises the
  // current readable minimum. That keeps the live edge advancing instead of
  // projecting every retained window back to 0..buffer_seconds.
  if (const auto it = dataset_min_cache_.find(dataset_id); it != dataset_min_cache_.end()) {
    return it->second;
  }
  const auto bounds = datasetRawBounds(dataset_id);
  if (!bounds) {
    return 0;
  }
  return rememberDatasetMinTimestamp(dataset_id, bounds->first);
}

Timestamp SessionManager::rememberDatasetMinTimestamp(DatasetId dataset_id, Timestamp observed_min) const {
  const auto [it, inserted] = dataset_min_cache_.emplace(dataset_id, observed_min);
  if (!inserted && observed_min < it->second) {
    it->second = observed_min;
  }
  return it->second;
}

void SessionManager::refreshDatasetMinTimestampsForTopics(const QVector<TopicId>& ids) const {
  // Refreshing per-dataset mins invalidates the across-datasets memo. Reset up
  // front so it still fires on the empty-but-live notify path (early-return below).
  global_min_cache_.reset();
  if (ids.isEmpty()) {
    return;
  }

  std::unordered_set<DatasetId> dataset_ids;
  {
    auto lock = data_engine_.lockEngine();
    dataset_ids.reserve(static_cast<std::size_t>(ids.size()));
    for (const TopicId id : ids) {
      if (const TopicStorage* storage = data_engine_.getTopicStorage(id)) {
        dataset_ids.insert(storage->descriptor().dataset_id);
      }
    }
  }

  for (const DatasetId dataset_id : dataset_ids) {
    if (const auto bounds = datasetRawBounds(dataset_id); bounds.has_value()) {
      (void)rememberDatasetMinTimestamp(dataset_id, bounds->first);
    } else {
      invalidateDatasetMinTimestamp(dataset_id);
    }
  }
}

void SessionManager::invalidateDatasetMinTimestamp(DatasetId dataset_id) const {
  dataset_min_cache_.erase(dataset_id);
  global_min_cache_.reset();
}

void SessionManager::setUseTimeOffset(bool use) {
  if (use_time_offset_ == use) {
    return;
  }
  use_time_offset_ = use;
  // Flip ONLY the global frame: displayOffset() now adds globalTimeReference()
  // (earliest raw sample across all datasets when on, 0 when off) on top of each
  // dataset's per-source alignment. No per-source TimeDomain offset is written, so
  // the Source Timeline's bar positions are untouched — only the numbers reframe.
  // No topic changed, only the display->raw mapping; tell every offset reader
  // (curve adapters, scenes, the playback seed, the Timeline) to re-resolve. This
  // global frame change uses the no-arg overload; the per-dataset overload is
  // reserved for single-source Timeline edits.
  // A boolean frame flip is observable even when the session is empty and the
  // numerical reference is zero (timeline formatting still changes), so this
  // always emits — record the current origin so the change-detecting notify
  // below does not re-emit for the same value on the next ingest.
  last_notified_global_reference_ = globalTimeReference();
  emit displayOffsetChanged();
}

void SessionManager::notifyGlobalTimeReferenceIfChanged() {
  const Timestamp current = globalTimeReference();
  if (current == last_notified_global_reference_) {
    return;
  }
  last_notified_global_reference_ = current;
  emit displayOffsetChanged();
}

std::optional<DisplayRange> SessionManager::datasetDisplayRange(DatasetId dataset_id) const {
  const auto bounds = datasetRawBounds(dataset_id);
  if (!bounds) {
    return std::nullopt;
  }
  // The single raw-ns -> display-seconds crossing for the streaming range, via
  // the dataset's own offset — identical to the file-load seed (both origins
  // match) so a bare raw-ns range can never reach the playback axis.
  const DisplayOffset offset = displayOffset(dataset_id);
  return DisplayRange{
      .min = rawToDisplaySeconds(bounds->first, offset), .max = rawToDisplaySeconds(bounds->second, offset)};
}

void SessionManager::setDisplayOffset(DatasetId dataset_id, DisplayOffset offset) {
  // Mirror displayOffset()'s resolution: dataset -> its TimeDomain id. Writing
  // the domain (not the dataset snapshot) is what makes displayOffset() read it
  // back live; emit so consumers re-snap/re-map without re-indexing samples.
  const DatasetInfo* dataset = data_engine_.getDataset(dataset_id);
  if (dataset == nullptr || dataset->time_domain.id == 0) {
    qCWarning(lcSession) << "setDisplayOffset: unknown dataset or default domain" << dataset_id;
    return;
  }
  // Idempotent: an unchanged offset emits nothing, so a no-op write (resetAll over
  // already-zero datasets, a settled live drag re-sending the same value) doesn't
  // churn consumers — every displayOffsetChanged triggers a PlotWidget adapter
  // drop + replot and a timeline offset refresh.
  if (sourceDisplayOffset(dataset_id).value == offset.value) {
    return;
  }
  data_engine_.setDisplayOffset(dataset->time_domain.id, static_cast<Timestamp>(offset.value.count()));
  emit displayOffsetChanged(dataset_id);
  // All-datasets marker sets follow the display frame: re-derive this dataset's part.
  if (marker_service_ && marker_service_->hasGenerators() &&
      !marker_service_->rebaseForDisplayOffset(dataset_id).empty()) {
    notifyMarkersChanged();
  }
}

std::vector<TopicId> SessionManager::commitChunks(std::vector<std::pair<TopicId, TopicChunk>> chunks) {
  auto changed = data_engine_.commitChunks(std::move(chunks));
  if (changed.empty()) {
    return changed;
  }
  // Run eager filters over the freshly committed input, then notify both the raw
  // and the derived output topics so filtered curves refresh. (Loaded files keep
  // full history, so there is no retention race on this path.)
  const std::vector<TopicId> derived_outputs =
      processor_service_ ? processor_service_->advanceOnCommit(changed) : std::vector<TopicId>{};

  // Whole-series generators are not incremental: a committed topic re-runs every
  // generator that reads it. Repaint overlays if any marker set was republished.
  // Unscoped: a commit batch may span datasets, and this path is cold (bulk load).
  recomputeMarkersForChanged(changed, /*scope=*/0);
  QVector<TopicId> ids;
  ids.reserve(static_cast<qsizetype>(changed.size() + derived_outputs.size()));
  for (const TopicId id : changed) {
    ids.push_back(id);
  }
  for (const TopicId id : derived_outputs) {
    ids.push_back(id);
  }
  refreshDatasetMinTimestampsForTopics(ids);  // also resets global_min_cache_
  // Freshly committed samples can lower the cross-dataset origin (a file loaded
  // later but starting earlier). Reframe surviving adapters before the per-topic
  // signal, which never covers a pure origin shift.
  notifyGlobalTimeReferenceIfChanged();
  emit samplesIngested(std::move(ids), /*live=*/false);
  return changed;
}

void SessionManager::notifyIngest(QVector<TopicId> ids, bool live) {
  refreshDatasetMinTimestampsForTopics(ids);  // also resets global_min_cache_
  // Object-only ingest legitimately carries no scalar TopicIds; recompute the
  // global origin before the empty/non-live samples signal is suppressed.
  notifyGlobalTimeReferenceIfChanged();
  if (ids.isEmpty() && !live) {
    return;
  }
  emit samplesIngested(std::move(ids), live);
}

SessionManager::ActiveIngestEntry* SessionManager::liveEntry(IngestToken token) {
  if (token.id == 0) {
    return nullptr;
  }
  const auto it = active_ingests_.find(token.dataset_id);
  if (it == active_ingests_.end() || it->second.id != token.id) {
    return nullptr;
  }
  return &it->second;
}

IngestToken SessionManager::beginIngest(
    DatasetId dataset_id, QString label, quint64 total, bool cancellable, IngestCancelFn on_cancel, IngestStop stop) {
  // Mint a new monotonic id (never 0, which is reserved for invalid/stale).
  const IngestId new_id = next_ingest_id_++;
  const IngestToken token{new_id, dataset_id};
  const bool discardable = stop == IngestStop::kKeepOrDiscard;

  // A restart supersedes whatever this dataset was already running. Report that
  // as a real terminal instead of dropping the entry silently: kCancelled is
  // exactly what happened to it, and every observer already knows how to retire
  // a cancelled ingest. Without it each observer would have to re-derive the
  // supersede rule from the next ingestBegan — and anything it had bound to the
  // old token (a strip owner, a row) would leak until it did.
  IngestToken superseded_token{};
  if (const auto superseded = active_ingests_.find(dataset_id); superseded != active_ingests_.end()) {
    superseded_token = IngestToken{superseded->second.id, dataset_id};
  }

  // Install the replacement FIRST, then report the terminal. The emit below is
  // synchronous and re-entrant: an observer that calls back in (activeIngests,
  // ingestActive, even another beginIngest) must find this dataset's lifecycle
  // continuous rather than momentarily absent. The superseded token is already
  // stale by then — liveEntry matches on the id — so a late update/end/cancel
  // carrying it still no-ops.
  active_ingests_.insert_or_assign(
      dataset_id, ActiveIngestEntry{
                      new_id, label, /*current=*/0, total, cancellable, discardable,
                      /*cancel_requested=*/false, /*cancel_requested_ms=*/0, std::move(on_cancel)});
  if (superseded_token.id != 0) {
    emit ingestEnded(superseded_token, IngestOutcome::kCancelled);
  }

  // The terminal above is synchronous and re-entrant: an observer may have begun
  // ANOTHER ingest on this dataset from inside it — which superseded the entry
  // installed above and already reported ITS terminal. Announcing this began now
  // would arrive after the successor's, and every observer keyed by dataset would
  // bind its row to a token that is already dead — so this token faces the same
  // staleness oracle as every other call. Suppressing is consistent rather than
  // lossy: a token whose
  // began never went out has no observer state to retire, and the nested call
  // already reported the whole (empty) lifecycle it did have.
  if (liveEntry(token) == nullptr) {
    qCInfo(lcCancel) << "ingest" << new_id << "on dataset" << dataset_id
                     << "was superseded from inside its own supersede terminal; its began is suppressed";
    return token;
  }

  // Deliberately no data publication here: begin announces a lifecycle, it does
  // not commit rows. Publishing would make the catalog rebuild before the
  // loader's own commit seam, and a dataset with no row yet is served by the
  // controller's ghost row until its first flush lands.
  emit ingestBegan(token, label, total, cancellable, discardable);
  return token;
}

void SessionManager::updateIngest(IngestToken token, quint64 current, quint64 total) {
  // Publish the tick's flushed rows through the one non-live data-publication
  // seam every ingest path shares (plots/playback/catalog listen on
  // samplesIngested; notifyIngest suppresses the empty-non-live case itself).
  // Deliberately AHEAD of EVERY gate below: data must never be dropped because
  // a lifecycle call arrived out of order, the token was superseded mid-flush,
  // or no beginIngest ever ran. Only the lifecycle SIGNAL is gated. A token
  // naming no dataset has nothing to publish for, which is the sole exception.
  if (token.dataset_id != 0) {
    const auto ids = data_engine_.listTopics(token.dataset_id);
    notifyIngest(QVector<TopicId>(ids.begin(), ids.end()), /*live=*/false);
  }

  ActiveIngestEntry* entry = liveEntry(token);
  if (entry == nullptr) {
    return;
  }
  entry->current = current;
  entry->total = total;
  // Reported unconditionally, including after a stop was requested: this signal
  // states what the producer did, and it really did deliver these rows. Whether
  // a stopping row should keep animating is a presentation policy, and it lives
  // with the presentation (IngestProgressController).
  emit ingestProgressed(token, current, total);
}

void SessionManager::endIngest(IngestToken token, IngestOutcome outcome) {
  ActiveIngestEntry* entry = liveEntry(token);
  if (entry == nullptr) {
    return;  // stale (superseded by a newer begin) or already terminal
  }

  // If cancellation was requested on this token, report kCancelled regardless
  // of the outcome the producer supplies.
  if (entry->cancel_requested) {
    outcome = IngestOutcome::kCancelled;
  }

  if (entry->cancel_requested_ms != 0) {
    qCInfo(lcCancel) << "ingest" << token.id << "terminated"
                     << (QDateTime::currentMSecsSinceEpoch() - entry->cancel_requested_ms)
                     << "ms after the stop was requested; outcome" << static_cast<int>(outcome);
  }

  active_ingests_.erase(token.dataset_id);
  emit ingestEnded(token, outcome);
}

bool SessionManager::requestCancel(IngestToken token, bool keep_partial) {
  ActiveIngestEntry* entry = liveEntry(token);
  if (entry == nullptr) {
    return false;  // stale, unknown, or already terminal
  }

  if (!entry->cancellable) {
    return false;
  }

  // A producer that cannot roll back must not be handed a discard: silently
  // downgrading it to a keep is exactly the lie the UI greys the bin to avoid.
  if (!keep_partial && !entry->discardable) {
    return false;
  }

  // FIRST INTENT WINS, the same doctrine the terminal follows. Without this gate
  // an ingestStopping observer that stops the ingest again recurses through the
  // synchronous emit below, and a second click with the opposite keep_partial
  // would re-run the producer's handler with a choice contradicting the one it
  // already acted on. Report the acceptance that already stands.
  if (entry->cancel_requested) {
    qCInfo(lcCancel) << "requestCancel ignored: ingest" << token.id << "is already stopping";
    return true;
  }

  // Record cancellation intent so the terminal outcome reports kCancelled.
  entry->cancel_requested = true;
  entry->cancel_requested_ms = QDateTime::currentMSecsSinceEpoch();
  qCInfo(lcCancel) << "requestCancel accepted: ingest" << token.id << "dataset" << token.dataset_id << "keep_partial"
                   << keep_partial;

  // Snapshot what the rest of this call needs BEFORE the emit below. That emit
  // is synchronous, and an observer is allowed to terminalize (endIngest) or
  // restart (beginIngest) this dataset from inside it — either erases the map
  // element `entry` points at, and everything after would be reading freed
  // memory. The copy also keeps the producer's handler alive across its own
  // call, which normally ends the ingest and destroys the stored std::function.
  const IngestCancelFn on_cancel = entry->on_cancel;
  entry = nullptr;  // anything past the emit must go through liveEntry() again

  // Announce BEFORE running the producer's handler: the acknowledgement must not
  // be hostage to how long the producer takes to notice, and every stop is
  // cooperative to some degree.
  emit ingestStopping(token, keep_partial);

  // A producer that never terminates would leave the row stopping forever with
  // no clue why; say so once rather than leaving it to be discovered by eye.
  QTimer::singleShot(kStopWatchdogMs, this, [this, token]() {
    const ActiveIngestEntry* still_live = liveEntry(token);
    if (still_live != nullptr && still_live->cancel_requested) {
      qCWarning(lcCancel) << "ingest" << token.id << "has not terminated" << kStopWatchdogMs
                          << "ms after the stop was requested; the producer is not responding";
    }
  });

  if (on_cancel) {
    QElapsedTimer handler_timer;
    handler_timer.start();
    on_cancel(keep_partial);
    // The handler is only expected to REQUEST a stop; a large number here means
    // the producer did teardown work on the GUI thread and blocked the UI.
    qCInfo(lcCancel) << "producer cancel handler returned after" << handler_timer.elapsed() << "ms";
  }

  return true;
}

std::unordered_map<DatasetId, SessionManager::ActiveIngest> SessionManager::activeIngests() const {
  std::unordered_map<DatasetId, ActiveIngest> by_dataset;
  by_dataset.reserve(active_ingests_.size());
  for (const auto& [dataset_id, entry] : active_ingests_) {
    by_dataset.emplace(
        dataset_id, ActiveIngest{
                        .token = IngestToken{.id = entry.id, .dataset_id = dataset_id},
                        .label = entry.label,
                        .current = entry.current,
                        .total = entry.total,
                        .cancellable = entry.cancellable,
                        .discardable = entry.discardable,
                    });
  }
  return by_dataset;
}

bool SessionManager::ingestActive(DatasetId dataset_id) const {
  return active_ingests_.count(dataset_id) != 0;
}

void SessionManager::notifyDatasetAboutToBeReplaced(DatasetId dataset_id) {
  invalidateDatasetMinTimestamp(dataset_id);
  emit datasetAboutToBeReplaced(dataset_id);
}

RefillGuard SessionManager::beginRefill(DatasetId dataset_id) {
  return RefillGuard(*this, dataset_id);  // guaranteed copy elision (move-only)
}

// --- RefillGuard: the transactional in-place reload (see SessionManager::beginRefill). ---

RefillGuard::RefillGuard(SessionManager& session, DatasetId dataset_id) : session_(&session), dataset_id_(dataset_id) {
  // GUI thread, no event loop — same contract as replaceDataset.
  Q_ASSERT(session.thread() == QThread::currentThread());

  // Capture the prior topic id sets BEFORE detaching: scalars for the empty-state
  // notify, object ids so rollback can tell which object topics a failed refill added.
  const std::vector<TopicId> scalar_topics = session.dataEngine().listTopics(dataset_id);
  processor_output_topic_ids_ = session.dataProcessorService().processorOutputTopics();
  replaced_source_topic_ids_.reserve(scalar_topics.size());
  for (const TopicId topic_id : scalar_topics) {
    if (processor_output_topic_ids_.count(topic_id) == 0) {
      replaced_source_topic_ids_.push_back(topic_id);
    }
  }
  prior_object_topic_ids_ = session.objectStore().listTopics(dataset_id);
  // The refill re-binds same-named topics into these SAME ids, so it also lands its
  // parsers in their slots. Hold the prior bindings (shared ownership) so a rollback
  // can put back the parser that wrote the bytes it restores.
  object_parser_snapshot_ = session.snapshotObjectTopicParsers(prior_object_topic_ids_);

  // (1) Adapters drop cached TopicChunk* before any deque is moved (same ordering
  //     as replaceDataset).
  session.notifyDatasetAboutToBeReplaced(dataset_id);
  // (2) scalar + (3) object: DETACH (move aside, not free), keeping ids registered
  //     so the progressive refill writes back into the same ids.
  scalar_snapshot_ = session.dataEngine().detachDatasetChunks(dataset_id);
  object_snapshot_ = session.objectStore().detachDataset(dataset_id);
  // (4) UI sees the dataset empty (non-live). no-ops on an empty id list.
  session.notifyIngest(QVector<TopicId>(scalar_topics.begin(), scalar_topics.end()), /*live=*/false);
}

RefillGuard::RefillGuard(RefillGuard&& other) noexcept
    : session_(other.session_),
      dataset_id_(other.dataset_id_),
      scalar_snapshot_(std::move(other.scalar_snapshot_)),
      object_snapshot_(std::move(other.object_snapshot_)),
      prior_object_topic_ids_(std::move(other.prior_object_topic_ids_)),
      object_parser_snapshot_(std::move(other.object_parser_snapshot_)),
      replaced_source_topic_ids_(std::move(other.replaced_source_topic_ids_)),
      processor_output_topic_ids_(std::move(other.processor_output_topic_ids_)),
      committed_(other.committed_) {
  other.session_ = nullptr;  // the moved-from guard must not roll back
  other.committed_ = true;
}

RefillGuard& RefillGuard::operator=(RefillGuard&& other) noexcept {
  if (this != &other) {
    if (session_ != nullptr && !committed_) {
      rollback();  // discard the transaction this guard still owns before taking over
    }
    session_ = other.session_;
    dataset_id_ = other.dataset_id_;
    scalar_snapshot_ = std::move(other.scalar_snapshot_);
    object_snapshot_ = std::move(other.object_snapshot_);
    prior_object_topic_ids_ = std::move(other.prior_object_topic_ids_);
    object_parser_snapshot_ = std::move(other.object_parser_snapshot_);
    replaced_source_topic_ids_ = std::move(other.replaced_source_topic_ids_);
    processor_output_topic_ids_ = std::move(other.processor_output_topic_ids_);
    committed_ = other.committed_;
    other.session_ = nullptr;
    other.committed_ = true;
  }
  return *this;
}

RefillGuard::~RefillGuard() {
  if (session_ != nullptr && !committed_) {
    rollback();
  }
}

void RefillGuard::commit() {
  committed_ = true;
  scalar_snapshot_ = {};  // free the held-aside prior data; the refilled data is kept
  object_snapshot_ = {};
  prior_object_topic_ids_.clear();
  // Releases the prior parsers: the refill's bindings are the live ones now.
  object_parser_snapshot_.clear();
  replaced_source_topic_ids_.clear();
  processor_output_topic_ids_.clear();
  // The refill rewrote the dataset's content from a NEW source, so whatever
  // provider provenance the OLD content carried is stale — a promoted cloud
  // dataset refilled from a local file must not keep the cloud descriptor for
  // the local bytes. Structural here (any in-place refill, not one caller's
  // path); a rollback never reaches commit and keeps the record.
  if (session_ != nullptr) {
    session_->detachSourceRecord(dataset_id_);
    // Loader metadata described the OLD content just as the record did; the
    // new load's document (if any) is published at its own commit seam.
    session_->clearDatasetMetadata(dataset_id_);
  }
}

void SessionManager::publishSharedMarkersToNewDatasets() {
  if (marker_service_ && marker_service_->hasGenerators() && !marker_service_->reachNewDatasets().empty()) {
    notifyMarkersChanged();
  }
}

void SessionManager::recomputeMarkersForChanged(const std::vector<TopicId>& changed, DatasetId scope) {
  // Gated by hasGenerators() so the common (no-generator) path is a single bool check.
  if (marker_service_ && marker_service_->hasGenerators() &&
      !marker_service_->recomputeForChangedInputs(changedTopicNames(createReader(), changed), scope).empty()) {
    notifyMarkersChanged();
  }
}

Status RefillGuard::recomputeProcessors() {
  if (session_ == nullptr) {
    return PJ::okStatus();
  }
  // Markers are whole-series generators: a reload swaps the dataset's data
  // wholesale (and may rename or drop the inputs a generator read), so re-run
  // every generator bound to this dataset rather than matching changed names.
  // pruneVanishedTopics exempts marker topics, so the republished set lands on a
  // topic that survived the refill.
  if (session_->marker_service_ && session_->marker_service_->hasGenerators() &&
      !session_->marker_service_->recomputeForDataset(dataset_id_).empty()) {
    session_->notifyMarkersChanged();
  }
  if (replaced_source_topic_ids_.empty()) {
    return PJ::okStatus();
  }
  auto outputs = session_->dataProcessorService().rebindAndRecomputeForReplacedSources(replaced_source_topic_ids_);
  if (!outputs.has_value()) {
    return PJ::unexpected(outputs.error());
  }
  if (!outputs->empty()) {
    session_->notifyIngest(QVector<TopicId>(outputs->begin(), outputs->end()), /*live=*/false);
  }
  return PJ::okStatus();
}

void RefillGuard::pruneVanishedTopics() {
  if (session_ == nullptr) {
    return;
  }
  // Scalar: a prior topic still empty after the refill vanished from the new file.
  // Collect under one engine lock (getTopicStorage needs it held), then retire —
  // retireTopic re-locks (recursive mutex) and clears its already-empty deque.
  std::vector<TopicId> vanished_scalar;
  {
    auto lock = session_->dataEngine().lockEngine();
    for (const TopicId topic_id : scalar_snapshot_.prior_topic_ids) {
      if (processor_output_topic_ids_.count(topic_id) != 0) {
        continue;
      }
      const TopicStorage* storage = session_->dataEngine().getTopicStorage(topic_id);
      if (storage != nullptr && storage->empty()) {
        vanished_scalar.push_back(topic_id);
      }
    }
  }
  for (const TopicId topic_id : vanished_scalar) {
    session_->dataEngine().retireTopic(topic_id);
  }
  // Object: a prior object topic with no entries after the refill vanished too.
  // Marker snapshot topics are exempt: the refill empties the dataset's markers
  // (detach) but the file never re-writes them; the marker generators republish
  // on the post-refill recompute, so evicting the topic here would drop live
  // markers and orphan the recipe.
  std::vector<ObjectTopicId> vanished_object;
  for (const ObjectTopicId object_topic_id : prior_object_topic_ids_) {
    if (isMarkerObjectTopic(session_->objectStore().descriptor(object_topic_id).topic_name)) {
      continue;
    }
    if (session_->objectStore().entryCount(object_topic_id) == 0) {
      vanished_object.push_back(object_topic_id);
    }
  }
  if (!vanished_object.empty()) {
    session_->evictObjectTopics(vanished_object);  // drops the empty store series + its parser slot
  }
}

void RefillGuard::rollback() {
  // (a) Adapters drop any partial-refill chunk pointers cached via progress notifies.
  session_->notifyDatasetAboutToBeReplaced(dataset_id_);
  // (b) Scalar: drop partial refill chunks + retire topics it added, then move the
  //     prior chunks back into the stable ids.
  session_->dataEngine().reattachDatasetChunks(dataset_id_, std::move(scalar_snapshot_));
  // (c) Object: evict topics the refill ADDED (drops their store series + parser
  //     slots) BEFORE reattach, so reattach's own "current - prior" removal is a
  //     no-op. evictObjectTopics re-takes store_mutex_, so it must run OUTSIDE it —
  //     it does here (no ObjectStore lock held on this thread).
  std::unordered_set<uint32_t> prior;
  prior.reserve(prior_object_topic_ids_.size());
  for (const ObjectTopicId id : prior_object_topic_ids_) {
    prior.insert(id.id);
  }
  std::vector<ObjectTopicId> added;
  for (const ObjectTopicId id : session_->objectStore().listTopics(dataset_id_)) {
    if (prior.find(id.id) == prior.end()) {
      added.push_back(id);
    }
  }
  if (!added.empty()) {
    session_->evictObjectTopics(added);
  }
  // (d) Object: move the prior entries back into the stable ids.
  session_->objectStore().reattachDataset(dataset_id_, std::move(object_snapshot_));
  // (e) Object: put back the parser each stable id was bound to. The refill may have
  //     registered a different schema's parser in the very same slot, and the bytes
  //     just restored in (d) are only decodable by the one that wrote them. Restore is
  //     limited to ids this dataset still owns: a topic destructively removed while the
  //     guard was active (clearAllObjects / eviction) must not have its parser
  //     resurrected — ObjectStore::clear() even resets id allocation, so an unrelated
  //     future topic could reuse the raw id and silently inherit the stale parser.
  {
    std::unordered_set<uint32_t> surviving_ids;
    for (const ObjectTopicId id : session_->objectStore().listTopics(dataset_id_)) {
      surviving_ids.insert(id.id);
    }
    std::erase_if(object_parser_snapshot_, [&surviving_ids](const SessionManager::ObjectParserSnapshotEntry& entry) {
      return surviving_ids.find(entry.topic_id) == surviving_ids.end();
    });
  }
  session_->restoreObjectTopicParsers(std::move(object_parser_snapshot_));
  // (f) A failed tentative replay may have reset stateful operator internals.
  // Replaying over the byte-for-byte restored raw snapshot repairs that state.
  if (auto replayed = session_->dataProcessorService().rebindAndRecomputeForReplacedSources(replaced_source_topic_ids_);
      !replayed.has_value()) {
    qCWarning(lcSession).noquote() << "RefillGuard rollback processor replay failed:"
                                   << QString::fromStdString(replayed.error());
  }
  // (g) UI: reflect the restored topic set (non-live).
  const std::vector<TopicId> current = session_->dataEngine().listTopics(dataset_id_);
  session_->notifyIngest(QVector<TopicId>(current.begin(), current.end()), /*live=*/false);
}

std::size_t RefillGuard::snapshotBytes() const noexcept {
  std::size_t bytes = 0;
  // Scalar: approximate — rows * (1 timestamp column + N value columns) * 8 bytes/cell.
  for (const auto& [topic_id, topic] : scalar_snapshot_.topics) {
    (void)topic_id;
    for (const TopicChunk& chunk : topic.chunks) {
      const std::size_t cells = static_cast<std::size_t>(chunk.stats.row_count) * (1 + chunk.columns.size());
      bytes += cells * sizeof(double);
    }
  }
  // Object: exact — the store tracks per-series resident bytes.
  for (const auto& [raw_id, series] : object_snapshot_.series) {
    (void)raw_id;
    bytes += series.memory_bytes;
  }
  return bytes;
}

void SessionManager::replaceDataset(
    DataEngine& staged_engine, ObjectStore& staged_store, DatasetId staged_id, DatasetId primary_id,
    std::vector<std::pair<ObjectTopicId, std::unique_ptr<MessageParserHandle>>> staged_object_parsers) {
  // (1) Adapters drop cached TopicChunk* before any deque is touched. Same-thread
  // direct connection: every slot returns before the emit does, and this method
  // runs no event loop (the caller must not either) — so the pointers stay dead.
  notifyDatasetAboutToBeReplaced(primary_id);

  // (2a) Scalar swap. replaceDatasetFrom only errors on a programming mistake (same
  // engine, unknown dataset id), never on user data, and validates before mutating —
  // so on the unreachable error path the primary keeps its prior data. Log and continue.
  QVector<TopicId> changed;
  if (auto scalars = data_engine_.replaceDatasetFrom(staged_engine, staged_id, primary_id); scalars.has_value()) {
    changed.reserve(static_cast<int>(scalars->replaced_topics.size() + scalars->added_topics.size()));
    for (const TopicId t : scalars->replaced_topics) {
      changed.push_back(t);
    }
    for (const TopicId t : scalars->added_topics) {
      changed.push_back(t);
    }
  } else {
    qCWarning(lcSession).noquote() << "replaceDataset (scalars):" << QString::fromStdString(scalars.error());
  }

  // (2b) Object swap, then (3) re-register the collected parsers under the stable
  // primary ObjectTopicIds and drop parsers for object topics that vanished.
  if (auto objects = object_store_.replaceDatasetFrom(staged_store, staged_id, primary_id); objects.has_value()) {
    std::unordered_map<uint32_t, ObjectTopicId> staged_to_primary;
    staged_to_primary.reserve(objects->remapped.size());
    for (const auto& [staged_obj, primary_obj] : objects->remapped) {
      staged_to_primary.emplace(staged_obj.id, primary_obj);
    }
    for (auto& [staged_obj, parser] : staged_object_parsers) {
      const auto it = staged_to_primary.find(staged_obj.id);
      registerObjectTopicParser(it != staged_to_primary.end() ? it->second : staged_obj, std::move(parser));
    }
    evictObjectTopics(objects->removed_topics);
  } else {
    qCWarning(lcSession).noquote() << "replaceDataset (objects):" << QString::fromStdString(objects.error());
  }

  // (4) Recompute the filters whose input was just swapped: a reload replaces the
  // input chunks wholesale, so the derived output must be reset+replayed (not
  // appended). Notify those outputs too, so plots showing filtered curves refresh.
  if (processor_service_ && !changed.isEmpty()) {
    auto outputs =
        processor_service_->rebindAndRecomputeForReplacedSources(std::vector<TopicId>(changed.begin(), changed.end()));
    if (outputs.has_value()) {
      for (const TopicId out : *outputs) {
        changed.push_back(out);
      }
    } else {
      qCWarning(lcSession).noquote() << "replaceDataset (processors):" << QString::fromStdString(outputs.error());
    }
  }

  // (4b) A replace swaps the dataset's data wholesale and may rename or drop the
  // inputs a generator read, so re-run every generator bound to this dataset (not
  // just those whose changed-input names still match) and repaint if any set changed.
  if (marker_service_ && marker_service_->hasGenerators() &&
      !marker_service_->recomputeForDataset(primary_id).empty()) {
    notifyMarkersChanged();
  }

  // (5) Re-index the (already-cleared) adapters against the swapped-in data.
  notifyIngest(std::move(changed), /*live=*/false);
}

std::optional<DatasetMergeReport> SessionManager::mergeDatasets(
    DatasetId anchor, const std::vector<DatasetMergeSource>& sources) {
  // (1) Adapters bound to the anchor or any source drop cached TopicChunk* before
  // the engine clears/rebuilds chunks. Same-thread direct connections; this method
  // runs no event loop (the caller must not either), so the pointers stay dead.
  // The helper also invalidates the per-dataset/global min caches; object-only
  // merges may not notify any scalar topics later.
  notifyDatasetAboutToBeReplaced(anchor);
  for (const auto& source : sources) {
    notifyDatasetAboutToBeReplaced(source.dataset_id);
  }

  // (2) Fold the scalar data. The engine validates all caller/data-driven inputs
  // up front (unknown/duplicate/self source), so a reachable failure leaves the
  // engine untouched — log and return an empty report. (Its in-loop guards cover
  // only unreachable internal invariants.)
  DatasetMergeReport report;
  if (auto result = data_engine_.mergeDatasets(anchor, sources); result.has_value()) {
    report = std::move(*result);
  } else {
    qCWarning(lcSession).noquote() << "mergeDatasets:" << QString::fromStdString(result.error());
    return std::nullopt;  // engine rejected: nothing mutated — let the caller skip the catalog update
  }

  // The merge is now destructive and committed: no single contributor's
  // original descriptor can re-obtain the merged result, so invalidate the
  // provenance records of EVERY contributor — the anchor included. (Observed
  // but deliberately untouched: dataset_source_paths_ is likewise not cleaned
  // here; the caller owns the post-merge catalog/source bookkeeping. Records
  // are the only provenance this class owns end-to-end.)
  dataset_source_records_.erase(anchor);
  dataset_metadata_.erase(anchor);
  dataset_resources_.erase(anchor);
  for (const auto& source : sources) {
    dataset_source_records_.erase(source.dataset_id);
    dataset_metadata_.erase(source.dataset_id);
    dataset_resources_.erase(source.dataset_id);
  }

  // (3) Fold the object topics the same way, EXCEPT marker sets: those are
  // single-entry supersede topics at a sentinel timestamp, so the generic
  // interleave+retention fold would keep only one dataset's set (and strand it on
  // the wrong clock). MarkerService merges them set-aware right after (3b), while
  // the source datasets still hold their marker topics. This shares the scalar
  // merge's structural validation, so it cannot fail once the scalar merge above
  // succeeded on the same inputs.
  const auto exclude_markers = [](const ObjectTopicDescriptor& d) { return isMarkerObjectTopic(d.topic_name); };
  if (auto objects = object_store_.mergeDatasets(anchor, sources, exclude_markers); objects.has_value()) {
    // Shared-name source topics are now empty — their entries folded into the
    // anchor topic, which the anchor's own parser decodes. Evict those redundant
    // source-side topics AND their (now-orphaned) parser slots via evictObjectTopics
    // (which defers slot teardown past the lock, matching the reload path). Source-
    // only topics were reparented under the anchor and KEEP their ids + parsers, so
    // they are deliberately not evicted here.
    std::vector<ObjectTopicId> folded_sources;
    folded_sources.reserve(objects->remapped.size());
    for (const auto& [source_id, dest_id] : objects->remapped) {
      folded_sources.push_back(source_id);
    }
    evictObjectTopics(folded_sources);
  } else {
    qCWarning(lcSession).noquote() << "mergeDatasets(objects):" << QString::fromStdString(objects.error());
  }

  // (3b) Merge the marker sets set-aware (concatenate + shift onto the anchor
  // clock), before the sources are dropped from the catalog. Runs regardless of
  // live generators — markers may outlive the recipe that produced them.
  if (marker_service_) {
    marker_service_->mergeMarkerTopics(anchor, sources);
    // The sources leave the catalog right after this call, so a generator still bound
    // to one would resolve zero inputs from here on and quietly stop producing. Its
    // data now lives in the anchor — move the rule with it. Driven by the engine's own
    // record of what it folded, not by the request, so a source the engine declined to
    // consume keeps its generator.
    marker_service_->remapGeneratorsToAnchor(anchor, report.consumed_datasets);
  }

  // (4) Re-index adapters against the rebuilt anchor topics.
  QVector<TopicId> changed;
  changed.reserve(static_cast<int>(report.modified_topics.size() + report.added_topics.size()));
  for (const TopicId t : report.modified_topics) {
    changed.push_back(t);
  }
  for (const TopicId t : report.added_topics) {
    changed.push_back(t);
  }
  notifyIngest(std::move(changed), /*live=*/false);
  return report;
}

void SessionManager::registerObjectTopicParser(ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
  const bool incoming_valid = parser != nullptr && parser->valid();
  // The old slot is moved out under the lock and destroyed AFTER it releases:
  // its dtor runs MessageParserHandle teardown (potentially dlclose), which must
  // never run while holding object_parsers_mutex_.
  ObjectParserSlot replaced_slot;
  {
    std::unique_lock lock(object_parsers_mutex_);
    if (!incoming_valid) {
      // Do not silently drop a previously valid registration just because the
      // caller handed us an invalid replacement — that produced "topic suddenly
      // can't be decoded anymore" with no diagnostic. Keep the prior parser and
      // warn loudly so the operator can see something is wrong with the binding.
      const auto existing = object_topic_parsers_.find(id.id);
      if (existing != object_topic_parsers_.end() && existing->second.handle != nullptr &&
          existing->second.handle->valid()) {
        qCWarning(lcSession) << "registerObjectTopicParser: ignoring invalid replacement for topic" << id.id
                             << "(previous valid parser preserved)";
        return;
      }
      qCWarning(lcSession) << "registerObjectTopicParser: invalid parser for topic" << id.id
                           << "— erasing registration";
      if (existing != object_topic_parsers_.end()) {
        replaced_slot = std::move(existing->second);
        object_topic_parsers_.erase(existing);
      }
      return;
    }
    // Fresh mutex per registration: a re-registration swaps in a new parser, but
    // existing consumers still guard the old one. Reusing the lock would let the
    // new caller race with leftover work on the old parser pointer.
    ObjectParserSlot fresh{std::shared_ptr<MessageParserHandle>(std::move(parser)), std::make_shared<std::mutex>()};
    auto& slot = object_topic_parsers_[id.id];
    replaced_slot = std::move(slot);  // keep the old handle off the lock-held dtor path
    slot = std::move(fresh);
  }
  // replaced_slot destructs here, after the lock is released.
}

std::vector<SessionManager::ObjectParserSnapshotEntry> SessionManager::snapshotObjectTopicParsers(
    const std::vector<ObjectTopicId>& topic_ids) const {
  std::vector<ObjectParserSnapshotEntry> snapshot;
  snapshot.reserve(topic_ids.size());
  std::shared_lock lock(object_parsers_mutex_);
  for (const ObjectTopicId topic_id : topic_ids) {
    ObjectParserSnapshotEntry entry;
    entry.topic_id = topic_id.id;
    // Copy the slot verbatim — INCLUDING an invalid handle, so a later restore
    // reproduces the registration as it was rather than curating it.
    if (const auto it = object_topic_parsers_.find(topic_id.id); it != object_topic_parsers_.end()) {
      entry.slot = it->second;
    }
    snapshot.push_back(std::move(entry));
  }
  return snapshot;
}

void SessionManager::restoreObjectTopicParsers(std::vector<ObjectParserSnapshotEntry> snapshot) {
  // Slots the restore displaces are moved out under the lock and destroyed after
  // it releases: a slot dtor runs MessageParserHandle teardown (potentially
  // dlclose), which must never run while holding object_parsers_mutex_.
  std::vector<ObjectParserSlot> displaced;
  displaced.reserve(snapshot.size());
  {
    std::unique_lock lock(object_parsers_mutex_);
    for (ObjectParserSnapshotEntry& entry : snapshot) {
      if (const auto it = object_topic_parsers_.find(entry.topic_id); it != object_topic_parsers_.end()) {
        displaced.push_back(std::move(it->second));
        object_topic_parsers_.erase(it);
      }
      // The snapshot's own mutex comes back with the handle, so a consumer still
      // holding a binding from before keeps serializing against the same parser.
      if (entry.slot.handle != nullptr) {
        object_topic_parsers_.emplace(entry.topic_id, std::move(entry.slot));
      }
    }
  }
  // displaced destructs here, after the lock is released.
}

const SessionManager::ObjectParserSlot* SessionManager::findValidParserSlotLocked(ObjectTopicId id) const {
  auto it = object_topic_parsers_.find(id.id);
  if (it == object_topic_parsers_.end() || it->second.handle == nullptr || !it->second.handle->valid()) {
    return nullptr;
  }
  return &it->second;
}

SessionManager::ParserBinding SessionManager::parserBindingForObjectTopic(ObjectTopicId id) const {
  std::shared_lock lock(object_parsers_mutex_);
  const auto* slot = findValidParserSlotLocked(id);
  if (slot == nullptr) {
    return {};
  }
  // Copy the shared_ptrs out under the lock so the snapshot keeps the parser
  // alive even if the streaming worker replaces the slot right after we release.
  return ParserBinding{
      static_cast<MessageParserPluginBase*>(slot->handle->context()),
      slot->mutex,
      slot->handle,
  };
}

std::shared_ptr<void> SessionManager::parserKeepaliveForObjectTopic(ObjectTopicId id) const {
  // shared_ptr<MessageParserHandle> -> shared_ptr<void>: holding it keeps the
  // handle (and thus the parser instance + plugin DSO) alive for the consumer.
  std::shared_lock lock(object_parsers_mutex_);
  const auto* slot = findValidParserSlotLocked(id);
  return slot != nullptr ? slot->handle : nullptr;
}

MessageParserPluginBase* SessionManager::parserForObjectTopic(ObjectTopicId id) const {
  // Returns a RAW pointer with no keepalive: only safe for synchronous GUI-thread
  // use that does not outlive the call. Cross-thread / cached consumers must take
  // parserBindingForObjectTopic and hold its keepalive instead.
  std::shared_lock lock(object_parsers_mutex_);
  const auto* slot = findValidParserSlotLocked(id);
  return slot != nullptr ? static_cast<MessageParserPluginBase*>(slot->handle->context()) : nullptr;
}

std::shared_ptr<std::mutex> SessionManager::parserMutexForObjectTopic(ObjectTopicId id) const {
  std::shared_lock lock(object_parsers_mutex_);
  const auto* slot = findValidParserSlotLocked(id);
  return slot != nullptr ? slot->mutex : nullptr;
}

void SessionManager::recordLoadedSource(
    QString path, QString prefix, QString plugin_id, QString plugin_config_json, QString plugin_manifest_id) {
  LoadedSource source{
      normalizedSourcePath(path), std::move(prefix), std::move(plugin_id), std::move(plugin_config_json),
      std::move(plugin_manifest_id)};
  // Dedup by physical path (matching datasetSourcePath's normalization): a reload
  // — including a symlink/relative alias — updates its entry in place (keeping
  // list order) rather than appending a duplicate.
  const auto it = std::find_if(loaded_sources_.begin(), loaded_sources_.end(), [&source](const LoadedSource& existing) {
    return existing.path == source.path;
  });
  if (it != loaded_sources_.end()) {
    *it = std::move(source);
  } else {
    loaded_sources_.push_back(std::move(source));
  }
}

void SessionManager::evictDatasetObjects(DatasetId dataset_id) {
  evictObjectTopics(object_store_.listTopics(dataset_id));
}

void SessionManager::removeDataset(DatasetId dataset_id) {
  data_engine_.removeDataset(dataset_id);
  dataset_source_paths_.erase(dataset_id);
  // Provenance shares the source-path lifecycle: a removed dataset's record
  // must never resolve a future identity query (the record tier skips ids the
  // engine no longer knows, but a reminted id could collide).
  dataset_source_records_.erase(dataset_id);
  dataset_metadata_.erase(dataset_id);
  dataset_resources_.erase(dataset_id);
  // A generator bound to this dataset outlives its data otherwise: the object topics
  // go with the dataset, but the recipe keeps naming the dead id, so the next
  // recompute resolves nothing and re-registers a phantom marker topic under it.
  if (marker_service_ && !marker_service_->clearGeneratorsForDataset(dataset_id).empty()) {
    notifyMarkersChanged();
  }
  // The engine no longer holds this dataset; drop its pinned earliest-sample and
  // the memoized cross-dataset origin so globalTimeReference() re-scans the
  // survivors (removing the earliest dataset must re-base the display origin).
  invalidateDatasetMinTimestamp(dataset_id);
  // If that re-base actually moved the origin, every surviving curve adapter now
  // holds a stale display offset — no per-topic samplesIngested covers a pure
  // origin shift, so signal the global reframe. No-op when "Use time offset" is
  // off (globalTimeReference() is 0 both times).
  notifyGlobalTimeReferenceIfChanged();
}

void SessionManager::refreshDatasetTimeReference(DatasetId dataset_id) {
  // Drop the pinned min so globalTimeReference() re-scans this dataset's current
  // data (the terminal flush may have committed data earlier than the running pin).
  invalidateDatasetMinTimestamp(dataset_id);
  notifyGlobalTimeReferenceIfChanged();
}

std::unordered_set<DatasetId> SessionManager::datasetsOwningObjectTopics(
    const std::vector<ObjectTopicId>& topic_ids) const {
  std::unordered_set<DatasetId> affected_datasets;
  affected_datasets.reserve(topic_ids.size());
  for (const ObjectTopicId topic_id : topic_ids) {
    const DatasetId dataset_id = object_store_.descriptor(topic_id).dataset_id;
    if (dataset_id != 0) {
      affected_datasets.insert(dataset_id);
    }
  }
  return affected_datasets;
}

void SessionManager::evictObjectTopics(const std::vector<ObjectTopicId>& topic_ids) {
  // Object samples participate in the cross-dataset raw origin. Remember every
  // affected dataset BEFORE its descriptor disappears, then invalidate their
  // pinned minima after removal (invalidating only these keeps unrelated
  // retention pins intact).
  const std::unordered_set<DatasetId> affected_datasets = datasetsOwningObjectTopics(topic_ids);

  // removeTopic touches the ObjectStore, not the parser map, so keep it out of
  // the parser lock. Erased slots are collected and destroyed after the lock
  // releases: a slot dtor may run plugin teardown (dlclose), which must not run
  // under object_parsers_mutex_.
  std::vector<ObjectParserSlot> erased_slots;
  erased_slots.reserve(topic_ids.size());
  for (const ObjectTopicId topic_id : topic_ids) {
    object_store_.removeTopic(topic_id);
    // Topic gone: drop its parser too, so a reload (fresh ObjectTopicId) re-registers cleanly.
    std::unique_lock lock(object_parsers_mutex_);
    if (const auto it = object_topic_parsers_.find(topic_id.id); it != object_topic_parsers_.end()) {
      erased_slots.push_back(std::move(it->second));
      object_topic_parsers_.erase(it);
    }
  }
  for (const DatasetId dataset_id : affected_datasets) {
    invalidateDatasetMinTimestamp(dataset_id);
  }
  notifyGlobalTimeReferenceIfChanged();
  // erased_slots destructs here, after the last unlock.
}

void SessionManager::clearAllObjects() {
  // Same origin concern as evictObjectTopics: collect every dataset that owned an
  // object topic before the store is cleared, so their pinned minima can be
  // invalidated and the global origin re-scanned over the survivors.
  const std::unordered_set<DatasetId> affected_datasets = datasetsOwningObjectTopics(object_store_.listTopics());
  object_store_.clear();
  // Swap the map into a local under the lock, then let it destruct after the
  // lock releases — slot dtors may run plugin teardown (dlclose).
  std::unordered_map<uint32_t, ObjectParserSlot> drained;
  {
    std::unique_lock lock(object_parsers_mutex_);
    drained.swap(object_topic_parsers_);
  }
  for (const DatasetId dataset_id : affected_datasets) {
    invalidateDatasetMinTimestamp(dataset_id);
  }
  notifyGlobalTimeReferenceIfChanged();
  // drained destructs here, after the lock is released.
}

}  // namespace PJ
