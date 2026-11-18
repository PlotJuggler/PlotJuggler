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
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/sdk/dataset_qualified_name.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/saturating_time.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/DerivedTopics.h"
#include "pj_runtime/EnginePool.h"
#include "pj_runtime/InFlightBudget.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/TransformService.h"
#include "pj_scripting/filter_catalogue.h"
#include "pj_scripting/script_engine.h"
#include "pj_scripting/video_frame_source.h"
#include "pj_tf/tf_buffer.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcSession, "pj.runtime.session")
// Off by default; enable with QT_LOGGING_RULES="pj.runtime.ingest.cancel=true"
// to time a stop from click to terminal.
Q_LOGGING_CATEGORY(lcCancel, "pj.runtime.ingest.cancel", QtWarningMsg)

// How long a producer may take to honour a stop before we say so.
constexpr int kStopWatchdogMs = 2000;

/// An on-demand input split at the loaded source it starts with, and the datasets it can bind on: those loaded from
/// that source, or every dataset when unqualified, a dataset merged away counting as its anchor.
struct LoadedQualifierSplit {
  sdk::DatasetQualifierSplit split;
  std::vector<DatasetId> datasets;
};

LoadedQualifierSplit splitAgainstLoaded(
    const DataEngine& engine, const std::unordered_map<DatasetId, DatasetId>& merged_into, std::string_view input) {
  const auto datasets = engine.listDatasets();
  std::vector<std::string> sources;
  for (const DatasetId dataset : datasets) {
    const DatasetInfo* info = engine.getDataset(dataset);
    sources.push_back(info != nullptr ? info->source_name : std::string{});
  }
  LoadedQualifierSplit loaded;
  loaded.split = sdk::splitDatasetQualifier(input, sources);
  for (size_t index = 0; index < datasets.size(); ++index) {
    if (loaded.split.qualified && sources[index] != loaded.split.dataset_source) {
      continue;
    }
    const auto merged = merged_into.find(datasets[index]);
    const DatasetId bound = merged != merged_into.end() ? merged->second : datasets[index];
    if (std::ranges::find(loaded.datasets, bound) == loaded.datasets.end()) {
      loaded.datasets.push_back(bound);
    }
  }
  return loaded;
}

}  // namespace

QString SessionManager::normalizedSourcePath(const QString& path) {
  if (path.isEmpty()) {
    return {};
  }
  // Non-file identities (a browser upload lease) are logical ids: QFileInfo would prepend the working directory.
  if (path.contains(u"://"_s)) {
    return path;
  }
  const QFileInfo info(path);
  const QString canonical = info.canonicalFilePath();
  if (!canonical.isEmpty()) {
    return QDir::cleanPath(canonical);
  }
  // A deleted file has no canonical path, but its folder usually exists: resolving the folder keeps the stored path
  // matching behind a symlinked folder (macOS /var -> /private/var, a symlinked home).
  const QString folder = info.absoluteDir().canonicalPath();
  return QDir::cleanPath(folder.isEmpty() ? info.absoluteFilePath() : folder + u'/' + info.fileName());
}

// Session-wide cap on ingest-seeded resident object payloads: covers the live-edge window a scene chases during a
// progressive load. The browser keeps a quarter (TECHNICAL_NOTES.md "Browser resident payload pool").
#ifdef PJ_TARGET_WASM
constexpr size_t kResidentPayloadPoolBytes = 64ULL * 1024 * 1024;
#else
constexpr size_t kResidentPayloadPoolBytes = 256ULL * 1024 * 1024;
#endif

SessionManager::SessionManager(QObject* parent)
    : QObject(parent), ingest_taps_(std::make_shared<ObjectIngestTapRegistry>()) {
  static const auto dummy_ingest_token = qRegisterMetaType<PJ::IngestToken>("PJ::IngestToken");
  static const auto dummy_ingest_outcome = qRegisterMetaType<PJ::IngestOutcome>("PJ::IngestOutcome");
  (void)dummy_ingest_token;
  (void)dummy_ingest_outcome;

  // Live-edge object reads during and after a file load reuse the bytes the ingest fetched instead of re-fetching
  // (for MCAP, re-decompressing a chunk).
  resident_payload_pool_ = std::make_shared<ResidentPayloadPool>(kResidentPayloadPoolBytes);
  object_store_.setResidentPayloadPool(resident_payload_pool_);

  processor_service_ = std::make_unique<DataProcessorService>(*this);

  // AppSession injects the real series resolver once the catalog exists; this stub resolves nothing. A generator
  // restored from a layout runs as soon as it is re-created, so a resolver injected after the restore would run every
  // restored rule against no data.
  batch_engine_ =
      std::make_unique<BatchEngine>(object_store_, [](DatasetId, const std::string&, const BatchEngine::Window&) {
        return std::optional<BatchEngine::ResolvedSeries>{};
      });
  engine_pool_ = std::make_unique<EnginePool>();
  in_flight_budget_ = std::make_unique<InFlightBudget>(*engine_pool_);
  batch_engine_->setPool(engine_pool_.get());
  batch_engine_->setInFlightBudget(in_flight_budget_.get());

  OnDemandService::Services on_demand;
  // An input names an object topic or a scalar series of a dataset, maybe behind its "source:" qualifier. Never an
  // on-demand output: the graph binds those itself.
  on_demand.resolver = [this](std::string_view input) {
    const auto [split, datasets] = splitAgainstLoaded(data_engine_, merged_into_, input);
    std::vector<RecipeGraph::InputCandidate> candidates;
    for (const DatasetId dataset : datasets) {
      if (const std::optional<ObjectTopicId> topic = object_store_.findTopic(dataset, split.bare)) {
        const ObjectTopicDescriptor descriptor = object_store_.descriptor(*topic);
        const sdk::BuiltinObjectType type = objectTypeFromMetadata(descriptor.metadata_json);
        if (type != sdk::BuiltinObjectType::kNone && !isDerivedObjectTopic(descriptor)) {
          candidates.push_back({dataset, type, split.bare});
        }
      } else if (scalarInput(dataset, std::string(split.bare))) {
        candidates.push_back({dataset, sdk::BuiltinObjectType::kNone, split.bare});
      }
    }
    return candidates;
  };
  // A qualifier read as the resolver reads it (`RecipeGraph::QualifierSplitter`).
  on_demand.qualifier = [this](std::string_view input) -> std::optional<RecipeGraph::Qualifier> {
    LoadedQualifierSplit loaded = splitAgainstLoaded(data_engine_, merged_into_, input);
    if (!loaded.split.qualified) {
      return std::nullopt;
    }
    return RecipeGraph::Qualifier{std::move(loaded.split.bare), std::move(loaded.datasets)};
  };
  on_demand.scalars =
      [this](DatasetId dataset, std::string_view name, Timestamp t) -> std::optional<InputStager::ScalarSample> {
    const auto series = scalarInput(dataset, std::string(name));
    return series ? InputStager::sampleAt(data_engine_, series->first, series->second, t) : std::nullopt;
  };
  on_demand.scalar_changes =
      [this](DatasetId dataset, std::string_view name, Timestamp start, Timestamp end, std::size_t limit) {
        GridRunner::ScalarChanges changes;
        const auto series = scalarInput(dataset, std::string(name));
        if (!series) {
          return changes;
        }
        const DataReader reader = createReader();
        const auto values = reader.series(series->first, series->second);
        // ponytail: `series` reads numbers, so a string series reports no change and GridRunner refuses a window that
        // reads one. Upgrade: walk its rows here (their stamps, the first retained row, the limit).
        if (!values) {
          changes.unreadable = reader.columnType(series->first, series->second) == PrimitiveType::kString
                                   ? GridRunner::ScalarChanges::Unreadable::kString
                                   : GridRunner::ScalarChanges::Unreadable::kNotASeries;
          return changes;
        }
        if (const auto first = values->sampleAtOrAfterTime(std::numeric_limits<Timestamp>::min())) {
          changes.first = first->timestamp;
        }
        for (SeriesCursor cursor = values->samples(Range<Timestamp>{start, end}); cursor.valid(); cursor.advance()) {
          if (changes.instants.size() == limit) {
            changes.over_limit = true;
            changes.instants.clear();
            break;
          }
          changes.instants.push_back(cursor.current().timestamp);
        }
        return changes;
      };
  on_demand.parsers = [this](ObjectTopicId topic) { return parserBindingForObjectTopic(topic); };
  on_demand.scheduling.image_decoder = [this] { return image_decoder_; };
  on_demand.scheduling.video_source = [this] { return video_source_; };
  on_demand.scheduling.tf = [this](DatasetId dataset) { return onDemandTf(dataset); };
  on_demand.scheduling.tf_revision = [this](DatasetId dataset) { return onDemandTfRevision(dataset); };
  on_demand.scheduling.epoch = [this](DatasetId dataset) {
    const auto epoch = epochs_.find(dataset);
    return epoch != epochs_.end() ? epoch->second : uint64_t{0};
  };
  on_demand.scheduling.published = [this](const std::string& key) {
    emit derivedEntriesPublished(QString::fromStdString(key), on_demand_->history().topicsOf(key));
  };
  on_demand.scheduling.state_changed = [this](const std::string& key) {
    emit derivedStateChanged(QString::fromStdString(key), on_demand_->history().topicsOf(key));
  };
  on_demand.installed = [this](const std::string& key, const std::vector<ObjectTopicId>& retired) {
    if (!retired.empty()) {
      emit objectTopicsChanged();
    }
    emit onDemandRecipeInstalled(QString::fromStdString(key));
  };
  // A series' rows land in the DataEngine: announced like a transform's grown output, so plots and the catalog follow.
  on_demand.series_changed = [this](std::vector<TopicId> topics) {
    notifyIngest(QVector<TopicId>(topics.begin(), topics.end()), /*live=*/false);
  };
  on_demand.series_state_changed = [this](const std::string& key) {
    emit seriesStateChanged(QString::fromStdString(key));
  };
  on_demand_ = std::make_unique<OnDemandService>(
      object_store_, data_engine_, *engine_pool_, *in_flight_budget_, std::move(on_demand));
  // A whole-series read (a plugin's `read_series_arrow`, a marker rule's input) on the GUI thread finishes a
  // transform's replay first and requests an on-demand series (the reader gets the rows so far). A read on another
  // thread gets the transform's current rows (no blocking hop) and posts its series request.
  data_engine_.setCompleteReadBarrier([this, owner = QThread::currentThread()](TopicId topic) {
    if (QThread::currentThread() != owner) {
      QMetaObject::invokeMethod(
          this, [this, topic] { on_demand_->series().requestTopic(topic); }, Qt::QueuedConnection);
      return;
    }
    processor_service_->finishPendingMaterialization(topic);
    on_demand_->series().requestTopic(topic);
  });
  connect(this, qOverload<>(&SessionManager::displayOffsetChanged), this, &SessionManager::pushSeriesVisibleStarts);
  connect(
      this, qOverload<DatasetId>(&SessionManager::displayOffsetChanged), this, [this] { pushSeriesVisibleStarts(); });
  // A scheduled run lands after the change that asked for it was announced.
  connect(batch_engine_.get(), &BatchEngine::markersPublished, this, &SessionManager::notifyMarkersChanged);
  // A changed transform output (edit, finished replay, removal) makes the marker rules reading it stale. A restore's
  // own changes name only the outputs it replaces, so they never touch a rule it keeps.
  processor_service_->setOutputsChangedSink([this](const std::vector<std::string>& names, DatasetId dataset) {
    if (batch_engine_ && batch_engine_->hasGenerators()) {
      (void)batch_engine_->recomputeForChangedInputs(names, dataset);
    }
  });
  // An all-datasets set is stored in the display frame: each contributor's run is shifted by its alignment offset
  // (globalTimeReference is uniform and cancels out).
  batch_engine_->setDisplayOffsetResolver(
      [this](DatasetId dataset_id) { return static_cast<Timestamp>(sourceDisplayOffset(dataset_id).value.count()); });

  // The bundled Luau filter catalogue, so applied and restored filters resolve. The resource is absent only in a
  // headless unit test, which installs its own catalogue if it needs filters.
  auto catalogue = std::make_shared<scripting::FilterCatalogue>(scripting::makeLuauEngine());
  if (QFile f(u":/filters/builtin_filters.luau"_s); f.open(QIODevice::ReadOnly)) {
    if (auto added = catalogue->addBundledSource(f.readAll().toStdString(), "bundled"); !added.has_value()) {
      qCWarning(lcSession) << "filter catalogue load failed:" << QString::fromStdString(added.error());
    }
  }
  if (!catalogue->entries().empty()) {
    processor_service_->setFilterCatalogue(std::move(catalogue));
  }
}

SessionManager::~SessionManager() = default;

void SessionManager::setScalarSeriesLookup(ScalarSeriesLookup lookup) {
  scalar_lookup_ = std::move(lookup);
}

std::optional<std::pair<TopicId, std::size_t>> SessionManager::scalarInput(
    DatasetId dataset, const std::string& name) const {
  if (!scalar_lookup_) {
    return std::nullopt;
  }
  // The exact topic first, as a transform input resolves: `sensor/value` names that topic, not `sensor`'s field.
  {
    // ponytail: scans the dataset's topics at every request of a scalar input; index by name if it shows.
    const auto lock = data_engine_.lockEngine();
    for (const TopicId topic : data_engine_.listTopics(dataset)) {
      const TopicStorage* storage = data_engine_.getTopicStorage(topic);
      if (storage != nullptr && storage->descriptor().name == name) {
        return std::pair<TopicId, std::size_t>{topic, 0};
      }
    }
  }
  return scalar_lookup_(dataset, name);
}

std::unordered_set<TopicId> SessionManager::derivedScalarTopics(bool ephemeral_only) const {
  std::unordered_set<TopicId> topics =
      ephemeral_only ? processor_service_->ephemeralOutputTopics() : processor_service_->processorOutputTopics();
  topics.merge(on_demand_->series().topics(ephemeral_only));
  return topics;
}

void SessionManager::setTransformService(TransformService* service) {
  transform_service_ = service;
  held_tf_.clear();
}

void SessionManager::setTfClock(std::function<std::chrono::steady_clock::time_point()> clock) {
  tf_clock_ = std::move(clock);
}

void SessionManager::suspendOnDemand(DatasetId dataset_id) {
  ++epochs_[dataset_id];
  held_tf_.erase(dataset_id);
  if (video_source_) {
    video_source_->retireDataset(dataset_id);
  }
  on_demand_->suspend(dataset_id);
}

void SessionManager::resumeOnDemandLater(DatasetId dataset_id) {
  QMetaObject::invokeMethod(this, [this, dataset_id] { on_demand_->resume(dataset_id); }, Qt::QueuedConnection);
}

void SessionManager::rebindOnDemandAfterMerge(
    DatasetId anchor, const std::unordered_map<DatasetId, Timestamp>& shifts) {
  // One snapshot, producers first: a reader re-binds once its upstream is on the anchor.
  const RecipeGraph& graph = on_demand_->graph();
  std::map<std::string, Timestamp> shift_of;
  for (const auto& [dataset, shift] : shifts) {
    for (std::string& key : graph.keysOf(dataset)) {
      shift_of.emplace(std::move(key), shift);
    }
  }
  std::vector<std::pair<RecipeGraph::Recipe, Timestamp>> order;
  std::unordered_set<std::string> visited;
  const std::function<void(const std::string&)> visit = [&](const std::string& key) {
    if (!shift_of.contains(key) || !visited.insert(key).second) {
      return;
    }
    const RecipeGraph::Node* node = graph.find(key);
    for (const RecipeGraph::Binding& binding : node->bindings) {
      visit(binding.upstream);
    }
    order.emplace_back(node->recipe, shift_of.at(key));
  };
  for (const auto& entry : shift_of) {
    visit(entry.first);
  }
  for (const auto& [dataset, shift] : shifts) {
    if (dataset != anchor) {
      for (auto& [consumed, bound] : merged_into_) {
        if (bound == dataset) {
          bound = anchor;
        }
      }
      merged_into_[dataset] = anchor;
    }
  }
  for (auto& [recipe, raw_shift] : order) {
    const std::string key = recipe.key();
    // Every recipe here ran on the anchor or a consumed dataset. Pinning it keeps one a layout bound by uniqueness (no
    // dataset of its own) from being re-resolved, and removed as ambiguous when another dataset carries its inputs.
    recipe.dataset = anchor;
    if (recipe.pinned_t) {
      recipe.pinned_t = saturatingAdd(*recipe.pinned_t, raw_shift);
    }
    const std::optional<NodeScheduler::NodeState> before = on_demand_->state(key);
    if (const auto installed = on_demand_->install(std::move(recipe)); installed) {
      if (before && before->status != NodeScheduler::NodeState::Status::kIdle) {
        on_demand_->requestAt(key, saturatingAdd(before->requested_t, raw_shift));  // its cursor, on the anchor's clock
      }
    } else {
      qCWarning(lcSession).noquote() << "mergeDatasets: on-demand recipe" << QString::fromStdString(key)
                                     << "no longer resolves and was removed:"
                                     << QString::fromStdString(installed.error());
      if (!on_demand_->remove(key).empty()) {
        emit objectTopicsChanged();
      }
    }
  }
}

std::shared_ptr<const pj::tf::TransformSnapshot> SessionManager::onDemandTf(DatasetId dataset_id) {
  if (transform_service_ == nullptr) {
    return nullptr;
  }
  if (!tfIngesting(dataset_id)) {
    held_tf_.erase(dataset_id);
    return transform_service_->snapshot(dataset_id);
  }
  // A snapshot per request would make every TF write of the ingest copy the growing state.
  const auto now = tf_clock_();
  HeldTf& held = held_tf_[dataset_id];
  if (held.snapshot == nullptr || now - held.taken >= kTfSnapshotPeriod) {
    held = {transform_service_->snapshot(dataset_id), now};
  }
  return held.snapshot;
}

std::optional<std::pair<Timestamp, Timestamp>> SessionManager::derivedTopicInputRange(ObjectTopicId topic) const {
  const std::optional<std::string> key = on_demand_->keyOfTopic(topic);
  if (!key) {
    return std::nullopt;
  }
  std::pair<Timestamp, Timestamp> range{std::numeric_limits<Timestamp>::max(), std::numeric_limits<Timestamp>::min()};
  const auto widen = [&range](Timestamp lo, Timestamp hi) {
    range = {std::min(range.first, lo), std::max(range.second, hi)};
  };
  const DataReader reader = createReader();
  std::vector<std::string> pending{*key};
  std::unordered_set<std::string> seen;
  while (!pending.empty()) {
    const std::string at = std::move(pending.back());
    pending.pop_back();
    const RecipeGraph::Node* node = on_demand_->graph().find(at);
    // A finding (pinned) covers no clock range: neither as the topic's node nor as an upstream.
    if (node == nullptr || node->recipe.pinned_t || !seen.insert(at).second) {
      continue;
    }
    for (const RecipeGraph::Binding& binding : node->bindings) {
      if (!binding.upstream.empty()) {
        pending.push_back(binding.upstream);
      } else if (binding.object_type != sdk::BuiltinObjectType::kNone) {
        const std::optional<ObjectTopicId> input = object_store_.findTopic(node->dataset, binding.name);
        if (input && object_store_.entryCount(*input) != 0) {
          const auto [lo, hi] = object_store_.timeRange(*input);
          widen(lo, hi);
        }
      } else if (const auto series = scalar_lookup_ ? scalar_lookup_(node->dataset, binding.name) : std::nullopt) {
        if (const auto metadata = reader.getMetadata(series->first); metadata && metadata->total_row_count != 0) {
          widen(metadata->time_range_min, metadata->time_range_max);
        }
      }
    }
  }
  return range;
}

uint64_t SessionManager::onDemandTfRevision(DatasetId dataset_id) {
  if (transform_service_ == nullptr) {
    return 0;
  }
  if (const auto held = held_tf_.find(dataset_id);
      held != held_tf_.end() && tfIngesting(dataset_id) && tf_clock_() - held->second.taken < kTfSnapshotPeriod) {
    return held->second.snapshot->revision();
  }
  return transform_service_->transformBuffer(dataset_id)->revision();
}

bool SessionManager::tfIngesting(DatasetId dataset_id) const {
  return ingestActive(dataset_id) || isLive(dataset_id);
}

bool SessionManager::isLive(DatasetId dataset_id) const {
  const auto live = live_at_.find(dataset_id);
  return live != live_at_.end() && tf_clock_() - live->second < kTfSnapshotPeriod;
}

DataReader SessionManager::createReader() const {
  return data_engine_.createReader();
}

void SessionManager::setDatasetSourcePath(DatasetId dataset_id, QString path) {
  path = normalizedSourcePath(path);
  if (path == datasetSourcePath(dataset_id)) {
    return;  // an absent entry reads as empty
  }
  if (path.isEmpty()) {
    dataset_source_paths_.erase(dataset_id);
  } else {
    dataset_source_paths_.insert_or_assign(dataset_id, std::move(path));
  }
  emit datasetSourcePathChanged(dataset_id);
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
  // normalizedSourcePath resolves a deleted file through its folder, so it still compares equal to the path registered
  // while it existed; only a source whose folder is gone too can miss a registration made through a symlink.
  const QString normalized_saved_path = normalizedSourcePath(saved_path);
  const auto source_matches = [&saved_source](const DatasetInfo* info) {
    return info != nullptr && (saved_source.isEmpty() || QString::fromStdString(info->source_name) == saved_source);
  };
  const auto path_matches = [this, &normalized_saved_path](DatasetId id) {
    return normalized_saved_path.isEmpty() || datasetSourcePath(id) == normalized_saved_path;
  };
  const bool record_supplied = !saved_record.provider_id.isEmpty() || !saved_record.source_identity.isEmpty() ||
                               !saved_record.descriptor_json.isEmpty();
  // (provider_id, source_identity) is only the fast path (the digest could collide or drift): a match also needs a
  // byte-equal descriptor_json. An unsupplied record constrains nothing.
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

  // A unique confirmed record match outranks the path and name tiers; several are ambiguous.
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

  // No portable qualifier: valid only within the session (the exact id), no remint fallback.
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
  // Read the live time-domain map: the dataset's own time_domain is a copy taken at createDataset and goes stale after
  // setDisplayOffset.
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
  return DisplayOffset{Duration{datasetDomainDisplayOffset(dataset_id)}};
}

DisplayOffset SessionManager::displayOffset(DatasetId dataset_id) const {
  return DisplayOffset{sourceDisplayOffset(dataset_id).value + Duration{globalTimeReference()}};
}

std::optional<DisplaySeconds> SessionManager::displayTimeForSource(DatasetId dataset_id, int64_t absolute_ns) const {
  if (dataset_id == 0 || data_engine_.getDataset(dataset_id) == nullptr) {
    return std::nullopt;
  }
  // Seek-safe: a seek to the returned time shows the sample at `absolute_ns`, not the one before it.
  return rawToDisplaySecondsAtOrAfter(absolute_ns, displayOffset(dataset_id));
}

Timestamp SessionManager::globalTimeReference() const {
  // Per-dataset minima are pinned, so live retention cannot slide the display origin forward.
  if (!use_time_offset_) {
    return 0;
  }
  if (!global_min_cache_.has_value()) {
    Timestamp global_min = std::numeric_limits<Timestamp>::max();
    bool found = false;
    for (const DatasetId dataset_id : data_engine_.listDatasets()) {
      if (isStaticReferenceDataset(dataset_id)) {
        continue;
      }
      // Trusts the per-dataset pins (see global_min_cache_ in the header): a path that can raise a dataset's minimum
      // must invalidate its pin and this memo.
      if (const auto it = dataset_min_cache_.find(dataset_id); it != dataset_min_cache_.end()) {
        global_min = std::min(global_min, it->second);
        found = true;
      } else if (const auto bounds = datasetRawBounds(dataset_id); bounds.has_value()) {
        // Unpinned: pin it now (only ever lowers). An empty dataset contributes no 0 origin.
        global_min = std::min(global_min, rememberDatasetMinTimestamp(dataset_id, bounds->first));
        found = true;
      }
    }
    global_min_cache_ = found ? global_min : 0;
  }
  return *global_min_cache_;
}

std::optional<std::pair<Timestamp, Timestamp>> SessionManager::datasetRawBounds(DatasetId dataset_id) const {
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
    if (isOffClockTopic(object_store_.descriptor(object_topic_id))) {
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

bool SessionManager::isStaticReferenceDataset(DatasetId dataset_id) const {
  for (const auto topic : object_store_.listTopics(dataset_id)) {
    if (object_store_.entryCount(topic) == 0) {
      continue;
    }
    const auto& descriptor = object_store_.descriptor(topic);
    if ((descriptor.metadata_json.find("kPointCloud") == std::string::npos &&
         descriptor.metadata_json.find("kMesh3D") == std::string::npos) ||
        isOffClockTopic(descriptor)) {
      continue;
    }
    const auto metadata = QJsonDocument::fromJson(QByteArray::fromStdString(descriptor.metadata_json)).object();
    const auto type = metadata.value(u"builtin_object_type"_s).toString();
    if (type == u"kPointCloud"_s || type == u"kMesh3D"_s) {
      const auto [first, last] = object_store_.timeRange(topic);
      if (first != 0 || last != 0) {
        return false;
      }
      const auto bounds = datasetRawBounds(dataset_id);
      return bounds && bounds->first == 0 && bounds->second == 0;
    }
  }
  return false;
}

Timestamp SessionManager::datasetMinTimestamp(DatasetId dataset_id) const {
  // Pinned because displayOffset() is hot, and never moved forward when retention raises the readable minimum: the
  // live edge keeps advancing instead of every retained window projecting back to 0..buffer_seconds.
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

std::unordered_set<DatasetId> SessionManager::refreshDatasetMinTimestampsForTopics(const QVector<TopicId>& ids) const {
  // Reset up front so the empty-but-live notify path (early return below) resets it too.
  global_min_cache_.reset();
  if (ids.isEmpty()) {
    return {};
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
  return dataset_ids;
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
  // Emits even when the session is empty and the origin is 0: the Timeline's number formatting still changes.
  // Recording the origin stops notifyGlobalTimeReferenceIfChanged re-emitting the same value on the next ingest.
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
  const DisplayOffset offset = displayOffset(dataset_id);
  return DisplayRange{
      .min = rawToDisplaySecondsAtOrAfter(bounds->first, offset),
      .max = rawToDisplaySecondsAtOrAfter(bounds->second, offset)};
}

void SessionManager::setSeriesVisibleStart(DatasetId dataset_id, DisplaySeconds start) {
  series_visible_start_.insert_or_assign(dataset_id, start);
  on_demand_->series().setVisibleStart(dataset_id, displaySecondsToRaw(start, displayOffset(dataset_id)));
}

void SessionManager::pushSeriesVisibleStarts() {
  for (const auto& [dataset_id, start] : series_visible_start_) {
    on_demand_->series().setVisibleStart(dataset_id, displaySecondsToRaw(start, displayOffset(dataset_id)));
  }
}

void SessionManager::setDisplayOffset(DatasetId dataset_id, DisplayOffset offset) {
  // Write the TimeDomain, not the dataset's copy: displayOffset() reads the domain live.
  const DatasetInfo* dataset = data_engine_.getDataset(dataset_id);
  if (dataset == nullptr || dataset->time_domain.id == 0) {
    qCWarning(lcSession) << "setDisplayOffset: unknown dataset or default domain" << dataset_id;
    return;
  }
  // Unchanged offsets emit nothing: every displayOffsetChanged costs a PlotWidget adapter drop, a replot and a
  // Timeline refresh (resetAll over zero offsets, a settled drag re-sending its value).
  if (sourceDisplayOffset(dataset_id).value == offset.value) {
    return;
  }
  data_engine_.setDisplayOffset(dataset->time_domain.id, static_cast<Timestamp>(offset.value.count()));
  emit displayOffsetChanged(dataset_id);
}

std::vector<TopicId> SessionManager::commitChunks(std::vector<std::pair<TopicId, TopicChunk>> chunks) {
  auto changed = data_engine_.commitChunks(std::move(chunks));
  if (changed.empty()) {
    return changed;
  }
  // Filters run over the committed input; notify the raw and the derived topics so filtered curves refresh.
  const std::vector<TopicId> derived_outputs =
      processor_service_ ? processor_service_->advanceOnCommit(changed) : std::vector<TopicId>{};

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
  // Committed samples can lower the global origin (a later file starting earlier); reframe before the per-topic
  // signal, which never covers a pure origin shift.
  notifyGlobalTimeReferenceIfChanged();
  emit samplesIngested(std::move(ids), /*live=*/false);
  return changed;
}

void SessionManager::notifyIngest(QVector<TopicId> ids, bool live, std::optional<DatasetId> dataset) {
  std::unordered_set<DatasetId> datasets = refreshDatasetMinTimestampsForTopics(ids);  // also resets global_min_cache_
  // Object-only ingest carries no scalar TopicIds: recompute the origin before the empty, non-live signal is dropped.
  notifyGlobalTimeReferenceIfChanged();
  emit dataPublished();
  if (ids.isEmpty() && !live) {
    return;
  }
  if (live) {
    if (dataset) {
      datasets.insert(*dataset);
    }
    const auto now = tf_clock_();
    for (const DatasetId live_dataset : datasets) {
      live_at_[live_dataset] = now;
      on_demand_->followLiveEdge(live_dataset);
    }
  }
  emit samplesIngested(std::move(ids), live);
}

void SessionManager::notifyStreamEnded(DatasetId dataset_id) {
  on_demand_->series().endLive(dataset_id);
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
  const IngestId new_id = next_ingest_id_++;
  const IngestToken token{new_id, dataset_id};
  const bool discardable = stop == IngestStop::kKeepOrDiscard;

  // A restart supersedes the dataset's running ingest, reported as a kCancelled terminal: otherwise observers would
  // derive the supersede from the next ingestBegan and leak state bound to the old token until they did.
  IngestToken superseded_token{};
  if (const auto superseded = active_ingests_.find(dataset_id); superseded != active_ingests_.end()) {
    superseded_token = IngestToken{superseded->second.id, dataset_id};
  }

  // Install the replacement before the terminal: the emit is synchronous and re-entrant, and an observer calling back
  // in must find the dataset's lifecycle continuous. The old token is already stale (liveEntry matches the id).
  active_ingests_.insert_or_assign(
      dataset_id, ActiveIngestEntry{
                      new_id, label, /*current=*/0, total, cancellable, discardable,
                      /*cancel_requested=*/false, /*cancel_requested_ms=*/0, std::move(on_cancel)});
  if (superseded_token.id != 0) {
    emit ingestEnded(superseded_token, IngestOutcome::kCancelled);
  }

  // An observer may have begun another ingest on this dataset inside the terminal above, superseding this entry and
  // reporting its terminal. A began now would follow the successor's, binding observers to a dead token; suppressing
  // it loses nothing, since no observer saw this token.
  if (liveEntry(token) == nullptr) {
    qCInfo(lcCancel) << "ingest" << new_id << "on dataset" << dataset_id
                     << "was superseded from inside its own supersede terminal; its began is suppressed";
    return token;
  }

  // No data publication: the catalog would rebuild before the loader's commit seam; a dataset without a row yet is
  // shown by the controller's ghost row until its first flush.
  emit ingestBegan(token, label, total, cancellable, discardable);
  return token;
}

void SessionManager::updateIngest(IngestToken token, quint64 current, quint64 total) {
  // Publish the flushed rows ahead of every gate below: data is never dropped because a lifecycle call arrived out of
  // order, the token was superseded mid-flush, or no beginIngest ran. Only the lifecycle signal is gated.
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
  // Also after a stop was requested: the producer did deliver these rows. Freezing a stopping row is presentation
  // policy (IngestProgressController).
  emit ingestProgressed(token, current, total);
}

void SessionManager::endIngest(IngestToken token, IngestOutcome outcome) {
  ActiveIngestEntry* entry = liveEntry(token);
  if (entry == nullptr) {
    return;  // stale (superseded by a newer begin) or already terminal
  }

  if (entry->cancel_requested) {
    outcome = IngestOutcome::kCancelled;
  }

  if (entry->cancel_requested_ms != 0) {
    qCInfo(lcCancel) << "ingest" << token.id << "terminated"
                     << (QDateTime::currentMSecsSinceEpoch() - entry->cancel_requested_ms)
                     << "ms after the stop was requested; outcome" << static_cast<int>(outcome);
  }

  active_ingests_.erase(token.dataset_id);
  held_tf_.erase(token.dataset_id);  // the next on-demand run takes a fresh snapshot
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

  // A producer that cannot roll back is never handed a discard (the UI disables it instead).
  if (!keep_partial && !entry->discardable) {
    return false;
  }

  // First intent wins: without this an ingestStopping observer stopping again recurses through the emit below, and a
  // second click with the opposite keep_partial re-runs the handler against the choice it acted on.
  if (entry->cancel_requested) {
    qCInfo(lcCancel) << "requestCancel ignored: ingest" << token.id << "is already stopping";
    return true;
  }

  entry->cancel_requested = true;
  entry->cancel_requested_ms = QDateTime::currentMSecsSinceEpoch();
  qCInfo(lcCancel) << "requestCancel accepted: ingest" << token.id << "dataset" << token.dataset_id << "keep_partial"
                   << keep_partial;

  // Copy what the rest needs before the emit: an observer may endIngest or beginIngest this dataset inside it, erasing
  // the element `entry` points at. The copy also keeps the handler alive across its own call, which usually ends the
  // ingest and destroys the stored std::function.
  const IngestCancelFn on_cancel = entry->on_cancel;
  entry = nullptr;  // anything past the emit must go through liveEntry() again

  // Before the handler: the acknowledgement does not wait for a cooperative stop.
  emit ingestStopping(token, keep_partial);

  // A producer that never terminates would leave the row stopping with no clue why: warn once.
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
    // The handler only requests a stop; a large number means teardown ran on the GUI thread.
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

// RefillGuard

RefillGuard::RefillGuard(SessionManager& session, DatasetId dataset_id) : session_(&session), dataset_id_(dataset_id) {
  // GUI thread, no event loop, as replaceDataset.
  Q_ASSERT(session.thread() == QThread::currentThread());

  // Prior ids, taken before detaching: scalars for the empty-state notify, object ids so a rollback tells which object
  // topics a failed refill added.
  const std::vector<TopicId> scalar_topics = session.dataEngine().listTopics(dataset_id);
  processor_output_topic_ids_ = session.derivedScalarTopics(/*ephemeral_only=*/false);
  replaced_source_topic_ids_.reserve(scalar_topics.size());
  for (const TopicId topic_id : scalar_topics) {
    if (processor_output_topic_ids_.count(topic_id) == 0) {
      replaced_source_topic_ids_.push_back(topic_id);
    }
  }
  prior_object_topic_ids_ = session.objectStore().listTopics(dataset_id);
  // The refill re-binds same-named topics into these ids and lands its parsers in their slots: hold the prior
  // bindings so a rollback can reinstate the parser that wrote the bytes it restores.
  object_parser_snapshot_ = session.snapshotObjectTopicParsers(prior_object_topic_ids_);

  // (0) A marker run staged from the old data would publish into the refilling dataset.
  session.batchEngine().cancelRunsOn(dataset_id);
  //     On-demand results go before the detach, so a rollback cannot bring back one of the old meaning, and nothing is
  //     served from detached or partial data until commit or rollback.
  session.suspendOnDemand(dataset_id);
  // (1) Adapters drop cached TopicChunk* before any deque moves.
  session.notifyDatasetAboutToBeReplaced(dataset_id);
  // (2) Detach, not free, keeping the ids registered for the refill.
  scalar_snapshot_ = session.dataEngine().detachDatasetChunks(dataset_id);
  object_snapshot_ = session.objectStore().detachDataset(dataset_id);
  // (3) The UI shows the dataset empty.
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
  if (committed_) {
    return;
  }
  committed_ = true;
  scalar_snapshot_ = {};  // free the held-aside prior data; the refilled data is kept
  object_snapshot_ = {};
  prior_object_topic_ids_.clear();
  object_parser_snapshot_.clear();
  replaced_source_topic_ids_.clear();
  processor_output_topic_ids_.clear();
  // The content now comes from another source, so the old provenance is stale (a promoted cloud dataset refilled from
  // a local file must not keep the cloud descriptor). A rollback never reaches commit and keeps it.
  if (session_ != nullptr) {
    session_->detachSourceRecord(dataset_id_);
    // The new load's document, if any, is published at its own commit seam.
    session_->clearDatasetMetadata(dataset_id_);
    session_->resumeOnDemandLater(dataset_id_);
  }
}

void SessionManager::removeEphemerals(
    const std::unordered_set<std::string>& transform_keys, const std::unordered_set<std::string>& generator_ids,
    const std::unordered_set<std::string>& on_demand_keys) {
  processor_service_->removeEphemerals(transform_keys);
  if (batch_engine_->removeEphemerals(generator_ids)) {
    notifyMarkersChanged();
  }
  bool retired = false;
  for (const std::string& key : on_demand_keys) {
    const RecipeGraph::Node* node = on_demand_->graph().find(key);
    if (node != nullptr && node->recipe.ephemeral) {
      retired = !on_demand_->remove(key).empty() || retired;
    }
  }
  if (retired) {
    emit objectTopicsChanged();
  }
}

void SessionManager::publishSharedMarkersToNewDatasets() {
  if (batch_engine_ && batch_engine_->hasGenerators()) {
    batch_engine_->reachNewDatasets();
  }
}

void SessionManager::recomputeMarkersForChanged(const std::vector<TopicId>& changed, DatasetId scope) {
  if (!batch_engine_ || !batch_engine_->hasGenerators()) {
    return;
  }
  std::vector<std::string> moved;
  {
    const auto lock = data_engine_.lockEngine();
    for (const TopicId id : changed) {
      const TopicStorage* storage = data_engine_.getTopicStorage(id);
      if (storage == nullptr) {
        continue;
      }
      const TopicMetadata metadata = storage->metadata();
      // Retention can hide samples without moving the newest one or the row count; an out-of-order write or a
      // restore only bumps the generation.
      const MarkerInputStamp stamp{
          .time_range_max = metadata.time_range_max,
          .total_row_count = metadata.total_row_count,
          .retention_floor = storage->retentionFloor(),
          .series_generation = storage->seriesGeneration()};
      const auto [seen, first_time] = marker_input_stamps_.try_emplace(id, stamp);
      if (!first_time && std::exchange(seen->second, stamp) == stamp) {
        continue;  // a streaming tick lists every topic of its dataset; this one did not move
      }
      moved.push_back(metadata.name);
    }
  }
  // Never pass an empty list: recomputeForChangedInputs reads it as "every input".
  if (!moved.empty()) {
    (void)batch_engine_->recomputeForChangedInputs(moved, scope);
  }
}

Status RefillGuard::recomputeProcessors() {
  if (session_ == nullptr) {
    return PJ::okStatus();
  }
  // Processors first: a marker rule reading a transform output runs over the recomputed output.
  if (!replaced_source_topic_ids_.empty()) {
    auto outputs = session_->dataProcessorService().rebindAndRecomputeForReplacedSources(replaced_source_topic_ids_);
    if (!outputs.has_value()) {
      return PJ::unexpected(outputs.error());
    }
    if (!outputs->empty()) {
      session_->notifyIngest(QVector<TopicId>(outputs->begin(), outputs->end()), /*live=*/false);
    }
  }
  // A reload may rename or drop a rule's inputs, so every rule bound to the dataset re-runs. pruneVanishedTopics
  // exempts marker topics, so the republished set lands on a surviving topic.
  if (session_->batch_engine_ && session_->batch_engine_->hasGenerators() &&
      !session_->batch_engine_->recomputeForDataset(dataset_id_).empty()) {
    session_->notifyMarkersChanged();
  }
  return PJ::okStatus();
}

void RefillGuard::pruneVanishedTopics() {
  if (session_ == nullptr) {
    return;
  }
  // Collect under one engine lock (getTopicStorage needs it), then retire (retireTopic re-locks the recursive mutex).
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
  // Except host-published topics (`isOffClockTopic`): the marker rules republish on the post-refill recompute, and
  // evicting the topic would drop live markers and orphan the recipe.
  std::vector<ObjectTopicId> vanished_object;
  for (const ObjectTopicId object_topic_id : prior_object_topic_ids_) {
    if (isOffClockTopic(session_->objectStore().descriptor(object_topic_id))) {
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
  // The restore changes the dataset's meaning again: another epoch, held with the refill's.
  session_->suspendOnDemand(dataset_id_);
  // Adapters drop partial-refill chunk pointers cached through progress notifies.
  session_->notifyDatasetAboutToBeReplaced(dataset_id_);
  // Scalar: drop the partial chunks and the topics the refill added, then move the prior chunks back.
  session_->dataEngine().reattachDatasetChunks(dataset_id_, std::move(scalar_snapshot_));
  // Object: evict the topics the refill added before reattaching. evictObjectTopics takes the store lock itself, so
  // no ObjectStore lock may be held here.
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
  session_->objectStore().reattachDataset(dataset_id_, std::move(object_snapshot_));
  // Reinstate each stable id's parser: the refill may have put another schema's parser in the slot, and the restored
  // bytes decode only with the one that wrote them. Only ids the dataset still owns: a topic removed during the
  // refill keeps no parser, since ObjectStore::clear() resets id allocation and a future topic could reuse the id.
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
  // Markers published during the refill described the attempted data, and the shared all-datasets set lives outside
  // the snapshot: re-run over the restored data.
  if (session_->batchEngine().hasGenerators() && !session_->batchEngine().recomputeForDataset(dataset_id_).empty()) {
    session_->notifyMarkersChanged();
  }
  // A failed tentative replay may have reset operator state; replaying over the restored raw data repairs it.
  if (auto replayed = session_->dataProcessorService().rebindAndRecomputeForReplacedSources(replaced_source_topic_ids_);
      !replayed.has_value()) {
    qCWarning(lcSession).noquote() << "RefillGuard rollback processor replay failed:"
                                   << QString::fromStdString(replayed.error());
  }
  const std::vector<TopicId> current = session_->dataEngine().listTopics(dataset_id_);
  session_->notifyIngest(QVector<TopicId>(current.begin(), current.end()), /*live=*/false);
  session_->resumeOnDemandLater(dataset_id_);  // the rollback's hold
  session_->resumeOnDemandLater(dataset_id_);  // the refill's
}

std::size_t RefillGuard::snapshotBytes() const noexcept {
  std::size_t bytes = 0;
  // Approximate: rows * (timestamp + N value columns) * 8 bytes.
  for (const auto& [topic_id, topic] : scalar_snapshot_.topics) {
    (void)topic_id;
    for (const TopicChunk& chunk : topic.chunks) {
      const std::size_t cells = static_cast<std::size_t>(chunk.stats.row_count) * (1 + chunk.columns.size());
      bytes += cells * sizeof(double);
    }
  }
  for (const auto& [raw_id, series] : object_snapshot_.series) {
    (void)raw_id;
    bytes += series.memory_bytes;
  }
  return bytes;
}

void SessionManager::replaceDataset(
    DataEngine& staged_engine, ObjectStore& staged_store, DatasetId staged_id, DatasetId primary_id,
    std::vector<std::pair<ObjectTopicId, std::unique_ptr<MessageParserHandle>>> staged_object_parsers) {
  // (1) Adapters drop cached TopicChunk* before any deque is touched. Direct same-thread connections, and no event loop
  // runs here (nor in the caller until the catalog rebuild), so no slot sees the freed chunks.
  notifyDatasetAboutToBeReplaced(primary_id);
  suspendOnDemand(primary_id);

  // (2a) replaceDatasetFrom fails only on a programming error and validates before mutating, so the primary keeps its
  // data on that path.
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

  // (2b) Object swap, then the parsers re-registered under the primary ids; vanished topics lose theirs.
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

  // (3) A reload replaces filter inputs wholesale: outputs are reset and replayed, not appended, and notified.
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

  // (4) A reload may rename or drop a rule's inputs: every rule bound to the dataset re-runs.
  if (batch_engine_ && batch_engine_->hasGenerators() && !batch_engine_->recomputeForDataset(primary_id).empty()) {
    notifyMarkersChanged();
  }

  // (5) Re-index the cleared adapters.
  notifyIngest(std::move(changed), /*live=*/false);
  resumeOnDemandLater(primary_id);
}

std::optional<DatasetMergeReport> SessionManager::mergeDatasets(
    DatasetId anchor, const std::vector<DatasetMergeSource>& sources) {
  // (1) Adapters bound to the anchor or a source drop cached TopicChunk* (no event loop runs here). The helper also
  // invalidates the minimum caches, since an object-only merge may notify no scalar topic later.
  notifyDatasetAboutToBeReplaced(anchor);
  for (const auto& source : sources) {
    notifyDatasetAboutToBeReplaced(source.dataset_id);
  }
  // Before the object fold: derived entries are cleared, never merged and shifted as raw observations (a finding's
  // store timestamp is a sentinel).
  std::vector<DatasetId> contributors{anchor};
  for (const auto& source : sources) {
    contributors.push_back(source.dataset_id);
  }
  for (const DatasetId dataset : contributors) {
    suspendOnDemand(dataset);
  }

  // (2) The engine validates the inputs (unknown, duplicate, self source) before mutating, so a failure here leaves it
  // untouched.
  DatasetMergeReport report;
  if (auto result = data_engine_.mergeDatasets(anchor, sources); result.has_value()) {
    report = std::move(*result);
  } else {
    qCWarning(lcSession).noquote() << "mergeDatasets:" << QString::fromStdString(result.error());
    for (const DatasetId dataset : contributors) {
      resumeOnDemandLater(dataset);
    }
    return std::nullopt;  // engine rejected: nothing mutated — let the caller skip the catalog update
  }

  // The merged result is no contributor's original content: drop every contributor's provenance, the anchor's too.
  // dataset_source_paths_ stays: the caller owns the post-merge source bookkeeping.
  dataset_source_records_.erase(anchor);
  dataset_metadata_.erase(anchor);
  dataset_resources_.erase(anchor);
  for (const auto& source : sources) {
    dataset_source_records_.erase(source.dataset_id);
    dataset_metadata_.erase(source.dataset_id);
    dataset_resources_.erase(source.dataset_id);
  }

  // (3) Object topics fold the same way except marker sets: single-entry supersede topics at a sentinel timestamp,
  // which the generic fold would reduce to one dataset's set on the wrong clock. BatchEngine merges them below, while
  // the sources still hold them. Shares the scalar merge's validation, so it cannot fail after that succeeded.
  if (auto objects = object_store_.mergeDatasets(anchor, sources, isMarkerTopic); objects.has_value()) {
    // Shared-name source topics are empty now (folded into the anchor's, decoded by the anchor's parser): evict them
    // and their parser slots. Source-only topics were reparented and keep their ids and parsers.
    std::vector<ObjectTopicId> folded_sources;
    folded_sources.reserve(objects->remapped.size());
    for (const auto& [source_id, dest_id] : objects->remapped) {
      folded_sources.push_back(source_id);
    }
    evictObjectTopics(folded_sources);
  } else {
    qCWarning(lcSession).noquote() << "mergeDatasets(objects):" << QString::fromStdString(objects.error());
  }

  // (3b) Marker sets, concatenated and shifted onto the anchor clock, before the sources leave the catalog; also
  // without live generators, since markers outlive their recipe.
  if (batch_engine_) {
    batch_engine_->mergeMarkerTopics(anchor, sources);
    // A rule bound to a consumed source would resolve nothing once it leaves the catalog: move it to the anchor.
    // Driven by what the engine consumed, so a source it declined keeps its rule.
    batch_engine_->remapGeneratorsToAnchor(anchor, report.consumed_datasets);
  }

  // (3c) Re-install the on-demand recipes of the anchor and the consumed datasets so they bind on the anchor (a
  // finding's pinned time moves with its data). From the event loop: the caller rebuilds the catalog first.
  std::unordered_map<DatasetId, Timestamp> shifts{{anchor, 0}};
  for (const DatasetId consumed : report.consumed_datasets) {
    const auto source = std::ranges::find(sources, consumed, &DatasetMergeSource::dataset_id);
    shifts.emplace(consumed, source != sources.end() ? source->raw_shift_ns : 0);
  }
  QMetaObject::invokeMethod(
      this,
      [this, anchor, shifts = std::move(shifts), contributors = std::move(contributors)] {
        rebindOnDemandAfterMerge(anchor, shifts);
        for (const DatasetId dataset : contributors) {
          on_demand_->resume(dataset);
        }
      },
      Qt::QueuedConnection);

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
  // The old slot is destroyed after the lock is released: its teardown may dlclose.
  ObjectParserSlot replaced_slot;
  {
    std::unique_lock lock(object_parsers_mutex_);
    if (!incoming_valid) {
      // Keep the prior parser and warn: dropping a valid registration for an invalid replacement made a topic
      // undecodable with no diagnostic.
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
    // A fresh mutex per registration: consumers of the old parser still hold the old one.
    ObjectParserSlot fresh{std::shared_ptr<MessageParserHandle>(std::move(parser)), std::make_shared<std::mutex>()};
    auto& slot = object_topic_parsers_[id.id];
    replaced_slot = std::move(slot);  // keep the old handle off the lock-held dtor path
    slot = std::move(fresh);
  }
}

std::vector<SessionManager::ObjectParserSnapshotEntry> SessionManager::snapshotObjectTopicParsers(
    const std::vector<ObjectTopicId>& topic_ids) const {
  std::vector<ObjectParserSnapshotEntry> snapshot;
  snapshot.reserve(topic_ids.size());
  std::shared_lock lock(object_parsers_mutex_);
  for (const ObjectTopicId topic_id : topic_ids) {
    ObjectParserSnapshotEntry entry;
    entry.topic_id = topic_id.id;
    // Copied verbatim, an invalid handle included, so a restore reproduces the registration as it was.
    if (const auto it = object_topic_parsers_.find(topic_id.id); it != object_topic_parsers_.end()) {
      entry.slot = it->second;
    }
    snapshot.push_back(std::move(entry));
  }
  return snapshot;
}

void SessionManager::restoreObjectTopicParsers(std::vector<ObjectParserSnapshotEntry> snapshot) {
  // Displaced slots are destroyed after the lock is released (teardown may dlclose).
  std::vector<ObjectParserSlot> displaced;
  displaced.reserve(snapshot.size());
  {
    std::unique_lock lock(object_parsers_mutex_);
    for (ObjectParserSnapshotEntry& entry : snapshot) {
      if (const auto it = object_topic_parsers_.find(entry.topic_id); it != object_topic_parsers_.end()) {
        displaced.push_back(std::move(it->second));
        object_topic_parsers_.erase(it);
      }
      // The snapshot's own mutex comes back, so a consumer holding an older binding serializes against it.
      if (entry.slot.handle != nullptr) {
        object_topic_parsers_.emplace(entry.topic_id, std::move(entry.slot));
      }
    }
  }
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
  // Copied under the lock: the snapshot keeps the parser alive if a worker replaces the slot right after.
  return ParserBinding{
      static_cast<MessageParserPluginBase*>(slot->handle->context()),
      slot->mutex,
      slot->handle,
  };
}

std::shared_ptr<void> SessionManager::parserKeepaliveForObjectTopic(ObjectTopicId id) const {
  std::shared_lock lock(object_parsers_mutex_);
  const auto* slot = findValidParserSlotLocked(id);
  return slot != nullptr ? slot->handle : nullptr;
}

MessageParserPluginBase* SessionManager::parserForObjectTopic(ObjectTopicId id) const {
  // No keepalive: only for synchronous GUI-thread use. Other threads and cached uses take parserBindingForObjectTopic.
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

bool SessionManager::removeDataset(DatasetId dataset_id, bool remove_dependent_processors) {
  bool removed_processors = false;
  if (remove_dependent_processors) {
    // Before the erase: an eager node must not outlive its input storage.
    const std::vector<TopicId> topics = datasetTopicIds(dataset_id);
    removed_processors = !processor_service_->dependentProcessorOutputs(topics).empty();
    if (const Status removed = processor_service_->removeProcessorsDependingOn(topics); !removed.has_value()) {
      qCWarning(lcSession).noquote() << "removeDataset: dependent processors:"
                                     << QString::fromStdString(removed.error());
    }
  }
  data_engine_.removeDataset(dataset_id);
  dataset_source_paths_.erase(dataset_id);
  // A removed dataset's record must not resolve a future query (a reminted id could collide).
  dataset_source_records_.erase(dataset_id);
  dataset_metadata_.erase(dataset_id);
  dataset_resources_.erase(dataset_id);
  std::erase_if(merged_into_, [dataset_id](const auto& entry) {
    return entry.first == dataset_id || entry.second == dataset_id;
  });
  // Its rules go too: one naming the dead id would re-register a phantom marker topic at the next recompute.
  if (batch_engine_ && !batch_engine_->clearGeneratorsForDataset(dataset_id).empty()) {
    notifyMarkersChanged();
  }
  // Its on-demand recipes go with it, their topics retired: a run in flight is cancelled and never published.
  std::vector<ObjectTopicId> retired;
  for (const std::string& key : on_demand_->graph().keysOf(dataset_id)) {
    const std::vector<ObjectTopicId> topics = on_demand_->remove(key);
    retired.insert(retired.end(), topics.begin(), topics.end());
  }
  held_tf_.erase(dataset_id);
  live_at_.erase(dataset_id);
  if (!retired.empty()) {
    emit objectTopicsChanged();
  }
  // Re-base globalTimeReference() on the survivors (removing the earliest dataset moves the origin).
  invalidateDatasetMinTimestamp(dataset_id);
  // A pure origin shift reaches no per-topic signal: emit the global reframe.
  notifyGlobalTimeReferenceIfChanged();
  return removed_processors;
}

std::vector<TopicId> SessionManager::datasetTopicIds(DatasetId dataset_id) const {
  const auto lock = data_engine_.lockEngine();
  const DatasetInfo* info = data_engine_.getDataset(dataset_id);
  return info != nullptr ? info->topic_ids : std::vector<TopicId>{};
}

void SessionManager::refreshDatasetTimeReference(DatasetId dataset_id) {
  // The terminal flush may have committed data earlier than the running pin.
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
  // Collect the owning datasets before their descriptors go; invalidate only their pins, keeping other datasets'
  // retention pins.
  const std::unordered_set<DatasetId> affected_datasets = datasetsOwningObjectTopics(topic_ids);
  for (const DatasetId dataset_id : affected_datasets) {
    suspendOnDemand(dataset_id);  // a removed topic may be an input: its readers run again without it
  }

  // Erased slots are destroyed after the lock is released (teardown may dlclose).
  std::vector<ObjectParserSlot> erased_slots;
  erased_slots.reserve(topic_ids.size());
  for (const ObjectTopicId topic_id : topic_ids) {
    object_store_.removeTopic(topic_id);
    // A reload (fresh ObjectTopicId) then re-registers cleanly.
    std::unique_lock lock(object_parsers_mutex_);
    if (const auto it = object_topic_parsers_.find(topic_id.id); it != object_topic_parsers_.end()) {
      erased_slots.push_back(std::move(it->second));
      object_topic_parsers_.erase(it);
    }
  }
  for (const DatasetId dataset_id : affected_datasets) {
    invalidateDatasetMinTimestamp(dataset_id);
    resumeOnDemandLater(dataset_id);
  }
  notifyGlobalTimeReferenceIfChanged();
}

void SessionManager::clearAllObjects() {
  // As evictObjectTopics: collect the owning datasets before the store is cleared.
  const std::unordered_set<DatasetId> affected_datasets = datasetsOwningObjectTopics(object_store_.listTopics());
  for (const DatasetId dataset_id : affected_datasets) {
    suspendOnDemand(dataset_id);
  }
  object_store_.clear();
  // Destroyed after the lock is released (teardown may dlclose).
  std::unordered_map<uint32_t, ObjectParserSlot> drained;
  {
    std::unique_lock lock(object_parsers_mutex_);
    drained.swap(object_topic_parsers_);
  }
  for (const DatasetId dataset_id : affected_datasets) {
    invalidateDatasetMinTimestamp(dataset_id);
    resumeOnDemandLater(dataset_id);
  }
  notifyGlobalTimeReferenceIfChanged();
}

}  // namespace PJ
