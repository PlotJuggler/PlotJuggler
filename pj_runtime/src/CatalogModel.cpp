// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/CatalogModel.h"

#include <fmt/format.h>
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>
#include <QTimer>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_datastore/column_buffer.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/DatasetQualifiedName.h"  // markerSeriesField
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

Q_LOGGING_CATEGORY(lcCatalog, "pj.runtime.catalog")

struct QStringHash {
  [[nodiscard]] std::size_t operator()(const QString& value) const noexcept {
    return qHash(value);
  }
};

[[nodiscard]] bool matchesCapability(const ScalarFieldPayload& field, SeriesCapability capability) noexcept {
  switch (capability) {
    case SeriesCapability::kPlottable:
      return isPlottablePrimitive(field.logical_type);
    case SeriesCapability::kDiscrete:
      return isDiscreteField(field);
    case SeriesCapability::kAny:
      return true;
  }
  return false;
}

[[nodiscard]] QString makeCurveKey(DatasetId dataset_id, TopicId topic_id, std::size_t column_index) {
  return u"dataset:%1/topic:%2/column:%3"_s.arg(dataset_id).arg(topic_id).arg(column_index);
}

[[nodiscard]] QString makeObjectTopicKey(DatasetId dataset_id, ObjectTopicId object_topic_id) {
  return u"dataset:%1/object_topic:%2"_s.arg(dataset_id).arg(object_topic_id.id);
}

// Key for a data-less advertised placeholder; its own namespace so it never collides with a storage-backed key.
[[nodiscard]] QString makeAdvertisedKey(DatasetId dataset_id, const QString& topic_name) {
  return u"dataset:%1/advertised:%2"_s.arg(dataset_id).arg(topic_name);
}

// True when `live_path` and `saved_path` name the same on-disk source. Replicates pj_app's layout_xml::isSamePath
// semantics without depending on it (pj_runtime is below pj_app): literal equality first, then a normalisation through
// SessionManager::normalizedSourcePath. Empty inputs never match.
[[nodiscard]] bool isSamePathLike(const QString& live_path, const QString& saved_path) {
  if (live_path.isEmpty() || saved_path.isEmpty()) {
    return false;
  }
  if (live_path == saved_path) {
    return true;
  }
  // The stored form (normalizedSourcePath) on both sides, so a deleted recording
  // and a Windows drive-letter path compare equal to their saved-layout forms.
  const QString norm_live = SessionManager::normalizedSourcePath(live_path);
  const QString norm_saved = SessionManager::normalizedSourcePath(saved_path);
  return !norm_live.isEmpty() && !norm_saved.isEmpty() && norm_live == norm_saved;
}

[[nodiscard]] CurveDescriptor curveFromItem(const CatalogItem& item) {
  const auto* scalar = asScalarField(item);
  Q_ASSERT(scalar != nullptr);  // Precondition: caller verified isScalarField(item).
  return CurveDescriptor{
      .name = item.key,
      .dataset_name = item.dataset_name,
      .topic_name = item.topic_name,
      .field_name = scalar->field_name,
      .topic_id = scalar->topic_id,
      .dataset_id = item.dataset_id,
      .column_index = scalar->column_index,
      .field_path = scalar->field_path,
  };
}

[[nodiscard]] bool catalogItemLess(const CatalogItem& lhs, const CatalogItem& rhs) {
  if (lhs.dataset_id != rhs.dataset_id) {
    return lhs.dataset_id < rhs.dataset_id;
  }
  const int topic_compare = QString::localeAwareCompare(lhs.topic_name, rhs.topic_name);
  if (topic_compare != 0) {
    return topic_compare < 0;
  }
  // ObjectTopic sorts after ScalarField within the same topic (the variant
  // index gives a deterministic order: ScalarFieldPayload=0, ObjectTopic=1).
  if (lhs.payload.index() != rhs.payload.index()) {
    return lhs.payload.index() < rhs.payload.index();
  }
  if (const auto* l = asScalarField(lhs)) {
    const auto* r = asScalarField(rhs);  // index match implies r is non-null
    if (l->column_index != r->column_index) {
      return l->column_index < r->column_index;
    }
  }
  return lhs.key < rhs.key;
}

void collectTypeTreeLeaves(
    const TypeTreeNode& node, const std::string& prefix, std::size_t& next_column,
    std::vector<ColumnDescriptor>& columns) {
  const std::string current_path = prefix.empty() ? node.name : prefix + "." + node.name;

  if (node.kind == TypeKind::kStruct) {
    for (const auto& child : node.children) {
      collectTypeTreeLeaves(*child, current_path, next_column, columns);
    }
    return;
  }

  if (node.kind == TypeKind::kArray) {
    if (node.element_type && node.fixed_array_size.has_value()) {
      for (uint32_t i = 0; i < *node.fixed_array_size; ++i) {
        const std::string element_path = fmt::format("{}[{}]", current_path, i);
        if (node.element_type->kind == TypeKind::kStruct) {
          for (const auto& child : node.element_type->children) {
            collectTypeTreeLeaves(*child, element_path, next_column, columns);
          }
        } else {
          columns.push_back(
              ColumnDescriptor{
                  .field_id = static_cast<FieldId>(next_column),
                  .logical_type = node.element_type->primitive_type.value_or(PrimitiveType::kFloat64),
                  .field_path = element_path,
              });
          ++next_column;
        }
      }
    }
    return;
  }

  columns.push_back(
      ColumnDescriptor{
          .field_id = static_cast<FieldId>(next_column),
          .logical_type = node.primitive_type.value_or(PrimitiveType::kFloat64),
          .field_path = current_path,
      });
  ++next_column;
}

[[nodiscard]] std::vector<ColumnDescriptor> columnsFromTypeTree(const TypeTreeNode& root) {
  std::vector<ColumnDescriptor> columns;
  std::size_t next_column = 0;
  if (root.kind == TypeKind::kStruct) {
    for (const auto& child : root.children) {
      collectTypeTreeLeaves(*child, "", next_column, columns);
    }
  } else {
    collectTypeTreeLeaves(root, "", next_column, columns);
  }
  return columns;
}

// A well-mixed hash of `value` under `tag`: summed where the order does not matter, chained (the result as the next
// tag) where it does.
[[nodiscard]] std::uint64_t mixHash(std::uint64_t tag, std::uint64_t value) {
  std::uint64_t x = (tag * 1099511628211ULL) ^ (value + 0x9E3779B97F4A7C15ULL);
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27;
  return x;
}

// The columns a topic's items come from: the storage's own, else its first chunk's, else its type tree's. The last two
// are built into `scratch`; the first is returned as it is, valid while the caller holds the engine lock.
[[nodiscard]] const std::vector<ColumnDescriptor>& topicColumns(
    const TopicStorage& storage, const TypeTreeNode* type_tree, std::vector<ColumnDescriptor>& scratch) {
  if (!storage.columnDescriptors().empty()) {
    return storage.columnDescriptors();
  }

  const auto& chunks = storage.sealedChunks();
  if (!chunks.empty()) {
    scratch.reserve(chunks.front().columns.size());
    for (const auto& column : chunks.front().columns) {
      if (column.descriptor) {
        scratch.push_back(*column.descriptor);
      }
    }
  } else if (type_tree != nullptr) {
    scratch = columnsFromTypeTree(*type_tree);
  }
  return scratch;
}

// Everything a scalar topic's items are derived from, in one number: a topic whose hash has not moved since its items
// were derived keeps them (CatalogModel::rebuildNow). `dataset_hash` covers what they take from their dataset;
// `columns` are topicColumns(), read under the engine lock like `storage`.
[[nodiscard]] std::uint64_t scalarTopicSourceHash(
    std::uint64_t dataset_hash, const std::string& topic_name, const std::vector<ColumnDescriptor>& columns,
    const TopicStorage& storage) {
  std::uint64_t hash = mixHash(dataset_hash, std::hash<std::string>{}(topic_name));
  hash = mixHash(hash, columns.size());
  for (const ColumnDescriptor& column : columns) {
    hash = mixHash(
        hash, (static_cast<std::uint64_t>(column.field_id) << 8) ^ static_cast<std::uint64_t>(column.logical_type));
    hash = mixHash(hash, std::hash<std::string>{}(column.field_path));
  }
  // Which float columns hold whole numbers only (ScalarFieldPayload::whole_numbers_only), and which are retired.
  hash = mixHash(hash, storage.wholeNumbersSignature());
  std::uint64_t retired = 0;
  for (const FieldId field_id : storage.retiredColumns()) {
    retired += mixHash(3, field_id);
  }
  return mixHash(hash, retired);
}

}  // namespace

std::string objectPayloadFormatFromMetadata(std::string_view metadata_json) {
  const auto metadata = nlohmann::json::parse(metadata_json, nullptr, /*allow_exceptions=*/false);
  if (!metadata.is_object()) {
    return {};
  }
  const std::string encoding = metadata.value("payload_encoding", std::string{});
  const std::string schema = metadata.value("payload_schema", std::string{});
  if (encoding.empty() && schema.empty()) {
    return {};
  }
  return encoding + ":" + schema;
}

sdk::BuiltinObjectType objectTypeFromMetadata(std::string_view metadata_json) {
  if (metadata_json.empty()) {
    return sdk::BuiltinObjectType::kNone;
  }
  try {
    const auto metadata = nlohmann::json::parse(metadata_json);
    const auto it = metadata.find("builtin_object_type");
    if (it == metadata.end()) {
      qCWarning(lcCatalog) << "objectTypeFromMetadata: missing 'builtin_object_type' key — metadata="
                           << QString::fromUtf8(metadata_json.data(), static_cast<int>(metadata_json.size()));
      return sdk::BuiltinObjectType::kNone;
    }
    if (!it->is_string()) {
      qCWarning(lcCatalog) << "objectTypeFromMetadata: 'builtin_object_type' is not a string — metadata="
                           << QString::fromUtf8(metadata_json.data(), static_cast<int>(metadata_json.size()));
      return sdk::BuiltinObjectType::kNone;
    }
    const auto raw = it->get<std::string>();
    const auto parsed = sdk::parseBuiltinObjectType(raw);
    if (!parsed.has_value()) {
      qCWarning(lcCatalog) << "objectTypeFromMetadata: unknown builtin_object_type='" << QString::fromStdString(raw)
                           << "' — topic will be hidden from object-aware views";
      return sdk::BuiltinObjectType::kNone;
    }
    return *parsed;
  } catch (const nlohmann::json::exception& e) {
    qCWarning(lcCatalog) << "objectTypeFromMetadata: JSON parse failed:" << e.what() << "metadata="
                         << QString::fromUtf8(metadata_json.data(), static_cast<int>(metadata_json.size()));
    return sdk::BuiltinObjectType::kNone;
  }
}

struct CatalogModel::Impl {
  explicit Impl(SessionManager* session_in) : session(session_in) {}

  using ItemMap = tsl::robin_map<QString, CatalogItem, QStringHash>;
  using NameSet = tsl::robin_set<QString, QStringHash>;

  SessionManager* session = nullptr;
  ItemMap items;
  // Caches derived from `items`: dropCaches() runs on every change of `items`, before any signal, or a slot reads a
  // stale one. findScalarForMarkerKey's answers by dataset, then `strings_too`, then key:
  tsl::robin_map<DatasetId, std::array<tsl::robin_map<std::string, ScalarFieldPayload>, 2>> scalar_lookups;
  // datasets(), filled on first read.
  std::optional<std::vector<std::pair<DatasetId, QString>>> datasets;
  // shownTopics(), computed on first read, for the same reason.
  std::optional<ShownTopics> shown_topics;

  // A scalar topic's items as of the last rebuild: the hash of everything they were derived from
  // (scalarTopicSourceHash) and their keys. A rebuild that may reuse them (rebuildIfChanged) skips a topic whose hash
  // has not moved, so it costs the topics that changed and not the catalog: a load that keeps adding columns to a few
  // topics re-derived ~100k items a second.
  struct ScalarTopicItems {
    std::uint64_t source_hash = 0;
    DatasetId dataset_id = 0;
    QString topic_name;
    std::vector<QString> keys;
  };
  using ScalarTopicMap = tsl::robin_map<TopicId, ScalarTopicItems>;
  ScalarTopicMap scalar_topics;
  // Keys of the items that are not scalar fields: object topics and advertised placeholders.
  std::vector<QString> other_keys;
  // False once anything but rebuildNow() edited `items`: scalar_topics and other_keys then no longer describe them,
  // and the next rebuild derives everything.
  bool topic_items_valid = false;
  std::size_t scalar_topics_derived = 0;  ///< for scalarTopicsDerived()

  void dropCaches() {
    scalar_lookups.clear();
    datasets.reset();
    shown_topics.reset();
    topic_items_valid = false;
  }

  // Puts `fresh` in place of the items `topic` had, and adds what that changed to the three lists.
  void replaceScalarTopicItems(
      ScalarTopicItems& topic, std::vector<CatalogItem>&& fresh, std::vector<CatalogItem>& added, QStringList& removed,
      QStringList& changed) {
    std::vector<QString> keys;
    keys.reserve(fresh.size());
    NameSet fresh_keys;
    fresh_keys.reserve(fresh.size());
    for (CatalogItem& item : fresh) {
      keys.push_back(item.key);
      fresh_keys.insert(item.key);
      const auto existing = items.find(keys.back());
      if (existing == items.end()) {
        added.push_back(item);
        items.insert({keys.back(), std::move(item)});
      } else if (!(existing->second == item)) {
        changed.push_back(keys.back());
        existing.value() = std::move(item);
      }
    }
    for (const QString& key : topic.keys) {
      if (fresh_keys.count(key) == 0 && items.erase(key) > 0) {
        removed.push_back(key);
      }
    }
    topic.keys = std::move(keys);
  }
  // Datasets hidden by clearAll: rebuildFromDatastore skips them entirely.
  // A reload of the same file creates a fresh DatasetId not in this set, so
  // the curves come back.
  tsl::robin_set<DatasetId> removed_datasets;
  // Per-dataset name blacklist for selective removal via removeCurves. Keyed
  // by the DatasetId the curve belonged to, so a reload (new DatasetId)
  // re-introduces the name.
  tsl::robin_map<DatasetId, NameSet> removed_names_per_dataset;

  // Advertised (available-but-unsubscribed) topics per dataset: topic name →
  // a-priori classification. Merged into the catalog by rebuildNow as data-less
  // placeholders only where storage has NOT produced an entry for the same topic
  // (real data wins). Declarative full-set per dataset (see setAdvertisedTopics).
  tsl::robin_map<DatasetId, tsl::robin_map<QString, sdk::BuiltinObjectType, QStringHash>> advertised;

  // Datasets whose streaming source supports per-topic pause (see
  // setPerTopicPauseCapable). Membership only, no payload.
  tsl::robin_set<DatasetId> per_topic_pause_capable_datasets;

  // Taken before the last rebuild of either kind, for rebuildIfChanged; unset until the first, so the first ingest
  // always rebuilds.
  std::optional<std::uint64_t> last_fingerprint;

  // During an open ingest, a commit's rebuild waits until `next_ingest_rebuild`; the timer runs the one left waiting.
  std::chrono::steady_clock::time_point next_ingest_rebuild{};
  std::chrono::nanoseconds min_ingest_rebuild_gap{0};
  QTimer ingest_rebuild_timer;

  // The gap is four rebuild costs (rebuilds take at most a fifth of the GUI thread while one costs under 250 ms),
  // capped, the floor included, so a new topic shows within a second.
  void recordRebuildCost(std::chrono::steady_clock::time_point started) {
    using namespace std::chrono;
    const auto ended = steady_clock::now();
    next_ingest_rebuild =
        ended + std::min<nanoseconds>(std::max<nanoseconds>(4 * (ended - started), min_ingest_rebuild_gap), 1s);
  }
  /// `items()` and `forEachItem()` calls and `shownTopics()` recomputations, for `wholeCatalogWalks()`.
  std::atomic<std::size_t> whole_catalog_walks{0};
};

CatalogModel::CatalogModel(SessionManager* session, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(session)) {
  impl_->ingest_rebuild_timer.setSingleShot(true);
  connect(&impl_->ingest_rebuild_timer, &QTimer::timeout, this, &CatalogModel::rebuildIfChanged);
  if (impl_->session != nullptr) {
    // While an ingest is open, a non-live commit inside the gap after the last rebuild waits for the timer (see
    // ARCHITECTURE "CatalogModel rebuild timing"); streams and other commits rebuild at once.
    connect(impl_->session, &SessionManager::samplesIngested, this, [this](const QVector<TopicId>& /*ids*/, bool live) {
      const auto wait = impl_->next_ingest_rebuild - std::chrono::steady_clock::now();
      if (!live && wait.count() > 0 && impl_->session->hasActiveIngests()) {
        if (!impl_->ingest_rebuild_timer.isActive()) {
          impl_->ingest_rebuild_timer.start(std::chrono::ceil<std::chrono::milliseconds>(wait));
        }
        return;
      }
      rebuildIfChanged();
    });
    // An on-demand install registers its output topics, a removal retires them; neither is an ingest.
    connect(impl_->session, &SessionManager::objectTopicsChanged, this, &CatalogModel::rebuildIfChanged);
    connect(impl_->session, &SessionManager::onDemandRecipeInstalled, this, &CatalogModel::rebuildIfChanged);
    connect(this, &CatalogModel::itemsAdded, impl_->session, &SessionManager::publishSharedMarkersToNewDatasets);
  }
}

CatalogModel::~CatalogModel() = default;

QSet<QString> CatalogModel::ephemeralPreviewKeys() const {
  QSet<QString> keys;
  if (impl_->session == nullptr) {
    return keys;
  }
  // From the previews to their topics, then one hash lookup each: no walk of the catalog's items.
  const OnDemandService& on_demand = impl_->session->onDemandService();
  for (const DatasetId dataset : impl_->session->dataEngine().listDatasets()) {
    for (const std::string& node_key : on_demand.graph().keysOf(dataset)) {
      const RecipeGraph::Node* node = on_demand.graph().find(node_key);
      if (node == nullptr || !node->recipe.ephemeral) {
        continue;
      }
      for (const ObjectTopicId topic : on_demand.history().topicsOf(node_key)) {
        if (std::optional<QString> key = objectTopicKey(dataset, topic)) {
          keys.insert(*std::move(key));
        }
      }
    }
  }
  return keys;
}

std::vector<CatalogItem> CatalogModel::items() const {
  impl_->whole_catalog_walks.fetch_add(1, std::memory_order_relaxed);
  std::vector<CatalogItem> items;
  items.reserve(impl_->items.size());
  for (const auto& [key, item] : impl_->items) {
    (void)key;
    items.push_back(item);
  }
  std::sort(items.begin(), items.end(), catalogItemLess);
  return items;
}

std::size_t CatalogModel::wholeCatalogWalks() const noexcept {
  return impl_->whole_catalog_walks.load(std::memory_order_relaxed);
}

std::size_t CatalogModel::scalarTopicsDerived() const noexcept {
  return impl_->scalar_topics_derived;
}

void CatalogModel::forEachItem(const std::function<void(const CatalogItem&)>& visit) const {
  impl_->whole_catalog_walks.fetch_add(1, std::memory_order_relaxed);
  for (const auto& entry : impl_->items) {
    visit(entry.second);
  }
}

std::vector<CatalogItem> CatalogModel::objectTopicsNamed(
    const QString& topic_name, const QString& dataset_label) const {
  std::vector<CatalogItem> matches;
  const std::optional<DatasetId> named = dataset_label.isEmpty() ? std::nullopt : datasetByLabel(dataset_label);
  if (!dataset_label.isEmpty() && !named.has_value()) {
    return matches;
  }
  for (const auto& [key, item] : impl_->items) {
    (void)key;
    if (isObjectTopic(item) && item.topic_name == topic_name && (!named.has_value() || item.dataset_id == *named)) {
      matches.push_back(item);
    }
  }
  std::sort(matches.begin(), matches.end(), catalogItemLess);
  return matches;
}

std::optional<CatalogItem> CatalogModel::objectTopicItem(ObjectTopicId topic_id) const {
  for (const auto& [key, item] : impl_->items) {
    (void)key;
    if (const auto* object_topic = asObjectTopic(item);
        object_topic != nullptr && object_topic->object_topic_id == topic_id) {
      return item;
    }
  }
  return std::nullopt;
}

std::optional<ScalarFieldPayload> CatalogModel::findScalarForMarkerKey(
    DatasetId dataset, const std::string& key, bool strings_too) const {
  auto& found_in_dataset = impl_->scalar_lookups[dataset][strings_too ? 1 : 0];
  if (const auto hit = found_in_dataset.find(key); hit != found_in_dataset.end()) {
    return hit->second;
  }
  const QString key_text = QString::fromStdString(key);
  const CatalogItem* first = nullptr;
  const ScalarFieldPayload* found = nullptr;
  for (const auto& [item_key, item] : impl_->items) {
    (void)item_key;
    // A series key starts with its topic name: reject cheaply before building strings.
    if (item.dataset_id != dataset || !key_text.startsWith(item.topic_name)) {
      continue;
    }
    if (const ScalarFieldPayload* scalar = markerSeriesField(item, key, strings_too);
        scalar != nullptr && (first == nullptr || catalogItemLess(item, *first))) {
      first = &item;
      found = scalar;
    }
  }
  if (found == nullptr) {
    return std::nullopt;
  }
  found_in_dataset.emplace(key, *found);
  return *found;
}

DatasetId CatalogModel::datasetForTopic(const QString& topic_name) const {
  DatasetId found = 0;
  for (const auto& [key, item] : impl_->items) {
    (void)key;
    if (item.topic_name != topic_name) {
      continue;
    }
    if (found != 0 && item.dataset_id != found) {
      return 0;
    }
    found = item.dataset_id;
  }
  return found;
}

bool CatalogModel::isEmpty() const noexcept {
  return impl_->items.empty();
}

std::optional<CatalogItem> CatalogModel::itemDescriptor(const QString& key) const {
  const auto it = impl_->items.find(key);
  if (it == impl_->items.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::string> CatalogModel::pluginDropName(const QString& key) const {
  const auto it = impl_->items.find(key);
  if (it == impl_->items.end()) {
    return std::nullopt;
  }
  const CatalogItem& item = it->second;
  const std::string topic_name = item.topic_name.toStdString();
  if (const auto* scalar = asScalarField(item); scalar != nullptr) {
    // The shared key builder: the plugin names the series as the producer and the plot overlay do.
    return sdk::markerSeriesKey(topic_name, scalar->field_name.toStdString());
  }
  if (isAdvertisedTopic(item)) {
    return topic_name;
  }
  if (!isObjectTopic(item)) {
    return std::nullopt;
  }
  // Only a dataset label addresses one of several same-named topics.
  const QString source = datasetLabel(item.dataset_id).value_or(QString{});
  if (!source.isEmpty() && objectTopicsNamed(item.topic_name, {}).size() > 1 &&
      objectTopicsNamed(item.topic_name, source).size() == 1) {
    return sdk::qualifiedSeriesName(source.toStdString(), topic_name);
  }
  return topic_name;
}

std::optional<QString> CatalogModel::objectTopicKey(DatasetId dataset, ObjectTopicId topic) const {
  QString key = makeObjectTopicKey(dataset, topic);
  if (impl_->items.find(key) == impl_->items.end()) {
    return std::nullopt;
  }
  return key;
}

std::optional<QString> CatalogModel::scalarColumnKey(DatasetId dataset, TopicId topic, std::size_t column) const {
  QString key = makeCurveKey(dataset, topic, column);
  if (impl_->items.find(key) == impl_->items.end()) {
    return std::nullopt;
  }
  return key;
}

std::optional<double> CatalogModel::scalarValueAt(const QString& key, double display_seconds) const {
  if (impl_->session == nullptr) {
    return std::nullopt;
  }
  const auto it = impl_->items.find(key);
  if (it == impl_->items.end()) {
    return std::nullopt;
  }
  const ScalarFieldPayload* scalar = asScalarField(it->second);
  if (scalar == nullptr || !isPlottablePrimitive(scalar->logical_type)) {
    return std::nullopt;  // object topics carry no scalar value; strings → stringValueAt
  }
  // The plot tracker's lookup, under the engine lock. The type was checked above,
  // so read the storage directly rather than through DataReader::series, which
  // copies the topic's column descriptors on every call (per visible row, 10 Hz).
  const DisplayOffset offset = impl_->session->displayOffset(it->second.dataset_id);
  const Timestamp raw_ns = displaySecondsToRaw(DisplaySeconds{display_seconds}, offset);
  const DataEngine& engine = impl_->session->dataEngine();
  const auto lock = engine.lockEngine();
  const TopicStorage* storage = engine.getTopicStorage(scalar->topic_id);
  if (storage == nullptr) {
    return std::nullopt;
  }
  const SeriesReader series(storage->sealedChunks(), scalar->column_index, storage->retentionFloor());
  const auto sample = series.sampleAtOrBeforeTime(raw_ns);
  return sample.has_value() ? std::optional<double>{sample->value} : std::nullopt;
}

std::optional<TopicStats> CatalogModel::topicStats(const QString& key) const {
  if (impl_->session == nullptr) {
    return std::nullopt;
  }
  const auto it = impl_->items.find(key);
  if (it == impl_->items.end()) {
    return std::nullopt;
  }
  const CatalogItem& item = it->second;
  TopicStats stats{.topic_name = item.topic_name, .live = impl_->session->ingestActive(item.dataset_id)};
  if (const ScalarFieldPayload* scalar = asScalarField(item)) {
    const DataReader reader(impl_->session->dataEngine());
    const auto metadata = reader.getMetadata(scalar->topic_id);
    if (!metadata.has_value()) {
      return std::nullopt;
    }
    stats.count = metadata->total_row_count;
    stats.schema_name = QString::fromStdString(reader.getSchemaName(scalar->topic_id).value_or(""));
    return stats;
  }
  if (const ObjectTopicPayload* object = asObjectTopic(item)) {
    stats.count = impl_->session->objectStore().entryCount(object->object_topic_id);
    // sdk::name() spells the enum ("kImage"); drop the k prefix for display.
    stats.schema_name = QString::fromUtf8(sdk::name(object->object_type)).mid(1);
    return stats;
  }
  return std::nullopt;  // advertised placeholder: no storage behind it
}

bool CatalogModel::isScalarKey(const QString& key) const {
  const auto it = impl_->items.find(key);
  if (it == impl_->items.end()) {
    return false;
  }
  if (isScalarField(it->second)) {
    return true;
  }
  // A scalar-shaped (kNone) advertised placeholder is scalar too — no sample has
  // arrived yet, but the Value column should render "-" for it exactly like a
  // real scalar field with no data, not blank like a non-scalar row.
  const auto* advertised = asAdvertisedTopic(it->second);
  return advertised != nullptr && advertised->classification == sdk::BuiltinObjectType::kNone;
}

bool CatalogModel::isStringKey(const QString& key) const {
  const auto it = impl_->items.find(key);
  if (it == impl_->items.end()) {
    return false;
  }
  const ScalarFieldPayload* scalar = asScalarField(it->second);
  return scalar != nullptr && scalar->logical_type == PrimitiveType::kString;
}

bool CatalogModel::isDiscreteKey(const QString& key) const {
  const auto it = impl_->items.find(key);
  if (it == impl_->items.end()) {
    return false;
  }
  const ScalarFieldPayload* scalar = asScalarField(it->second);
  return scalar != nullptr && isDiscreteField(*scalar);
}

std::optional<QString> CatalogModel::stringValueAt(const QString& key, double display_seconds) const {
  if (impl_->session == nullptr) {
    return std::nullopt;
  }
  const auto it = impl_->items.find(key);
  if (it == impl_->items.end()) {
    return std::nullopt;
  }
  const ScalarFieldPayload* scalar = asScalarField(it->second);
  if (scalar == nullptr || scalar->logical_type != PrimitiveType::kString) {
    return std::nullopt;  // only string fields carry a string value
  }
  const DisplayOffset offset = impl_->session->displayOffset(it->second.dataset_id);
  const Timestamp raw_ns = displaySecondsToRaw(DisplaySeconds{display_seconds}, offset);
  const DataReader reader(impl_->session->dataEngine());
  const auto value_or =
      reader.latestStringAt(QueryPoint{.topic_id = scalar->topic_id, .t = raw_ns}, scalar->column_index);
  if (!value_or.has_value() || !value_or->has_value() || !(*value_or)->value) {
    return std::nullopt;  // read error, no row at/before the query time, or a null cell
  }
  return QString::fromStdString(*(*value_or)->value);
}

std::vector<CurveDescriptor> CatalogModel::curves() const {
  std::vector<CurveDescriptor> curves;
  curves.reserve(impl_->items.size());
  for (const auto& [key, item] : impl_->items) {
    (void)key;
    // String fields are catalog items but not plottable curves.
    if (const ScalarFieldPayload* scalar = asScalarField(item);
        scalar != nullptr && isPlottablePrimitive(scalar->logical_type)) {
      curves.push_back(curveFromItem(item));
    }
  }
  std::sort(curves.begin(), curves.end(), [](const CurveDescriptor& lhs, const CurveDescriptor& rhs) {
    if (lhs.dataset_id != rhs.dataset_id) {
      return lhs.dataset_id < rhs.dataset_id;
    }
    if (lhs.topic_id != rhs.topic_id) {
      return lhs.topic_id < rhs.topic_id;
    }
    return lhs.column_index < rhs.column_index;
  });
  return curves;
}

std::optional<CurveDescriptor> CatalogModel::curveDescriptor(const QString& key, SeriesCapability capability) const {
  const auto it = impl_->items.find(key);
  if (it == impl_->items.end()) {
    return std::nullopt;
  }
  const ScalarFieldPayload* scalar = asScalarField(it->second);
  if (scalar == nullptr || !matchesCapability(*scalar, capability)) {
    return std::nullopt;
  }
  return curveFromItem(it->second);
}

std::vector<std::pair<DatasetId, QString>> CatalogModel::datasets() const {
  if (!impl_->datasets) {
    std::vector<std::pair<DatasetId, QString>> result;
    tsl::robin_set<DatasetId> seen;
    for (const auto& [key, item] : impl_->items) {
      (void)key;
      if (seen.insert(item.dataset_id).second) {
        result.emplace_back(item.dataset_id, item.dataset_name);
      }
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    impl_->datasets = std::move(result);
  }
  return *impl_->datasets;
}

CatalogModel::ShownTopics CatalogModel::shownTopics() const {
  if (!impl_->shown_topics) {
    ShownTopics shown;
    tsl::robin_set<TopicId> seen_scalar;
    tsl::robin_set<uint32_t> seen_object;
    for (const auto& [key, item] : impl_->items) {
      (void)key;
      if (const ScalarFieldPayload* scalar = asScalarField(item)) {
        if (seen_scalar.insert(scalar->topic_id).second) {
          shown.scalar.emplace_back(item.dataset_id, scalar->topic_id);
        }
      } else if (const ObjectTopicPayload* object = asObjectTopic(item)) {
        if (seen_object.insert(object->object_topic_id.id).second) {
          shown.object.emplace_back(item.dataset_id, object->object_topic_id);
        }
      }
    }
    std::sort(shown.scalar.begin(), shown.scalar.end());
    std::sort(shown.object.begin(), shown.object.end(), [](const auto& lhs, const auto& rhs) {
      return std::pair(lhs.first, lhs.second.id) < std::pair(rhs.first, rhs.second.id);
    });
    impl_->shown_topics = std::move(shown);
    impl_->whole_catalog_walks.fetch_add(1, std::memory_order_relaxed);
  }
  return *impl_->shown_topics;
}

std::vector<DatasetId> CatalogModel::datasetIds() const {
  const auto listed = datasets();
  std::vector<DatasetId> ids;
  ids.reserve(listed.size());
  for (const auto& [dataset_id, label] : listed) {
    (void)label;
    ids.push_back(dataset_id);
  }
  return ids;
}

std::optional<QString> CatalogModel::datasetLabel(DatasetId dataset_id) const {
  if (impl_->session == nullptr) {
    return std::nullopt;
  }
  const std::string label = impl_->session->dataEngine().datasetLabel(dataset_id);
  return label.empty() ? std::nullopt : std::optional{QString::fromStdString(label)};
}

std::optional<DatasetId> CatalogModel::datasetByLabel(const QString& label) const {
  if (impl_->session == nullptr) {
    return std::nullopt;
  }
  return impl_->session->dataEngine().datasetByLabel(label.toStdString());
}

std::optional<QString> CatalogModel::datasetSourceName(DatasetId dataset_id) const {
  if (impl_->session == nullptr) {
    return std::nullopt;
  }
  const DatasetInfo* dataset = impl_->session->dataEngine().getDataset(dataset_id);
  if (dataset == nullptr) {
    return std::nullopt;
  }
  return QString::fromStdString(dataset->source_name);
}

std::optional<QString> CatalogModel::resolveCurveKey(const SavedSeriesPath& path, SeriesCapability capability) const {
  return resolveCurveKey(path.dataset_id, path.dataset_source, path.dataset_path, path.topic, path.field, capability);
}

std::optional<QString> CatalogModel::resolveCurveKey(
    DatasetId saved_id, const QString& saved_source, const QString& saved_path, const QString& topic,
    const QString& field, SeriesCapability capability) const {
  if (impl_->session == nullptr) {
    return std::nullopt;
  }

  // Qualified identities share SessionManager's one resolver: in-session duplicate
  // basenames keep their exact id, while a reminted/swapped id must also agree with
  // the FileLoader-known full path (see resolveDatasetIdentity).
  if (saved_id != 0 || !saved_source.isEmpty() || !saved_path.isEmpty()) {
    const DatasetIdentityResolution identity = resolveDatasetIdentity(saved_id, saved_source, saved_path);
    if (identity.id.has_value()) {
      if (const auto descriptor = descriptorForPath(*identity.id, topic, field, capability)) {
        return descriptor->name;
      }
      // The intended dataset exists but this series has not materialized (or
      // disappeared). Never steal a same-named field from a sibling source.
      return std::nullopt;
    }

    // Exact id failed: the physical file path now OUTRANKS the raw source label.
    // A same-file single<->fan-out reload relabels the dataset (the former raw
    // source label no longer exists) while the file path stays stable, so path is
    // the more trustworthy identity. Bind only when exactly one physical-source
    // sibling provides this series; ambiguity stays unresolved, never fan-out order.
    if (!saved_path.isEmpty()) {
      std::optional<QString> unique_path_match;
      for (const auto& [dataset_id, name] : datasets()) {
        (void)name;
        if (!isSamePathLike(datasetSourcePath(dataset_id), saved_path)) {
          continue;
        }
        if (const auto descriptor = descriptorForPath(dataset_id, topic, field, capability)) {
          if (unique_path_match.has_value()) {
            return std::nullopt;
          }
          unique_path_match = descriptor->name;
        }
      }
      return unique_path_match;
    }
    return std::nullopt;
  }

  // Legacy/generic unqualified layouts may target a structurally similar
  // recording. Accept exactly one candidate; duplicates stay unresolved (taking the
  // first match binds the layout to an arbitrary dataset).
  std::optional<QString> unique_match;
  for (const auto& [id, name] : datasets()) {
    (void)name;
    if (const auto descriptor = descriptorForPath(id, topic, field, capability)) {
      if (unique_match.has_value()) {
        return std::nullopt;
      }
      unique_match = descriptor->name;
    }
  }
  return unique_match;
}

DatasetIdentityResolution CatalogModel::resolveDatasetIdentity(
    DatasetId saved_id, const QString& saved_source, const QString& saved_path) const {
  return impl_->session != nullptr ? impl_->session->resolveDatasetIdentity(saved_id, saved_source, saved_path)
                                   : DatasetIdentityResolution{};
}

QString CatalogModel::datasetSourcePath(DatasetId dataset_id) const {
  return impl_->session != nullptr ? impl_->session->datasetSourcePath(dataset_id) : QString{};
}

std::optional<DatasetMetadata> CatalogModel::datasetMetadata(DatasetId dataset_id) const {
  if (impl_->session == nullptr) {
    return std::nullopt;
  }
  const DatasetMetadata* metadata = impl_->session->datasetMetadata(dataset_id);
  return metadata != nullptr ? std::optional<DatasetMetadata>(*metadata) : std::nullopt;
}

std::optional<CurveDescriptor> CatalogModel::descriptorForPath(
    DatasetId dataset_id, const QString& topic, const QString& field, SeriesCapability capability) const {
  for (const auto& [key, item] : impl_->items) {
    (void)key;
    if (item.dataset_id != dataset_id || item.topic_name != topic || !isScalarField(item)) {
      continue;
    }
    // The consumer's capability gates the match: a plot curve never binds a
    // string field, a strip series never binds a float — but the overlap is
    // deliberate (a strip series saved against a string field may rebind onto
    // an integer field at the same path after a reload).
    const ScalarFieldPayload* scalar = asScalarField(item);
    if (scalar->field_path == field && matchesCapability(*scalar, capability)) {
      return curveFromItem(item);
    }
  }
  return std::nullopt;
}

void CatalogModel::rebuildFromDatastore() {
  // The fingerprint is taken before the rebuild: a worker commit landing during it then moves the fingerprint, and the
  // next ingest rebuilds instead of skipping the change.
  const std::uint64_t fingerprint = catalogFingerprint();
  const auto started = std::chrono::steady_clock::now();
  rebuildNow(/*reuse_unchanged_topics=*/false);
  impl_->recordRebuildCost(started);
  impl_->last_fingerprint = fingerprint;
}

void CatalogModel::rebuildIfChanged() {
  // Most ingest batches only append rows: skip when nothing the catalog lists changed, else re-derive only the
  // topics that changed.
  const std::uint64_t fp = catalogFingerprint();
  if (impl_->last_fingerprint == fp) {
    return;
  }
  const auto started = std::chrono::steady_clock::now();
  rebuildNow(/*reuse_unchanged_topics=*/true);
  impl_->recordRebuildCost(started);
  impl_->last_fingerprint = fp;  // taken before the rebuild — see rebuildFromDatastore
}

void CatalogModel::setMinIngestRebuildGap(std::chrono::milliseconds gap) {
  impl_->min_ingest_rebuild_gap = gap;
}

std::uint64_t CatalogModel::catalogFingerprint() const {
  // Order-independent accumulation (sum of well-mixed per-element hashes) so it
  // does not depend on list*() enumeration order. Allocation-light: only the
  // small vectors returned by list*(); no per-column QString building.
  const auto mix = &mixHash;
  std::uint64_t fp = 0;

  if (impl_->session != nullptr) {
    const DataReader reader = impl_->session->createReader();
    DataEngine& engine = impl_->session->dataEngine();
    ObjectStore& object_store = impl_->session->objectStore();
    // Ephemeral (preview) transform outputs are excluded from the catalog, so they
    // must not move the fingerprint either — otherwise each preview keystroke (which
    // mints a fresh output topic id) would trip a full catalog rebuild.
    const std::unordered_set<TopicId> ephemeral_outputs = impl_->session->derivedScalarTopics(/*ephemeral_only=*/true);
    for (const DatasetId dataset_id : reader.listDatasets()) {
      fp += mix(1, dataset_id);
      // A relabel (setDatasetDisplayName) changes no topic, only the label.
      fp += mix(7, (static_cast<std::uint64_t>(dataset_id) << 32) ^ qHash(engine.datasetLabel(dataset_id)));
      {
        // getTopicStorage() is raw + non-locking; hold the engine lock for the
        // scan. The object store has an INDEPENDENT lock that must never nest
        // with the engine lock, so it is queried after this scope closes.
        const auto lock = engine.lockEngine();
        for (const TopicId topic_id : reader.listTopics(dataset_id)) {
          if (ephemeral_outputs.count(topic_id) > 0) {
            continue;
          }
          std::uint64_t columns = 0;
          std::uint64_t whole_numbers = 0;
          if (const TopicStorage* storage = engine.getTopicStorage(topic_id); storage != nullptr) {
            columns = storage->columnDescriptors().size();
            whole_numbers = storage->wholeNumbersSignature();
            for (const FieldId field_id : storage->retiredColumns()) {
              fp += mix(11, (static_cast<std::uint64_t>(topic_id) << 20) ^ field_id);
            }
          }
          fp += mix(2, (static_cast<std::uint64_t>(topic_id) << 20) ^ columns);
          // A float field gaining its first chunk, or its first fractional value,
          // changes whether it is discrete (ScalarFieldPayload::whole_numbers_only).
          fp += mix(10, whole_numbers ^ static_cast<std::uint64_t>(topic_id));
        }
      }
      for (const ObjectTopicId object_topic_id : object_store.listTopics(dataset_id)) {
        fp += mix(3, object_topic_id.id);
      }
    }
  }

  // Tombstones + display overrides also change catalog output, and some mutators
  // (clearAll, removeDataset) change them WITHOUT calling rebuildFromDatastore —
  // folding them in keeps the gate self-correcting regardless of the trigger.
  for (const DatasetId dataset_id : impl_->removed_datasets) {
    fp += mix(4, dataset_id);
  }
  for (const auto& [dataset_id, names] : impl_->removed_names_per_dataset) {
    fp += mix(5, dataset_id);
    for (const QString& name : names) {
      fp += mix(6, (static_cast<std::uint64_t>(dataset_id) << 32) ^ qHash(name));
    }
  }
  // Advertised placeholders also change catalog output and are mutated outside
  // the samplesIngested path (setAdvertisedTopics), so fold them in to keep the
  // gate self-correcting.
  for (const auto& [dataset_id, topics] : impl_->advertised) {
    fp += mix(8, dataset_id);
    for (const auto& [topic_name, classification] : topics) {
      fp +=
          mix(9, (static_cast<std::uint64_t>(dataset_id) << 32) ^ qHash(topic_name) ^
                     (static_cast<std::uint64_t>(classification) << 8));
    }
  }
  return fp;
}

void CatalogModel::rebuildNow(bool reuse_unchanged_topics) {
  if (impl_->session == nullptr) {
    if (!impl_->items.empty()) {
      impl_->items.clear();
      impl_->dropCaches();
      emit cleared();
    }
    return;
  }

  // In place: the items are edited topic by topic, and a scalar topic whose sources did not change is left alone
  // (Impl::ScalarTopicItems). Otherwise the whole catalog is derived anew and compared with the one before. Both end
  // with the same items and announce the same changes.
  const bool in_place = reuse_unchanged_topics && impl_->topic_items_valid;
  const bool had_items = !impl_->items.empty();
  // Set again once the items and their record agree: a pass cut short (an allocation that fails) leaves them apart,
  // and the next rebuild then derives everything.
  impl_->topic_items_valid = false;
  Impl::ItemMap next_items;  // the whole next catalog; in place, only its items that are not scalar fields
  Impl::ScalarTopicMap next_scalar_topics;
  tsl::robin_set<TopicId> live_topics;  // in place: the scalar topics this pass went through
  std::vector<QString> next_other_keys;
  std::vector<CatalogItem> added_items;
  QStringList removed_keys;
  QStringList changed_keys;
  if (!in_place) {
    next_items.reserve(impl_->items.size());
  }
  const DataReader reader = impl_->session->createReader();
  DataEngine& engine = impl_->session->dataEngine();
  ObjectStore& object_store = impl_->session->objectStore();
  std::vector<DatasetId> dataset_ids = reader.listDatasets();
  // Deterministic order (ascending id): listDatasets() iterates a robin_map whose order is unstable across
  // create/delete/recreate.
  std::sort(dataset_ids.begin(), dataset_ids.end());
  // Ephemeral (preview) transform outputs are intentionally NOT catalogued — the
  // Transform Editor's live preview materializes a real output topic, and it must
  // never surface in the Sources tree regardless of when a rebuild fires.
  const std::unordered_set<TopicId> ephemeral_outputs = impl_->session->derivedScalarTopics(/*ephemeral_only=*/true);

  for (const DatasetId dataset_id : dataset_ids) {
    if (impl_->removed_datasets.count(dataset_id) > 0) {
      continue;
    }
    const Impl::NameSet* removed_names_for_dataset = nullptr;
    if (const auto it = impl_->removed_names_per_dataset.find(dataset_id);
        it != impl_->removed_names_per_dataset.end()) {
      removed_names_for_dataset = &it->second;
    }

    const QString dataset_label = QString::fromStdString(engine.datasetLabel(dataset_id));
    // What every item of the dataset takes from it: its label, and the names removed from it.
    std::uint64_t dataset_hash = mixHash(1, qHash(dataset_label));
    if (removed_names_for_dataset != nullptr) {
      std::uint64_t removed_names = 0;
      for (const QString& name : *removed_names_for_dataset) {
        removed_names += mixHash(2, qHash(name));
      }
      dataset_hash = mixHash(dataset_hash, removed_names);
    }

    for (const TopicId topic_id : reader.listTopics(dataset_id)) {
      if (ephemeral_outputs.count(topic_id) > 0) {
        continue;  // preview node output — never shown in the catalog
      }
      const auto metadata = reader.getMetadata(topic_id);
      if (!metadata.has_value()) {
        continue;
      }

      // Fetch the type tree (itself a locking read) BEFORE the short lock below, so
      // the lock spans only the raw getTopicStorage + column copy. (The recursive
      // mutex would tolerate nesting; this just keeps the hold short.)
      const TypeTreeNode* type_tree = reader.getTypeTree(topic_id);

      // getTopicStorage() is raw + non-locking: its result is valid only while the
      // engine lock is held, so copy the columns under a short lock — a concurrent
      // worker commit / createTopicField cannot then race the descriptor read.
      std::vector<ColumnDescriptor> columns;
      std::vector<bool> whole_numbers_only;
      std::vector<bool> retired;
      std::uint64_t source_hash = 0;
      {
        const auto lock = engine.lockEngine();
        const TopicStorage* storage = engine.getTopicStorage(topic_id);
        if (storage == nullptr) {
          continue;
        }
        std::vector<ColumnDescriptor> built_columns;
        const std::vector<ColumnDescriptor>& live_columns = topicColumns(*storage, type_tree, built_columns);
        source_hash = scalarTopicSourceHash(dataset_hash, metadata->name, live_columns, *storage);
        if (in_place) {
          live_topics.insert(topic_id);
          if (const auto known = impl_->scalar_topics.find(topic_id);
              known != impl_->scalar_topics.end() && known->second.source_hash == source_hash) {
            continue;  // its items are what they would be derived to
          }
        }
        columns = live_columns;
        whole_numbers_only.reserve(columns.size());
        retired.reserve(columns.size());
        for (const ColumnDescriptor& column : columns) {
          retired.push_back(storage->isColumnRetired(column.field_id));
          const StorageKind kind = storageKindOf(column.logical_type);
          const bool is_float = kind == StorageKind::kFloat32 || kind == StorageKind::kFloat64;
          whole_numbers_only.push_back(is_float && storage->wholeNumbersOnly(column.field_id).value_or(false));
        }
      }
      ++impl_->scalar_topics_derived;
      const QString topic_name = QString::fromStdString(metadata->name);  // one string for all the topic's items
      std::vector<CatalogItem> topic_items;
      for (std::size_t column_index = 0; column_index < columns.size(); ++column_index) {
        const ColumnDescriptor& column = columns[column_index];
        // Surface every column in a consumer family (plottable curves and/or
        // discrete state series); only kUnspecified stays out.
        if (retired[column_index] ||
            (!isPlottablePrimitive(column.logical_type) && !isDiscretePrimitive(column.logical_type))) {
          continue;
        }

        const QString key = makeCurveKey(dataset_id, topic_id, column_index);
        if (removed_names_for_dataset != nullptr && removed_names_for_dataset->count(key) > 0) {
          continue;
        }
        const QString field_path = QString::fromStdString(column.field_path);
        topic_items.push_back(
            CatalogItem{
                .key = key,
                .dataset_name = dataset_label,
                .topic_name = topic_name,
                .dataset_id = dataset_id,
                .payload =
                    ScalarFieldPayload{
                        .field_name = field_path,
                        .field_path = field_path,
                        .topic_id = topic_id,
                        .column_index = column_index,
                        .logical_type = column.logical_type,
                        .whole_numbers_only = whole_numbers_only[column_index],
                    },
            });
      }
      Impl::ScalarTopicItems& topic = in_place ? impl_->scalar_topics[topic_id] : next_scalar_topics[topic_id];
      topic.source_hash = source_hash;
      topic.dataset_id = dataset_id;
      topic.topic_name = topic_name;
      if (in_place) {
        impl_->replaceScalarTopicItems(topic, std::move(topic_items), added_items, removed_keys, changed_keys);
      } else {
        topic.keys.reserve(topic_items.size());
        for (CatalogItem& item : topic_items) {
          topic.keys.push_back(item.key);
          next_items.insert_or_assign(topic.keys.back(), std::move(item));
        }
      }
    }

    for (const ObjectTopicId object_topic_id : object_store.listTopics(dataset_id)) {
      const ObjectTopicDescriptor& object_topic = object_store.descriptor(object_topic_id);
      // Marker topics are internal annotation state, not user-facing series — keep
      // them out of the Datasets tree. The overlay reads them straight from the
      // ObjectStore, not via the catalog.
      if (isMarkerTopic(object_topic)) {
        continue;
      }
      const QString key = makeObjectTopicKey(dataset_id, object_topic_id);
      if (removed_names_for_dataset != nullptr && removed_names_for_dataset->count(key) > 0) {
        // Honor the per-dataset removal blacklist for object topics too, or a trashed image topic would come back on
        // the next rebuildFromDatastore.
        continue;
      }
      next_other_keys.push_back(key);
      next_items.insert_or_assign(
          key, CatalogItem{
                   .key = key,
                   .dataset_name = dataset_label,
                   .topic_name = QString::fromStdString(object_topic.topic_name),
                   .dataset_id = dataset_id,
                   .payload =
                       ObjectTopicPayload{
                           .object_topic_id = object_topic_id,
                           .object_type = objectTypeFromMetadata(object_topic.metadata_json),
                           .metadata_json = QString::fromStdString(object_topic.metadata_json),
                       },
               });
    }
  }

  if (in_place) {
    // The scalar topics this pass did not meet are gone, their items with them: a dataset removed or hidden, a topic
    // deleted or turned into a preview output.
    for (auto topic = impl_->scalar_topics.begin(); topic != impl_->scalar_topics.end();) {
      if (live_topics.count(topic->first) > 0) {
        ++topic;
        continue;
      }
      for (const QString& key : topic->second.keys) {
        if (impl_->items.erase(key) > 0) {
          removed_keys.push_back(key);
        }
      }
      topic = impl_->scalar_topics.erase(topic);
    }
  }

  // Merge advertised (available-but-unsubscribed) placeholders: a data-less node
  // for every advertised topic that storage has NOT already produced an entry for
  // (real data wins) and that isn't tombstoned. Superseded automatically once the
  // topic subscribes and its real scalar/object entries appear.
  if (!impl_->advertised.empty()) {
    Impl::NameSet materialized;  // "<dataset_id>\x1f<topic_name>" of every topic storage gave an item
    if (in_place) {
      for (const auto& [topic_id, topic] : impl_->scalar_topics) {
        (void)topic_id;
        if (!topic.keys.empty()) {
          materialized.insert(u"%1\x1f%2"_s.arg(topic.dataset_id).arg(topic.topic_name));
        }
      }
    }
    for (const auto& [key, item] : next_items) {
      (void)key;
      materialized.insert(u"%1\x1f%2"_s.arg(item.dataset_id).arg(item.topic_name));
    }
    for (const auto& [dataset_id, topics] : impl_->advertised) {
      if (impl_->removed_datasets.count(dataset_id) > 0) {
        continue;
      }
      const QString dataset_label = QString::fromStdString(engine.datasetLabel(dataset_id));
      const Impl::NameSet* removed_names_for_dataset = nullptr;
      if (const auto it = impl_->removed_names_per_dataset.find(dataset_id);
          it != impl_->removed_names_per_dataset.end()) {
        removed_names_for_dataset = &it->second;
      }
      for (const auto& [topic_name, classification] : topics) {
        if (materialized.count(u"%1\x1f%2"_s.arg(dataset_id).arg(topic_name)) > 0) {
          continue;  // real data for this topic already surfaced — it wins
        }
        const QString key = makeAdvertisedKey(dataset_id, topic_name);
        if (removed_names_for_dataset != nullptr && removed_names_for_dataset->count(key) > 0) {
          continue;
        }
        next_other_keys.push_back(key);
        next_items.insert_or_assign(
            key, CatalogItem{
                     .key = key,
                     .dataset_name = dataset_label,
                     .topic_name = topic_name,
                     .dataset_id = dataset_id,
                     .payload = AdvertisedTopicPayload{.classification = classification},
                 });
      }
    }
  }

  if (in_place) {
    // The items that are not scalar fields are few: they were derived whole above, and are compared here.
    for (const QString& key : impl_->other_keys) {
      if (next_items.find(key) == next_items.end() && impl_->items.erase(key) > 0) {
        removed_keys.push_back(key);
      }
    }
    for (const auto& [key, item] : next_items) {
      const auto existing = impl_->items.find(key);
      if (existing == impl_->items.end()) {
        added_items.push_back(item);
        impl_->items.insert({key, item});
      } else if (!(existing->second == item)) {
        changed_keys.push_back(key);
        existing.value() = item;
      }
    }
  } else {
    const Impl::ItemMap previous_items = std::move(impl_->items);
    impl_->items = std::move(next_items);
    impl_->scalar_topics = std::move(next_scalar_topics);
    for (const auto& [key, descriptor] : previous_items) {
      const auto current = impl_->items.find(key);
      if (current == impl_->items.end()) {
        removed_keys.push_back(key);
      } else if (!(current->second == descriptor)) {
        changed_keys.push_back(key);
      }
    }
    for (const auto& [key, descriptor] : impl_->items) {
      if (previous_items.find(key) == previous_items.end()) {
        added_items.push_back(descriptor);
      }
    }
  }
  impl_->other_keys = std::move(next_other_keys);
  impl_->dropCaches();
  impl_->topic_items_valid = true;  // dropCaches() clears it; scalar_topics and other_keys describe the items again

  if (had_items && impl_->items.empty()) {
    emit cleared();
    return;
  }

  if (!added_items.empty()) {
    std::sort(added_items.begin(), added_items.end(), catalogItemLess);
    emit itemsAdded(added_items);
    for (const CatalogItem& descriptor : added_items) {
      emit itemAdded(descriptor);
    }
  }
  if (!removed_keys.isEmpty()) {
    emit itemsRemoved(removed_keys);
  }
  if (!changed_keys.isEmpty()) {
    emit itemsChanged(changed_keys);
  }
}

void CatalogModel::clearAll(bool tombstone) {
  if (impl_->items.empty()) {
    return;
  }
  // Tombstone only when the underlying engine data is NOT being erased: the tombstone
  // hides data that lingers in the engine. A real "delete all" erases the engine too,
  // so there is nothing to hide (tombstone=false) and no soft-delete state accumulates.
  if (tombstone) {
    for (const auto& kv : impl_->items) {
      impl_->removed_datasets.insert(kv.second.dataset_id);
    }
  }
  impl_->items.clear();
  impl_->dropCaches();
  impl_->removed_names_per_dataset.clear();
  impl_->advertised.clear();
  impl_->per_topic_pause_capable_datasets.clear();
  emit cleared();
}

void CatalogModel::resetRemovalState() {
  if (impl_->removed_datasets.empty() && impl_->removed_names_per_dataset.empty()) {
    return;
  }
  impl_->removed_datasets.clear();
  impl_->removed_names_per_dataset.clear();
  rebuildFromDatastore();
}

void CatalogModel::restoreDataset(DatasetId dataset_id) {
  const bool was_removed = impl_->removed_datasets.erase(dataset_id) > 0;
  const bool had_per_item = impl_->removed_names_per_dataset.erase(dataset_id) > 0;
  if (was_removed || had_per_item) {
    rebuildFromDatastore();
  }
}

void CatalogModel::setDatasetDisplayName(DatasetId dataset_id, const QString& display_name) {
  if (impl_->session == nullptr) {
    return;
  }
  DataEngine& engine = impl_->session->dataEngine();
  const std::string before = engine.datasetLabel(dataset_id);
  if (engine.setDatasetLabel(dataset_id, display_name.toStdString()) != before) {
    rebuildFromDatastore();
  }
}

void CatalogModel::setAdvertisedTopics(DatasetId dataset_id, const std::vector<AdvertisedTopic>& topics) {
  tsl::robin_map<QString, sdk::BuiltinObjectType, QStringHash> next;
  next.reserve(topics.size());
  for (const AdvertisedTopic& t : topics) {
    next.insert_or_assign(t.topic_name, t.classification);
  }

  const auto existing = impl_->advertised.find(dataset_id);
  const bool had = existing != impl_->advertised.end();
  if (next.empty()) {
    if (!had) {
      return;  // nothing advertised for this dataset — no-op
    }
    impl_->advertised.erase(dataset_id);
  } else {
    // Idempotence: re-advertising an identical set (e.g. on stream reconnect)
    // must not churn the catalog.
    if (had && existing->second.size() == next.size()) {
      bool same = true;
      for (const auto& [name, cls] : next) {
        const auto it = existing->second.find(name);
        if (it == existing->second.end() || it->second != cls) {
          same = false;
          break;
        }
      }
      if (same) {
        return;
      }
    }
    impl_->advertised.insert_or_assign(dataset_id, std::move(next));
  }
  rebuildFromDatastore();
}

void CatalogModel::clearAdvertisedTopics(DatasetId dataset_id) {
  if (impl_->advertised.erase(dataset_id) > 0) {
    rebuildFromDatastore();
  }
}

void CatalogModel::setPerTopicPauseCapable(DatasetId dataset_id, bool capable) {
  if (capable) {
    impl_->per_topic_pause_capable_datasets.insert(dataset_id);
  } else {
    impl_->per_topic_pause_capable_datasets.erase(dataset_id);
  }
}

bool CatalogModel::isPerTopicPauseCapable(DatasetId dataset_id) const {
  return impl_->per_topic_pause_capable_datasets.count(dataset_id) > 0;
}

void CatalogModel::removeItems(const std::vector<QString>& keys) {
  // Accepts any catalog item kind. Object-topic keys are honored too, so the
  // trash/remove flow stays symmetric across scalar fields and object topics
  // — rebuildFromDatastore consults the same blacklist for both, so an entry
  // removed here will not silently come back on the next commit.
  QStringList removed;
  for (const QString& key : keys) {
    const auto it = impl_->items.find(key);
    if (it == impl_->items.end()) {
      continue;
    }
    const DatasetId dataset_id = it->second.dataset_id;
    impl_->removed_names_per_dataset[dataset_id].insert(key);
    impl_->items.erase(it);
    removed.push_back(key);
  }
  impl_->dropCaches();
  if (removed.isEmpty()) {
    return;
  }
  // Emptying the catalog via a trash-selection reaches the same terminal state as
  // removeDataset()/clearAll(): emit cleared() (O(1) view reset + the playback
  // empty-reset hook) rather than a full-key itemsRemoved.
  if (impl_->items.empty()) {
    emit cleared();
  } else {
    emit itemsRemoved(removed);
  }
}

void CatalogModel::removeCurves(const std::vector<QString>& keys) {
  removeItems(keys);
}

bool CatalogModel::removeDataset(DatasetId dataset_id, bool tombstone) {
  std::vector<QString> keys;
  for (const auto& [key, item] : impl_->items) {
    if (item.dataset_id == dataset_id) {
      keys.push_back(key);
    }
  }
  if (keys.empty()) {
    return false;
  }
  // A removed dataset shows no advertised placeholders; on restore the live source
  // re-advertises.
  impl_->advertised.erase(dataset_id);
  impl_->per_topic_pause_capable_datasets.erase(dataset_id);
  // Whole-dataset tombstone (vs removeItems' per-name blacklist): rebuildFromDatastore
  // hides this id until a reload mints a new DatasetId or restoreDataset un-hides it.
  // Skipped for a real "Remove Dataset": the caller erases the engine/object data too, so
  // there is nothing left to hide and no soft-delete state should accumulate.
  if (tombstone) {
    impl_->removed_datasets.insert(dataset_id);
  }
  impl_->removed_names_per_dataset.erase(dataset_id);
  for (const QString& key : keys) {
    impl_->items.erase(key);
  }
  impl_->dropCaches();
  // Empty catalog: one cleared() (views reset in O(1)); otherwise one batched
  // itemsRemoved so consumers react once, not per key.
  if (impl_->items.empty()) {
    emit cleared();
  } else {
    emit itemsRemoved(QStringList(keys.begin(), keys.end()));
  }
  return true;
}

}  // namespace PJ
