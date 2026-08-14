// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/plugin_data_host.hpp"

#include <fmt/format.h>
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nanoarrow/nanoarrow.h"
#include "nanoarrow/nanoarrow.hpp"
#include "pj_base/dataset.hpp"
#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_datastore/arrow_import.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/column_buffer.hpp"
#include "pj_datastore/encoding.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

using DataSourceHandle = PJ_data_source_handle_t;
using TopicHandle = PJ_topic_handle_t;
using FieldHandle = PJ_field_handle_t;

[[nodiscard]] std::string_view toStringView(PJ_string_view_t view) {
  return std::string_view(view.data == nullptr ? "" : view.data, view.size);
}

// Collapse runs of '/' into a single '/' in a topic/field name. Plugin-supplied names
// are opaque, but an empty '/'-segment ("//") carries no meaning in the curve
// hierarchy, so we normalize it here — the one boundary every DataSource/Parser name
// crosses to become a datastore key — so the stored key, the UI catalog (which reads
// the descriptor name) and the by-name transform resolver all agree on a single slash.
// Common case (no "//") allocates nothing: callers guard on `needsSlashNormalization`.
[[nodiscard]] bool needsSlashNormalization(std::string_view name) {
  return name.find("//") != std::string_view::npos;
}

// A field also needs work if it starts with '/' (a leading slash would double up at
// the "topic/field" junction), not just when it has internal "//".
[[nodiscard]] bool needsFieldNormalization(std::string_view name) {
  return needsSlashNormalization(name) || (!name.empty() && name.front() == '/');
}

[[nodiscard]] std::string normalizeSlashes(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  bool prev_slash = false;
  for (const char c : name) {
    if (c == '/' && prev_slash) {
      continue;
    }
    prev_slash = (c == '/');
    out.push_back(c);
  }
  return out;
}

[[nodiscard]] Expected<PrimitiveType> fromAbiType(PJ_primitive_type_t type) {
  const auto raw = static_cast<uint32_t>(type);
  if (raw > static_cast<uint32_t>(PrimitiveType::kString)) {
    return unexpected(fmt::format("unsupported primitive type value {}", raw));
  }
  return static_cast<PrimitiveType>(type);
}

template <typename T>
[[nodiscard]] T loadFromBytes(const uint8_t* data) {
  T value{};
  std::memcpy(&value, data, sizeof(T));
  return value;
}

[[nodiscard]] uint64_t readForOffset(const encoding::FrameOfReferenceEncoded& enc, std::size_t row) {
  const uint8_t* data = enc.offsets.data();
  switch (enc.offset_bytes) {
    case 1:
      return loadFromBytes<uint8_t>(data + row);
    case 2:
      return loadFromBytes<uint16_t>(data + row * 2);
    default:
      return loadFromBytes<uint32_t>(data + row * 4);
  }
}

template <typename T>
[[nodiscard]] T decodeNumericExact(const TopicChunk& chunk, std::size_t col_index, std::size_t row) {
  switch (chunk.columnEncoding(col_index)) {
    case EncodingType::kConstant: {
      const auto& enc = std::get<encoding::ConstantEncoded>(chunk.columns[col_index].data);
      return loadFromBytes<T>(enc.value_bytes.data());
    }
    case EncodingType::kFrameOfReference: {
      const auto& enc = std::get<encoding::FrameOfReferenceEncoded>(chunk.columns[col_index].data);
      const uint64_t offset = readForOffset(enc, row);
      return static_cast<T>(enc.reference + static_cast<int64_t>(offset));
    }
    case EncodingType::kRaw: {
      const StorageKind kind = storageKindOf(chunk.columns[col_index].descriptor->logical_type);
      const uint8_t* base = std::get<RawBuffer>(chunk.columns[col_index].data).data();
      switch (kind) {
        case StorageKind::kFloat32:
          return static_cast<T>(loadFromBytes<float>(base + row * sizeof(float)));
        case StorageKind::kFloat64:
          return static_cast<T>(loadFromBytes<double>(base + row * sizeof(double)));
        case StorageKind::kInt32:
          return static_cast<T>(loadFromBytes<int32_t>(base + row * sizeof(int32_t)));
        case StorageKind::kInt64:
          return static_cast<T>(loadFromBytes<int64_t>(base + row * sizeof(int64_t)));
        case StorageKind::kUint64:
          return static_cast<T>(loadFromBytes<uint64_t>(base + row * sizeof(uint64_t)));
        case StorageKind::kBool:
          return static_cast<T>(chunk.readBool(col_index, row));
        case StorageKind::kString:
          return T{};
      }
      return T{};
    }
    default:
      return T{};
  }
}

void flattenColumnsImpl(
    const TypeTreeNode& node, std::string_view prefix, FieldId& next_id, std::vector<ColumnDescriptor>& out) {
  std::string path = prefix.empty() ? node.name : fmt::format("{}.{}", prefix, node.name);
  switch (node.kind) {
    case TypeKind::kPrimitive: {
      ColumnDescriptor desc;
      desc.field_id = next_id++;
      desc.logical_type = node.primitive_type.value_or(PrimitiveType::kFloat64);
      desc.field_path = std::move(path);
      out.push_back(std::move(desc));
      return;
    }
    case TypeKind::kEnum: {
      ColumnDescriptor desc;
      desc.field_id = next_id++;
      desc.logical_type = node.primitive_type.value_or(PrimitiveType::kInt32);
      desc.field_path = std::move(path);
      out.push_back(std::move(desc));
      return;
    }
    case TypeKind::kStruct:
      for (const auto& child : node.children) {
        flattenColumnsImpl(*child, path, next_id, out);
      }
      return;
    case TypeKind::kArray:
      return;
  }
}

[[nodiscard]] std::vector<ColumnDescriptor> buildSchemaColumns(const TypeTreeNode& root) {
  std::vector<ColumnDescriptor> result;
  FieldId next_id = 0;
  if (root.kind == TypeKind::kStruct) {
    for (const auto& child : root.children) {
      flattenColumnsImpl(*child, "", next_id, result);
    }
  } else {
    flattenColumnsImpl(root, "", next_id, result);
  }
  return result;
}

[[nodiscard]] std::vector<ColumnDescriptor> effectiveColumns(const DataEngine& engine, const TopicStorage& storage) {
  const auto& stored = storage.columnDescriptors();
  if (!stored.empty()) {
    return stored;
  }
  if (const auto* type_tree = engine.typeRegistry().lookup(storage.descriptor().schema_id)) {
    return buildSchemaColumns(*type_tree);
  }
  const auto& chunks = storage.sealedChunks();
  if (!chunks.empty()) {
    std::vector<ColumnDescriptor> result;
    result.reserve(chunks.front().columns.size());
    for (const auto& col : chunks.front().columns) {
      result.push_back(*col.descriptor);
    }
    return result;
  }
  return {};
}

[[nodiscard]] const ColumnDescriptor* findFieldDescriptor(
    const std::vector<ColumnDescriptor>& columns, FieldId field_id) {
  for (const auto& col : columns) {
    if (col.field_id == field_id) {
      return &col;
    }
  }
  return nullptr;
}

}  // namespace

// Engine lock(s) for a write-host operation (driven by the worker while the GUI
// reads). The shared PJ::lockEnginePair (engine.hpp) in this file's vocabulary:
// with a staging engine (the pause/resume copy) armed it locks BOTH via
// std::lock so the worker's write-then-replay can't invert order against flushTo
// (which also std::lock()s both); otherwise a single recursive lock. Recursive so
// the locked methods underneath — and ensureField() nested in appendRecord() —
// re-acquire. The parser-binding ingest route locks through the same helper, so
// the two write paths can never drift out of lock order.
[[nodiscard]] inline EngineLockPair lockWriteEngines(DataEngine& live, DataEngine* staging) {
  return lockEnginePair(live, staging);
}

struct WriteCore {
  explicit WriteCore(DataEngine& data_engine) : engine(data_engine), writer(data_engine.createWriter()) {}

  DataEngine& engine;
  DataWriter writer;
  // Optional secondary engine for the streaming pause/resume two-engine
  // lockstep. When non-null, every ensureTopic/ensureField that lands a new
  // id in `engine_` is mirrored into `*secondary_engine_` with the SAME id —
  // so a cached plugin TopicHandle/FieldHandle still resolves after the host
  // swaps which engine is the active write target. Mirroring happens inside
  // ensureTopic/ensureField so it covers both the explicit C-ABI register
  // paths AND the lazy "first-write creates the column" paths inside
  // appendRecord / appendBoundRecord / appendArrowStream.
  DataEngine* secondary_engine = nullptr;
  std::string last_error;

  struct DatasetTopicKey {
    DatasetId dataset_id;
    std::string topic_name;

    friend bool operator==(const DatasetTopicKey& a, const DatasetTopicKey& b) {
      return a.dataset_id == b.dataset_id && a.topic_name == b.topic_name;
    }
  };

  struct DatasetTopicKeyHash {
    std::size_t operator()(const DatasetTopicKey& key) const noexcept {
      std::size_t h1 = std::hash<DatasetId>{}(key.dataset_id);
      std::size_t h2 = std::hash<std::string>{}(key.topic_name);
      return h1 ^ (h2 << 1);
    }
  };

  struct TopicFieldKey {
    TopicId topic_id;
    std::string field_name;

    friend bool operator==(const TopicFieldKey& a, const TopicFieldKey& b) {
      return a.topic_id == b.topic_id && a.field_name == b.field_name;
    }
  };

  // Borrowed lookup key: lets the per-record hot path probe field_cache without
  // heap-allocating a std::string for the field name on every (cache-hit) field
  // — the dominant temporary-allocation source during ingest. Heterogeneous
  // lookup is enabled by the is_transparent hash/equal below.
  struct TopicFieldKeyView {
    TopicId topic_id;
    std::string_view field_name;
  };

  struct TopicFieldKeyHash {
    using is_transparent = void;
    std::size_t operator()(const TopicFieldKey& key) const noexcept {
      return hash(key.topic_id, key.field_name);
    }
    std::size_t operator()(const TopicFieldKeyView& key) const noexcept {
      return hash(key.topic_id, key.field_name);
    }

   private:
    // Hash the name as a string_view in BOTH overloads so an owning key and a
    // borrowed view with equal content always hash identically.
    static std::size_t hash(TopicId topic_id, std::string_view field_name) noexcept {
      std::size_t h1 = std::hash<TopicId>{}(topic_id);
      std::size_t h2 = std::hash<std::string_view>{}(field_name);
      return h1 ^ (h2 << 1);
    }
  };

  // Transparent equality covering owning/view in either position, so
  // robin_map::find can accept a borrowed TopicFieldKeyView. (std::string ==
  // std::string_view compares by content.)
  struct TopicFieldKeyEq {
    using is_transparent = void;
    template <typename A, typename B>
    bool operator()(const A& a, const B& b) const noexcept {
      return a.topic_id == b.topic_id && a.field_name == b.field_name;
    }
  };

  struct TopicFieldIdKey {
    TopicId topic_id;
    FieldId field_id;

    friend bool operator==(const TopicFieldIdKey& a, const TopicFieldIdKey& b) {
      return a.topic_id == b.topic_id && a.field_id == b.field_id;
    }
  };

  struct TopicFieldIdKeyHash {
    std::size_t operator()(const TopicFieldIdKey& key) const noexcept {
      std::size_t h1 = std::hash<TopicId>{}(key.topic_id);
      std::size_t h2 = std::hash<FieldId>{}(key.field_id);
      return h1 ^ (h2 << 1);
    }
  };

  tsl::robin_map<DatasetTopicKey, TopicHandle, DatasetTopicKeyHash> topic_cache;
  tsl::robin_map<TopicFieldKey, FieldHandle, TopicFieldKeyHash, TopicFieldKeyEq> field_cache;
  tsl::robin_map<TopicFieldIdKey, PrimitiveType, TopicFieldIdKeyHash> field_types;

  void setError(std::string message) {
    last_error = std::move(message);
  }

  [[nodiscard]] const char* lastError() const {
    return last_error.empty() ? nullptr : last_error.c_str();
  }

  [[nodiscard]] bool createDataSource(std::string_view name, DataSourceHandle* out_source) {
    auto engine_locks = lockWriteEngines(engine, secondary_engine);
    // Each plugin-created source gets its OWN time domain, exactly as the app's
    // own file/stream loaders do. The per-source display offset lives on the
    // domain, so a dataset left on the default domain (id 0) can never carry
    // one — its Source Timeline bar would draw but refuse every drag.
    auto td_or = engine.createTimeDomain(std::string(name));
    if (!td_or.has_value()) {
      setError(td_or.error());
      return false;
    }
    // Same-id mirror per the two-engine lockstep invariant above, done before
    // the dataset exists so a failed mirror leaves nothing half-created.
    if (secondary_engine != nullptr) {
      auto mirror_or = secondary_engine->createTimeDomain(std::string(name), *td_or);
      if (!mirror_or.has_value()) {
        setError(fmt::format("secondary mirror createTimeDomain failed: {}", mirror_or.error()));
        return false;
      }
    }
    auto id_or = engine.createDataset(DatasetDescriptor{.source_name = std::string(name), .time_domain_id = *td_or});
    if (!id_or.has_value()) {
      setError(id_or.error());
      return false;
    }
    *out_source = DataSourceHandle{.id = *id_or};
    last_error.clear();
    return true;
  }

  // Idempotent re-mirror of a topic onto the secondary engine. Called on
  // every successful ensureTopic path (including cache hits) so a previously
  // failed mirror is retried instead of leaving the engines diverged forever
  // behind the cache.
  [[nodiscard]] bool mirrorTopicToSecondary(DatasetId dataset_id, TopicId topic_id, std::string_view topic_name) {
    if (secondary_engine == nullptr) {
      return true;
    }
    if (secondary_engine->getTopicStorage(topic_id) != nullptr) {
      return true;  // already mirrored (most common case)
    }
    TopicDescriptor desc;
    desc.name = std::string(topic_name);
    desc.schema_id = 0;
    auto mirror_or = secondary_engine->createTopic(dataset_id, std::move(desc), topic_id);
    if (!mirror_or.has_value()) {
      setError(fmt::format("secondary mirror createTopic failed: {}", mirror_or.error()));
      return false;
    }
    return true;
  }

  [[nodiscard]] bool ensureTopic(DataSourceHandle source, std::string_view topic_name, TopicHandle* out_topic) {
    // Normalize "//" → "/" once, at the ingest boundary, so the stored topic name,
    // the catalog and the by-name transform resolver all key off the same string.
    std::string topic_name_normalized;
    if (needsSlashNormalization(topic_name)) {
      topic_name_normalized = normalizeSlashes(topic_name);
      topic_name = topic_name_normalized;
    }
    auto engine_locks = lockWriteEngines(engine, secondary_engine);
    const auto* dataset = engine.getDataset(source.id);
    if (dataset == nullptr) {
      setError(fmt::format("data source {} not found", source.id));
      return false;
    }

    DatasetTopicKey key{.dataset_id = source.id, .topic_name = std::string(topic_name)};
    if (auto it = topic_cache.find(key); it != topic_cache.end()) {
      // Cache hit — but the secondary may still be missing this topic if a
      // prior mirror failed; retry. No-op if already mirrored.
      if (!mirrorTopicToSecondary(source.id, it->second.id, topic_name)) {
        return false;
      }
      *out_topic = it->second;
      last_error.clear();
      return true;
    }

    auto topic_ids = engine.listTopics(source.id);
    std::sort(topic_ids.begin(), topic_ids.end());
    for (TopicId tid : topic_ids) {
      const auto* storage = engine.getTopicStorage(tid);
      if (storage != nullptr && storage->descriptor().name == topic_name) {
        // Found in primary — same retry semantics on the secondary.
        if (!mirrorTopicToSecondary(source.id, tid, topic_name)) {
          return false;
        }
        *out_topic = TopicHandle{.id = tid};
        topic_cache.emplace(std::move(key), *out_topic);
        last_error.clear();
        return true;
      }
    }

    TopicDescriptor desc;
    desc.name = std::string(topic_name);
    desc.schema_id = 0;
    auto tid_or = writer.registerTopic(source.id, std::move(desc));
    if (!tid_or.has_value()) {
      setError(tid_or.error());
      return false;
    }

    // Lockstep mirror to the secondary engine (streaming two-engine model).
    // Forces the same TopicId on both sides so a later setTarget(secondary)
    // swap finds the topic that the plugin's cached TopicHandle refers to.
    // See DataEngine::createTopicField for the field-level counterpart.
    if (!mirrorTopicToSecondary(source.id, *tid_or, topic_name)) {
      return false;
    }

    *out_topic = TopicHandle{.id = *tid_or};
    topic_cache.emplace(std::move(key), *out_topic);
    last_error.clear();
    return true;
  }

  [[nodiscard]] bool lookupFieldType(TopicHandle topic, FieldId field_id, PrimitiveType* out_type) {
    const TopicFieldIdKey key{.topic_id = topic.id, .field_id = field_id};
    if (auto it = field_types.find(key); it != field_types.end()) {
      *out_type = it->second;
      return true;
    }

    const auto* storage = engine.getTopicStorage(topic.id);
    if (storage == nullptr) {
      setError(fmt::format("topic {} not found", topic.id));
      return false;
    }
    const auto columns = effectiveColumns(engine, *storage);
    const auto* desc = findFieldDescriptor(columns, field_id);
    if (desc == nullptr) {
      setError(fmt::format("field {} not found in topic {}", field_id, topic.id));
      return false;
    }

    *out_type = desc->logical_type;
    field_types[key] = desc->logical_type;
    field_cache[{.topic_id = topic.id, .field_name = desc->field_path}] = FieldHandle{.topic = topic, .id = field_id};
    return true;
  }

  [[nodiscard]] bool ensureField(
      TopicHandle topic, std::string_view field_name, PJ_primitive_type_t abi_type, FieldHandle* out_field) {
    // Normalize the field name on every entry point (direct ensureField and the lazy
    // auto-create inside appendRecord). A field path is relative to its topic, so two
    // things get fixed: internal runs "//" collapse to "/", and a leading '/' (e.g. a
    // CSV column header "/robot/pose/x") is dropped — otherwise the UI, which joins
    // topic + '/' + field, would render "topic//robot/pose/x".
    std::string field_name_normalized;
    if (needsFieldNormalization(field_name)) {
      field_name_normalized = normalizeSlashes(field_name);
      if (!field_name_normalized.empty() && field_name_normalized.front() == '/') {
        field_name_normalized.erase(0, 1);
      }
      field_name = field_name_normalized;
    }
    auto engine_locks = lockWriteEngines(engine, secondary_engine);
    const auto* storage = engine.getTopicStorage(topic.id);
    if (storage == nullptr) {
      setError(fmt::format("topic {} not found", topic.id));
      return false;
    }

    auto type_or = fromAbiType(abi_type);
    if (!type_or.has_value()) {
      setError(type_or.error());
      return false;
    }
    const PrimitiveType type = *type_or;

    // Borrowed probe: no std::string is allocated on the common cache-hit path
    // (every field after a topic's first record). The owning key is built only
    // when a new field is inserted below.
    if (auto it = field_cache.find(TopicFieldKeyView{.topic_id = topic.id, .field_name = field_name});
        it != field_cache.end()) {
      PrimitiveType existing{};
      if (!lookupFieldType(topic, it->second.id, &existing)) {
        return false;
      }
      if (existing != type) {
        setError(fmt::format("field '{}' already exists with a different type", field_name));
        return false;
      }
      // Idempotent re-mirror on cache hit — if a previous mirror failed, this
      // re-attempts it. createTopicField returns the existing FieldId when
      // (name, type) already matches in the secondary, so it's a cheap no-op
      // in the steady state.
      if (secondary_engine != nullptr) {
        auto mirror_or =
            secondary_engine->createTopicField(topic.id, field_name, type, std::optional<FieldId>{it->second.id});
        if (!mirror_or.has_value()) {
          setError(fmt::format("secondary mirror createTopicField failed: {}", mirror_or.error()));
          return false;
        }
      }
      *out_field = it->second;
      last_error.clear();
      return true;
    }

    auto field_id_or = writer.ensureColumn(topic.id, field_name, type);
    if (!field_id_or.has_value()) {
      setError(field_id_or.error());
      return false;
    }

    // Lockstep mirror to the secondary engine (streaming two-engine model).
    // Forces the same FieldId via DataEngine::createTopicField so that a
    // cached FieldHandle resolves identically on either engine after a
    // setTarget swap. Runs from EVERY path that lands here — explicit C-ABI
    // ensureField AND the lazy "auto-create column on first non-null write"
    // path inside appendRecord/appendBoundRecord/appendArrowStream — because
    // they all funnel through this method.
    //
    // requested_id is wrapped in std::optional so FieldId 0 (the first field
    // of any topic) is forced, not auto-assigned. A naive `FieldId == 0`
    // sentinel would silently mis-assign the first field of every topic.
    if (secondary_engine != nullptr) {
      auto mirror_or =
          secondary_engine->createTopicField(topic.id, field_name, type, std::optional<FieldId>{*field_id_or});
      if (!mirror_or.has_value()) {
        setError(fmt::format("secondary mirror createTopicField failed: {}", mirror_or.error()));
        return false;
      }
    }

    *out_field = FieldHandle{.topic = topic, .id = *field_id_or};
    field_cache.emplace(TopicFieldKey{.topic_id = topic.id, .field_name = std::string(field_name)}, *out_field);
    field_types[{.topic_id = topic.id, .field_id = *field_id_or}] = type;
    last_error.clear();
    return true;
  }

  [[nodiscard]] bool validateScalar(const PJ_scalar_value_t& value, PrimitiveType expected, std::string_view where) {
    auto actual_or = fromAbiType(value.type);
    if (!actual_or.has_value()) {
      setError(actual_or.error());
      return false;
    }
    if (*actual_or != expected) {
      setError(fmt::format("{}: scalar type mismatch", where));
      return false;
    }
    return true;
  }

  void setFieldValue(
      TopicId topic_id, std::size_t col_index, PrimitiveType logical_type, const PJ_scalar_value_t& value) {
    switch (logical_type) {
      case PrimitiveType::kFloat32:
        writer.set(topic_id, col_index, value.data.as_float32);
        break;
      case PrimitiveType::kFloat64:
        writer.set(topic_id, col_index, value.data.as_float64);
        break;
      case PrimitiveType::kInt8:
        writer.set(topic_id, col_index, static_cast<int64_t>(value.data.as_int8));
        break;
      case PrimitiveType::kInt16:
        writer.set(topic_id, col_index, static_cast<int64_t>(value.data.as_int16));
        break;
      case PrimitiveType::kInt32:
        writer.set(topic_id, col_index, value.data.as_int32);
        break;
      case PrimitiveType::kInt64:
        writer.set(topic_id, col_index, value.data.as_int64);
        break;
      case PrimitiveType::kUint8:
        writer.set(topic_id, col_index, static_cast<uint64_t>(value.data.as_uint8));
        break;
      case PrimitiveType::kUint16:
        writer.set(topic_id, col_index, static_cast<uint64_t>(value.data.as_uint16));
        break;
      case PrimitiveType::kUint32:
        writer.set(topic_id, col_index, static_cast<uint64_t>(value.data.as_uint32));
        break;
      case PrimitiveType::kUint64:
        writer.set(topic_id, col_index, value.data.as_uint64);
        break;
      case PrimitiveType::kBool:
        writer.set(topic_id, col_index, value.data.as_bool != 0);
        break;
      case PrimitiveType::kString:
        writer.set(topic_id, col_index, toStringView(value.data.as_string));
        break;
      case PrimitiveType::kUnspecified:
        break;
    }
  }

  [[nodiscard]] bool appendRecord(
      TopicHandle topic, Timestamp timestamp, const PJ_named_field_value_t* fields, std::size_t field_count) {
    auto engine_locks = lockWriteEngines(engine, secondary_engine);
    if (engine.getTopicStorage(topic.id) == nullptr) {
      setError(fmt::format("topic {} not found", topic.id));
      return false;
    }

    tsl::robin_set<std::string_view> seen_names;
    struct ResolvedField {
      FieldHandle handle;
      PrimitiveType type;
      const PJ_named_field_value_t* raw;
    };
    std::vector<ResolvedField> resolved;
    resolved.reserve(field_count);
    // Backing store for names that needed "//" → "/" normalization; a deque keeps
    // element addresses stable so the string_views below (and in seen_names) stay
    // valid for the whole record. Untouched on the common (no-"//") path.
    std::deque<std::string> normalized_names;
    for (std::size_t i = 0; i < field_count; ++i) {
      const auto& field = fields[i];
      auto name = toStringView(field.name);
      if (needsFieldNormalization(name)) {
        std::string norm = normalizeSlashes(name);
        if (!norm.empty() && norm.front() == '/') {
          norm.erase(0, 1);
        }
        normalized_names.push_back(std::move(norm));
        name = normalized_names.back();
      }
      if (!seen_names.insert(name).second) {
        setError(fmt::format("duplicate field name '{}'", name));
        return false;
      }
      if (field.is_null) {
        // Null values: look up existing field by name (borrowed probe, no alloc).
        auto it = field_cache.find(TopicFieldKeyView{.topic_id = topic.id, .field_name = name});
        if (it == field_cache.end()) {
          // Field has never been seen. Check if this is a typed null (the ABI
          // carries value.type even when is_null is true). A valid type lets
          // us create the column now; an untyped null (kNull) is silently
          // skipped — the column will be created when a non-null value arrives.
          auto type_or = fromAbiType(field.value.type);
          if (type_or.has_value()) {
            FieldHandle handle{};
            if (!ensureField(topic, name, field.value.type, &handle)) {
              return false;
            }
            resolved.push_back({handle, *type_or, &field});
          }
          continue;
        }
        PrimitiveType existing{};
        if (!lookupFieldType(topic, it->second.id, &existing)) {
          return false;
        }
        resolved.push_back({it->second, existing, &field});
      } else {
        auto type_or = fromAbiType(field.value.type);
        if (!type_or.has_value()) {
          setError(type_or.error());
          return false;
        }
        FieldHandle handle{};
        if (!ensureField(topic, name, field.value.type, &handle)) {
          return false;
        }
        if (!validateScalar(field.value, *type_or, "appendRecord")) {
          return false;
        }
        resolved.push_back({handle, *type_or, &field});
      }
    }

    auto begin_status = writer.beginRow(topic.id, timestamp);
    if (!begin_status.has_value()) {
      setError(begin_status.error());
      return false;
    }
    for (const auto& field : resolved) {
      if (field.raw->is_null) {
        writer.setNull(topic.id, static_cast<std::size_t>(field.handle.id));
      } else {
        setFieldValue(topic.id, static_cast<std::size_t>(field.handle.id), field.type, field.raw->value);
      }
    }
    auto finish_status = writer.finishRow(topic.id);
    if (!finish_status.has_value()) {
      setError(finish_status.error());
      return false;
    }
    last_error.clear();
    return true;
  }

  [[nodiscard]] bool appendBoundRecord(
      TopicHandle topic, Timestamp timestamp, const PJ_bound_field_value_t* fields, std::size_t field_count) {
    auto engine_locks = lockWriteEngines(engine, secondary_engine);
    if (engine.getTopicStorage(topic.id) == nullptr) {
      setError(fmt::format("topic {} not found", topic.id));
      return false;
    }

    tsl::robin_set<FieldId> seen_ids;
    struct ResolvedField {
      PrimitiveType type;
      const PJ_bound_field_value_t* raw;
    };
    std::vector<ResolvedField> resolved;
    resolved.reserve(field_count);
    for (std::size_t i = 0; i < field_count; ++i) {
      const auto& field = fields[i];
      if (field.field.topic.id != topic.id) {
        setError("field handle does not belong to the target topic");
        return false;
      }
      if (!seen_ids.insert(field.field.id).second) {
        setError(fmt::format("duplicate field id {}", field.field.id));
        return false;
      }
      PrimitiveType type{};
      if (!lookupFieldType(topic, field.field.id, &type)) {
        return false;
      }
      if (!field.is_null && !validateScalar(field.value, type, "appendBoundRecord")) {
        return false;
      }
      resolved.push_back({type, &field});
    }

    auto begin_status = writer.beginRow(topic.id, timestamp);
    if (!begin_status.has_value()) {
      setError(begin_status.error());
      return false;
    }
    for (const auto& field : resolved) {
      if (field.raw->is_null) {
        writer.setNull(topic.id, static_cast<std::size_t>(field.raw->field.id));
      } else {
        setFieldValue(topic.id, static_cast<std::size_t>(field.raw->field.id), field.type, field.raw->value);
      }
    }
    auto finish_status = writer.finishRow(topic.id);
    if (!finish_status.has_value()) {
      setError(finish_status.error());
      return false;
    }
    last_error.clear();
    return true;
  }

  /// Ingest a whole Arrow C Data Interface stream into a topic.
  ///
  /// Ownership contract: callers pass a producer-owned @p stream. The caller
  /// decides whether to release after this call — this method does NOT
  /// call stream->release. That lets the outermost ABI trampoline enforce
  /// the "success releases, failure retains" rule uniformly.
  [[nodiscard]] bool appendArrowStream(
      TopicHandle topic, struct ArrowArrayStream* stream, PJ_string_view_t timestamp_column) {
    auto engine_locks = lockWriteEngines(engine, secondary_engine);
    if (stream == nullptr) {
      setError("append_arrow_stream: null stream");
      return false;
    }
    if (engine.getTopicStorage(topic.id) == nullptr) {
      setError(fmt::format("topic {} not found", topic.id));
      return false;
    }

    auto schema_or = arrow_import::schemaFromArrowStream(stream);
    if (!schema_or.has_value()) {
      setError(schema_or.error());
      return false;
    }

    const std::string_view timestamp_name = toStringView(timestamp_column);
    int ts_arrow_col = -1;
    std::vector<arrow_import::ArrowColumnMapping> mappings;
    for (const auto& mapping : schema_or->second) {
      if (!timestamp_name.empty() && mapping.field_name == timestamp_name) {
        ts_arrow_col = mapping.arrow_column_index;
        continue;
      }

      FieldHandle field{};
      if (!ensureField(topic, mapping.field_name, static_cast<PJ_primitive_type_t>(mapping.pj_type), &field)) {
        return false;
      }
      auto adjusted = mapping;
      adjusted.pj_column_index = static_cast<std::size_t>(field.id);
      mappings.push_back(std::move(adjusted));
    }

    if (!timestamp_name.empty() && ts_arrow_col < 0) {
      setError(fmt::format("timestamp column '{}' not found in stream schema", timestamp_name));
      return false;
    }

    auto status = arrow_import::importArrowStream(writer, topic.id, stream, mappings, ts_arrow_col);
    if (!status.has_value()) {
      setError(status.error());
      return false;
    }
    last_error.clear();
    return true;
  }

  void flushPending() {
    auto lock = engine.lockEngine();
    auto flushed = writer.flushAll();
    if (!flushed.empty()) {
      engine.commitChunks(std::move(flushed));
    }
  }
};

struct CatalogSnapshotState {
  std::deque<std::string> names;
  std::vector<PJ_data_source_info_t> data_sources;
  std::vector<PJ_topic_info_t> topics;
  std::vector<PJ_field_info_t> fields;
};

void releaseCatalogSnapshot(void* ctx) {
  delete static_cast<CatalogSnapshotState*>(ctx);
}

PJ_string_view_t storeString(CatalogSnapshotState& state, std::string_view value) {
  state.names.emplace_back(value);
  const auto& stored = state.names.back();
  return PJ_string_view_t{stored.data(), stored.size()};
}

struct ToolboxCore {
  explicit ToolboxCore(DataEngine& data_engine) : write(data_engine), engine(data_engine) {}

  WriteCore write;
  DataEngine& engine;

  [[nodiscard]] bool acquireCatalogSnapshot(PJ_catalog_snapshot_t* out_snapshot) {
    auto lock = engine.lockEngine();  // held through the deep-copy below
    auto* state = new CatalogSnapshotState{};
    auto dataset_ids = engine.listDatasets();
    std::sort(dataset_ids.begin(), dataset_ids.end());
    state->data_sources.reserve(dataset_ids.size());

    for (DatasetId ds_id : dataset_ids) {
      const auto* dataset = engine.getDataset(ds_id);
      if (dataset == nullptr) {
        continue;
      }

      const uint32_t first_topic = static_cast<uint32_t>(state->topics.size());
      auto topic_ids = engine.listTopics(ds_id);
      std::sort(topic_ids.begin(), topic_ids.end());
      for (TopicId tid : topic_ids) {
        const auto* storage = engine.getTopicStorage(tid);
        if (storage == nullptr) {
          continue;
        }
        const uint32_t first_field = static_cast<uint32_t>(state->fields.size());
        const auto columns = effectiveColumns(engine, *storage);
        for (const auto& col : columns) {
          state->fields.push_back(
              PJ_field_info_t{
                  .handle = FieldHandle{.topic = TopicHandle{.id = tid}, .id = col.field_id},
                  .name = storeString(*state, col.field_path),
                  .type = static_cast<PJ_primitive_type_t>(col.logical_type),
              });
        }
        state->topics.push_back(
            PJ_topic_info_t{
                .handle = TopicHandle{.id = tid},
                .source = DataSourceHandle{.id = ds_id},
                .name = storeString(*state, storage->descriptor().name),
                .first_field = first_field,
                .field_count = static_cast<uint32_t>(state->fields.size()) - first_field,
            });
      }

      state->data_sources.push_back(
          PJ_data_source_info_t{
              .handle = DataSourceHandle{.id = ds_id},
              .name = storeString(*state, dataset->source_name),
              .first_topic = first_topic,
              .topic_count = static_cast<uint32_t>(state->topics.size()) - first_topic,
          });
    }

    *out_snapshot = PJ_catalog_snapshot_t{
        .data_sources = state->data_sources.data(),
        .data_source_count = state->data_sources.size(),
        .topics = state->topics.data(),
        .topic_count = state->topics.size(),
        .fields = state->fields.data(),
        .field_count = state->fields.size(),
        .release_ctx = state,
        .release = releaseCatalogSnapshot,
    };
    write.last_error.clear();
    return true;
  }

  // v4: materialise one field's time series into host-owned Arrow structs.
  // Output is a struct array with 2 columns: ["timestamp" (int64),
  // <field_name> (typed)]. The caller must invoke out_schema->release and
  // out_array->release when done; release callbacks are set by nanoarrow
  // and free all allocated buffers.
  [[nodiscard]] bool readSeriesArrow(FieldHandle field, struct ArrowSchema* out_schema, struct ArrowArray* out_array) {
    if (out_schema == nullptr || out_array == nullptr) {
      write.setError("readSeriesArrow: out_schema and out_array must be non-null");
      return false;
    }
    auto lock = engine.lockEngine();  // held through the row-decode loop below

    const auto* storage = engine.getTopicStorage(field.topic.id);
    if (storage == nullptr) {
      write.setError(fmt::format("topic {} not found", field.topic.id));
      return false;
    }
    const auto columns = effectiveColumns(engine, *storage);
    const auto* desc = findFieldDescriptor(columns, field.id);
    if (desc == nullptr) {
      write.setError(fmt::format("field {} not found in topic {}", field.id, field.topic.id));
      return false;
    }

    const ArrowType value_arrow_type = [&]() {
      switch (desc->logical_type) {
        case PrimitiveType::kFloat32:
          return NANOARROW_TYPE_FLOAT;
        case PrimitiveType::kFloat64:
          return NANOARROW_TYPE_DOUBLE;
        case PrimitiveType::kInt8:
          return NANOARROW_TYPE_INT8;
        case PrimitiveType::kInt16:
          return NANOARROW_TYPE_INT16;
        case PrimitiveType::kInt32:
          return NANOARROW_TYPE_INT32;
        case PrimitiveType::kInt64:
          return NANOARROW_TYPE_INT64;
        case PrimitiveType::kUint8:
          return NANOARROW_TYPE_UINT8;
        case PrimitiveType::kUint16:
          return NANOARROW_TYPE_UINT16;
        case PrimitiveType::kUint32:
          return NANOARROW_TYPE_UINT32;
        case PrimitiveType::kUint64:
          return NANOARROW_TYPE_UINT64;
        case PrimitiveType::kBool:
          return NANOARROW_TYPE_BOOL;
        case PrimitiveType::kString:
          return NANOARROW_TYPE_STRING;
        case PrimitiveType::kUnspecified:
          return NANOARROW_TYPE_NA;
      }
      return NANOARROW_TYPE_NA;
    }();

    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    if (ArrowSchemaSetTypeStruct(schema.get(), 2) != NANOARROW_OK) {
      write.setError("readSeriesArrow: ArrowSchemaSetTypeStruct failed");
      return false;
    }
    ArrowSchemaInit(schema->children[0]);
    if (ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64) != NANOARROW_OK ||
        ArrowSchemaSetName(schema->children[0], "timestamp") != NANOARROW_OK) {
      write.setError("readSeriesArrow: failed to set timestamp child schema");
      return false;
    }
    ArrowSchemaInit(schema->children[1]);
    if (ArrowSchemaSetType(schema->children[1], value_arrow_type) != NANOARROW_OK ||
        ArrowSchemaSetName(schema->children[1], desc->field_path.c_str()) != NANOARROW_OK) {
      write.setError("readSeriesArrow: failed to set value child schema");
      return false;
    }

    nanoarrow::UniqueArray array;
    ArrowError arrow_err;
    if (ArrowArrayInitFromSchema(array.get(), schema.get(), &arrow_err) != NANOARROW_OK) {
      write.setError(std::string("readSeriesArrow: ArrowArrayInitFromSchema failed: ") + arrow_err.message);
      return false;
    }
    if (ArrowArrayStartAppending(array.get()) != NANOARROW_OK) {
      write.setError("readSeriesArrow: ArrowArrayStartAppending failed");
      return false;
    }

    auto* ts_child = array->children[0];
    auto* val_child = array->children[1];

    // Rows below the topic's retention floor are logically evicted: a toolbox
    // plugin reading this series must not see them (the retention contract).
    const auto retention_floor = storage->retentionFloor();
    for (const auto& chunk : storage->sealedChunks()) {
      if (chunk.stats.t_max < retention_floor) {
        continue;  // whole chunk below the floor
      }
      int col_index = -1;
      for (std::size_t i = 0; i < chunk.columns.size(); ++i) {
        if (chunk.columns[i].descriptor->field_id == field.id) {
          col_index = static_cast<int>(i);
          break;
        }
      }
      if (col_index < 0) {
        continue;
      }
      const auto col_sz = static_cast<std::size_t>(col_index);

      for (uint32_t row = 0; row < chunk.stats.row_count; ++row) {
        // Skip logically-evicted rows; ts_child and val_child stay in sync
        // because both appends are gated by this single guard.
        const auto ts = chunk.readTimestamp(row);
        if (ts < retention_floor) {
          continue;
        }
        if (ArrowArrayAppendInt(ts_child, ts) != NANOARROW_OK) {
          write.setError("readSeriesArrow: timestamp append failed");
          return false;
        }

        const bool is_null = chunk.isNull(col_sz, row);
        if (is_null) {
          if (ArrowArrayAppendNull(val_child, 1) != NANOARROW_OK) {
            write.setError("readSeriesArrow: null append failed");
            return false;
          }
        } else {
          ArrowErrorCode rc = NANOARROW_OK;
          switch (desc->logical_type) {
            case PrimitiveType::kFloat32:
              rc = ArrowArrayAppendDouble(val_child, decodeNumericExact<float>(chunk, col_sz, row));
              break;
            case PrimitiveType::kFloat64:
              rc = ArrowArrayAppendDouble(val_child, decodeNumericExact<double>(chunk, col_sz, row));
              break;
            case PrimitiveType::kInt8:
              rc = ArrowArrayAppendInt(val_child, decodeNumericExact<int8_t>(chunk, col_sz, row));
              break;
            case PrimitiveType::kInt16:
              rc = ArrowArrayAppendInt(val_child, decodeNumericExact<int16_t>(chunk, col_sz, row));
              break;
            case PrimitiveType::kInt32:
              rc = ArrowArrayAppendInt(val_child, decodeNumericExact<int32_t>(chunk, col_sz, row));
              break;
            case PrimitiveType::kInt64:
              rc = ArrowArrayAppendInt(val_child, decodeNumericExact<int64_t>(chunk, col_sz, row));
              break;
            case PrimitiveType::kUint8:
              rc = ArrowArrayAppendUInt(val_child, decodeNumericExact<uint8_t>(chunk, col_sz, row));
              break;
            case PrimitiveType::kUint16:
              rc = ArrowArrayAppendUInt(val_child, decodeNumericExact<uint16_t>(chunk, col_sz, row));
              break;
            case PrimitiveType::kUint32:
              rc = ArrowArrayAppendUInt(val_child, decodeNumericExact<uint32_t>(chunk, col_sz, row));
              break;
            case PrimitiveType::kUint64:
              rc = ArrowArrayAppendUInt(val_child, decodeNumericExact<uint64_t>(chunk, col_sz, row));
              break;
            case PrimitiveType::kBool:
              rc = ArrowArrayAppendInt(val_child, chunk.readBool(col_sz, row) ? 1 : 0);
              break;
            case PrimitiveType::kString: {
              const auto text = chunk.readString(col_sz, row);
              const ArrowStringView sv{text.data(), static_cast<int64_t>(text.size())};
              rc = ArrowArrayAppendString(val_child, sv);
              break;
            }
            case PrimitiveType::kUnspecified:
              rc = ArrowArrayAppendNull(val_child, 1);
              break;
          }
          if (rc != NANOARROW_OK) {
            write.setError("readSeriesArrow: value append failed");
            return false;
          }
        }

        if (ArrowArrayFinishElement(array.get()) != NANOARROW_OK) {
          write.setError("readSeriesArrow: ArrowArrayFinishElement failed");
          return false;
        }
      }
    }

    if (ArrowArrayFinishBuildingDefault(array.get(), &arrow_err) != NANOARROW_OK) {
      write.setError(std::string("readSeriesArrow: finish building failed: ") + arrow_err.message);
      return false;
    }

    // Move schema + array into caller-provided out params (transfers release
    // callbacks; the UniqueXxx destructors become no-ops).
    ArrowSchemaMove(schema.get(), out_schema);
    ArrowArrayMove(array.get(), out_array);
    write.last_error.clear();
    return true;
  }
};

// Minimal atomic shared_ptr cell exposing only the load()/store() surface the
// write hosts need. std::atomic<std::shared_ptr<T>> (C++20) is provided by
// libstdc++ only since GCC 12; the build baseline (CI) is GCC 11, where the
// primary std::atomic<T> template is selected instead and fails its
// "trivially copyable" static_assert. A mutex gives a happens-before guarantee
// at least as strong as the acquire/release ordering the callers request, so
// this is a faithful drop-in. Non-copyable / non-movable like std::atomic — the
// enclosing *State structs only ever move via the owning unique_ptr, never the
// State itself, so this matches their requirements exactly.
template <typename T>
class AtomicSharedPtr {
 public:
  AtomicSharedPtr() = default;
  explicit AtomicSharedPtr(std::shared_ptr<T> ptr) : ptr_(std::move(ptr)) {}

  std::shared_ptr<T> load(std::memory_order = std::memory_order_seq_cst) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ptr_;
  }
  void store(std::shared_ptr<T> ptr, std::memory_order = std::memory_order_seq_cst) {
    std::lock_guard<std::mutex> lock(mutex_);
    ptr_ = std::move(ptr);
  }

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<T> ptr_;
};

struct DatastoreSourceWriteHostState {
  DatastoreSourceWriteHostState(DataEngine& engine, DataSourceHandle source_handle)
      : core(std::make_shared<WriteCore>(engine)), source(source_handle), primary_engine(&engine) {}
  // AtomicSharedPtr, not a plain unique_ptr, because the streaming pause/resume
  // setTarget() swap reconstructs the WriteCore on the GUI thread while the ingest
  // worker may still be dereferencing the previous one on its own thread. The
  // worker loads a strong reference for the duration of each call (so the old core
  // cannot be destroyed under it), and setTarget builds the replacement fully then
  // publishes it with release ordering. Mirrors the object-store host's
  // std::atomic<ObjectStore*>; WriteCore holds DataEngine by reference and is not
  // reseatable, hence the swap rather than an in-place rebind.
  AtomicSharedPtr<WriteCore> core;
  DataSourceHandle source;
  // Streaming two-engine lockstep: remember the primary + secondary engines
  // so setTarget can re-wire the new WriteCore's secondary_engine_ pointer to
  // the OTHER engine, keeping mirrors symmetric. Both are raw pointers, not
  // owning; the streaming manager outlives the host.
  DataEngine* primary_engine = nullptr;
  DataEngine* secondary_engine = nullptr;
};

struct DatastoreParserWriteHostState {
  DatastoreParserWriteHostState(DataEngine& engine, TopicHandle topic_handle)
      : core(std::make_shared<WriteCore>(engine)), topic(topic_handle), primary_engine(&engine) {}
  // AtomicSharedPtr — see DatastoreSourceWriteHostState::core. The setTarget()
  // swap publishes a fresh WriteCore while the parser worker may still be inside
  // the previous one; the worker pins it with a strong reference per call, so the
  // swap can never free a core out from under an in-flight append.
  AtomicSharedPtr<WriteCore> core;
  TopicHandle topic;
  // Streaming two-engine lockstep — see DatastoreSourceWriteHostState. Closes
  // the latent FieldHandle-stale bug for parser plugins that cache handles
  // (parser_protobuf et al.) on pause/resume.
  DataEngine* primary_engine = nullptr;
  DataEngine* secondary_engine = nullptr;
};

struct DatastoreToolboxHostState {
  DatastoreToolboxHostState(DataEngine& engine, ObjectStore& store) : core(engine), object_store(store) {}
  ToolboxCore core;
  // Toolbox plugins share the session's object store; the host holds a
  // reference so register_object_topic + push_owned_object can forward
  // without going back through the engine.
  ObjectStore& object_store;
  std::string object_last_error;

  void setObjectError(std::string msg) {
    object_last_error = std::move(msg);
  }
};

struct DatastoreSourceObjectWriteHostState {
  DatastoreSourceObjectWriteHostState(ObjectStore& s, DatasetId dataset) : target(&s), dataset_id(dataset) {}
  // Atomic pointer rather than reference: the streaming two-store flow
  // retargets the host between the primary and secondary ObjectStore on each
  // pause/resume transition. Plain reference would not be reassignable. The
  // atomic guarantees the worker thread sees a fully-published swap from the
  // manager thread without locking on the hot push path.
  std::atomic<ObjectStore*> target;
  DatasetId dataset_id;
  std::string last_error;

  void setError(std::string msg) {
    last_error = std::move(msg);
  }
};

struct DatastoreToolboxObjectReadHostState {
  explicit DatastoreToolboxObjectReadHostState(ObjectStore& s) : store(s) {}
  ObjectStore& store;
  std::string last_error;
  // Backing storage for the const char* returned by toolboxObjectTopicMetadata:
  // ObjectStore::descriptor() now returns by value, so the pointer must outlive the
  // call. Valid until the next toolboxObjectTopicMetadata() on this host.
  std::string last_metadata;

  void setError(std::string msg) {
    last_error = std::move(msg);
  }
};

struct DatastoreParserObjectWriteHostState {
  DatastoreParserObjectWriteHostState(ObjectStore& s, ObjectTopicId topic) : target(&s), bound_topic(topic) {}
  // Atomic pointer rather than reference: see DatastoreSourceObjectWriteHostState.
  std::atomic<ObjectStore*> target;
  ObjectTopicId bound_topic;
  std::string last_error;

  void setError(std::string msg) {
    last_error = std::move(msg);
  }
};

void propagateError(PJ_error_t* out_error, const char* msg) {
  sdk::fillError(out_error, 1, "datastore", msg != nullptr ? std::string_view(msg) : std::string_view{});
}

template <typename Fn>
bool guardHostCallback(PJ_error_t* out_error, Fn&& fn) noexcept {
  try {
    return fn();
  } catch (const std::exception& e) {
    propagateError(out_error, e.what());
  } catch (...) {
    propagateError(out_error, "unknown datastore host exception");
  }
  return false;
}

bool sourceEnsureTopic(void* ctx, PJ_string_view_t topic_name, TopicHandle* out_topic, PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreSourceWriteHostState*>(ctx);
    // Pin the current WriteCore for the whole call so a concurrent setTarget()
    // swap can't destroy it under us (and so the op + its error read the SAME core).
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->ensureTopic(impl->source, toStringView(topic_name), out_topic)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    return true;
  });
}

bool sourceEnsureField(
    void* ctx, TopicHandle topic, PJ_string_view_t field_name, PJ_primitive_type_t type, FieldHandle* out_field,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreSourceWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->ensureField(topic, toStringView(field_name), type, out_field)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    return true;
  });
}

bool sourceAppendRecord(
    void* ctx, TopicHandle topic, int64_t timestamp, const PJ_named_field_value_t* fields, uint64_t field_count,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreSourceWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->appendRecord(topic, timestamp, fields, field_count)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    return true;
  });
}

bool sourceAppendBoundRecord(
    void* ctx, TopicHandle topic, int64_t timestamp, const PJ_bound_field_value_t* fields, uint64_t field_count,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreSourceWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->appendBoundRecord(topic, timestamp, fields, field_count)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    return true;
  });
}

bool sourceAppendArrowStream(
    void* ctx, TopicHandle topic, struct ArrowArrayStream* stream, PJ_string_view_t timestamp_column,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreSourceWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->appendArrowStream(topic, stream, timestamp_column)) {
      // Failure: plugin retains ownership of the stream; we do NOT release.
      propagateError(out_error, core->lastError());
      return false;
    }
    // Success: host now owns the stream — release it.
    if (stream != nullptr && stream->release != nullptr) {
      stream->release(stream);
    }
    return true;
  });
}

bool parserEnsureField(
    void* ctx, PJ_string_view_t field_name, PJ_primitive_type_t type, FieldHandle* out_field,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreParserWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->ensureField(impl->topic, toStringView(field_name), type, out_field)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    return true;
  });
}

bool parserAppendRecord(
    void* ctx, int64_t timestamp, const PJ_named_field_value_t* fields, uint64_t field_count,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreParserWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->appendRecord(impl->topic, timestamp, fields, field_count)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    return true;
  });
}

bool parserAppendBoundRecord(
    void* ctx, int64_t timestamp, const PJ_bound_field_value_t* fields, uint64_t field_count,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreParserWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->appendBoundRecord(impl->topic, timestamp, fields, field_count)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    return true;
  });
}

bool parserAppendArrowStream(
    void* ctx, struct ArrowArrayStream* stream, PJ_string_view_t timestamp_column, PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreParserWriteHostState*>(ctx);
    auto core = impl->core.load(std::memory_order_acquire);
    if (!core->appendArrowStream(impl->topic, stream, timestamp_column)) {
      propagateError(out_error, core->lastError());
      return false;
    }
    if (stream != nullptr && stream->release != nullptr) {
      stream->release(stream);
    }
    return true;
  });
}

bool toolboxCreateDataSource(
    void* ctx, PJ_string_view_t name, DataSourceHandle* out_source, PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.write.createDataSource(toStringView(name), out_source)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxEnsureTopic(
    void* ctx, DataSourceHandle source, PJ_string_view_t topic_name, TopicHandle* out_topic,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.write.ensureTopic(source, toStringView(topic_name), out_topic)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxEnsureField(
    void* ctx, TopicHandle topic, PJ_string_view_t field_name, PJ_primitive_type_t type, FieldHandle* out_field,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.write.ensureField(topic, toStringView(field_name), type, out_field)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxAppendRecord(
    void* ctx, TopicHandle topic, int64_t timestamp, const PJ_named_field_value_t* fields, uint64_t field_count,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.write.appendRecord(topic, timestamp, fields, field_count)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxAppendBoundRecord(
    void* ctx, TopicHandle topic, int64_t timestamp, const PJ_bound_field_value_t* fields, uint64_t field_count,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.write.appendBoundRecord(topic, timestamp, fields, field_count)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxAppendArrowStream(
    void* ctx, TopicHandle topic, struct ArrowArrayStream* stream, PJ_string_view_t timestamp_column,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.write.appendArrowStream(topic, stream, timestamp_column)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    if (stream != nullptr && stream->release != nullptr) {
      stream->release(stream);
    }
    return true;
  });
}

bool toolboxAcquireCatalogSnapshot(void* ctx, PJ_catalog_snapshot_t* out_snapshot, PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.acquireCatalogSnapshot(out_snapshot)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxReadSeriesArrow(
    void* ctx, FieldHandle field, struct ArrowSchema* out_schema, struct ArrowArray* out_array,
    PJ_error_t* out_error) noexcept {
  return guardHostCallback(out_error, [&] {
    auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
    if (!impl->core.readSeriesArrow(field, out_schema, out_array)) {
      propagateError(out_error, impl->core.write.lastError());
      return false;
    }
    return true;
  });
}

bool toolboxRegisterObjectTopic(
    void* ctx, DataSourceHandle source, PJ_string_view_t topic_name, PJ_string_view_t metadata_json,
    PJ_object_topic_handle_t* out_handle, PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
  if (out_handle == nullptr) {
    propagateError(out_error, "out_handle must not be null");
    return false;
  }
  // Validate the source handle against the engine — same check used by scalar
  // ensureTopic so the toolbox can't register a topic against a dataset that
  // doesn't exist. Scope the engine lock to JUST this read — do NOT hold it
  // across object_store.registerTopic below: the engine and ObjectStore locks
  // must never nest (see pj_datastore/CLAUDE.md).
  {
    auto lock = impl->core.engine.lockEngine();
    if (impl->core.engine.getDataset(source.id) == nullptr) {
      impl->setObjectError(fmt::format("data source {} not found", source.id));
      propagateError(out_error, impl->object_last_error.c_str());
      return false;
    }
  }
  try {
    ObjectTopicDescriptor desc{};
    desc.dataset_id = source.id;
    desc.topic_name = std::string(toStringView(topic_name));
    desc.metadata_json = std::string(toStringView(metadata_json));
    auto result = impl->object_store.registerTopic(desc);
    if (!result) {
      impl->setObjectError(result.error());
      propagateError(out_error, impl->object_last_error.c_str());
      return false;
    }
    out_handle->id = result->id;
    impl->object_last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setObjectError(e.what());
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  } catch (...) {
    impl->setObjectError("registerObjectTopic: unknown exception");
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  }
}

bool toolboxRegisterObjectTopicOnDataset(
    void* ctx, uint32_t dataset_id, PJ_string_view_t topic_name, PJ_string_view_t metadata_json,
    PJ_object_topic_handle_t* out_handle, PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
  if (out_handle == nullptr) {
    propagateError(out_error, "out_handle must not be null");
    return false;
  }
  if (impl->core.engine.getDataset(dataset_id) == nullptr) {
    impl->setObjectError(fmt::format("dataset {} not found", dataset_id));
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  }
  try {
    const std::string name(toStringView(topic_name));
    // Idempotent: a producer that republishes its whole marker set re-resolves
    // the same topic handle on every push instead of failing on "already exists".
    if (const auto existing = impl->object_store.findTopic(dataset_id, name)) {
      out_handle->id = existing->id;
      impl->object_last_error.clear();
      return true;
    }
    ObjectTopicDescriptor desc{};
    desc.dataset_id = dataset_id;
    desc.topic_name = name;
    desc.metadata_json = std::string(toStringView(metadata_json));
    auto result = impl->object_store.registerTopic(desc);
    if (!result) {
      impl->setObjectError(result.error());
      propagateError(out_error, impl->object_last_error.c_str());
      return false;
    }
    out_handle->id = result->id;
    impl->object_last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setObjectError(e.what());
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  } catch (...) {
    impl->setObjectError("registerObjectTopicOnDataset: unknown exception");
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  }
}

// Bound a topic's retention to its last `max_entries` snapshots (0 = unlimited).
// A producer that republishes a whole set at a sentinel timestamp sets 1 so
// superseded snapshots are evicted instead of accumulating across re-pushes.
bool toolboxSetObjectTopicRetention(
    void* ctx, PJ_object_topic_handle_t topic, uint64_t max_entries, PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
  try {
    RetentionBudget budget{};
    budget.max_entries = static_cast<size_t>(max_entries);
    impl->object_store.setRetentionBudget(ObjectTopicId{topic.id}, budget);
    impl->object_last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setObjectError(e.what());
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  } catch (...) {
    impl->setObjectError("setObjectTopicRetention: unknown exception");
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  }
}

bool toolboxPushOwnedObject(
    void* ctx, PJ_object_topic_handle_t topic, int64_t timestamp_ns, const uint8_t* data, uint64_t size,
    PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreToolboxHostState*>(ctx);
  try {
    std::vector<uint8_t> bytes;
    if (data != nullptr && size > 0) {
      bytes.assign(data, data + size);
    }
    auto result = impl->object_store.pushOwned(ObjectTopicId{topic.id}, timestamp_ns, std::move(bytes));
    if (!result) {
      impl->setObjectError(result.error());
      propagateError(out_error, impl->object_last_error.c_str());
      return false;
    }
    impl->object_last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setObjectError(e.what());
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  } catch (...) {
    impl->setObjectError("pushOwnedObject: unknown exception");
    propagateError(out_error, impl->object_last_error.c_str());
    return false;
  }
}

/// RAII holder for the plugin-owned `fetch_ctx` passed to push_lazy. Stores
/// the destroy callback pointer and the ctx value; destroys both on drop.
/// Wrapped in a shared_ptr so the lambda that ObjectStore stores remains
/// copyable (std::function requires copyable targets).
class PluginFetchCtx {
 public:
  PluginFetchCtx(PJ_lazy_fetch_fn_t fetch_fn, void* fetch_ctx, void (*destroy_fn)(void*)) noexcept
      : fetch_fn_(fetch_fn), ctx_(fetch_ctx), destroy_fn_(destroy_fn) {}

  ~PluginFetchCtx() {
    if (destroy_fn_ != nullptr) {
      destroy_fn_(ctx_);
    }
  }

  PluginFetchCtx(const PluginFetchCtx&) = delete;
  PluginFetchCtx& operator=(const PluginFetchCtx&) = delete;
  PluginFetchCtx(PluginFetchCtx&&) = delete;
  PluginFetchCtx& operator=(PluginFetchCtx&&) = delete;

  // nullopt = the plugin reported failure (or promised bytes it did not hand
  // over): the LazyCallback contract's fetch-failed signal. An engaged empty
  // vector is a payload the plugin legitimately produced with zero bytes —
  // ok with size 0 (data null or not; an empty container's data() may be null).
  [[nodiscard]] std::optional<std::vector<uint8_t>> invoke() const {
    if (fetch_fn_ == nullptr) {
      return std::nullopt;
    }
    const uint8_t* data = nullptr;
    uint64_t size = 0;  // matches PJ_lazy_fetch_fn_t out_size (uint64_t*)
    if (!fetch_fn_(ctx_, &data, &size)) {
      return std::nullopt;
    }
    if (data == nullptr && size > 0) {
      return std::nullopt;
    }
    if (size == 0) {
      return std::vector<uint8_t>{};
    }
    return std::vector<uint8_t>(data, data + size);
  }

 private:
  PJ_lazy_fetch_fn_t fetch_fn_;
  void* ctx_;
  void (*destroy_fn_)(void*);
};

bool sourceObjectRegisterTopic(
    void* ctx, PJ_string_view_t topic_name, PJ_string_view_t metadata_json, PJ_object_topic_handle_t* out_handle,
    PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreSourceObjectWriteHostState*>(ctx);
  if (out_handle == nullptr) {
    propagateError(out_error, "out_handle must not be null");
    return false;
  }
  auto* target = impl->target.load(std::memory_order_acquire);
  try {
    ObjectTopicDescriptor desc{};
    desc.dataset_id = impl->dataset_id;
    desc.topic_name = std::string(toStringView(topic_name));
    desc.metadata_json = std::string(toStringView(metadata_json));
    auto result = target->registerTopic(desc);
    if (!result) {
      impl->setError(result.error());
      propagateError(out_error, impl->last_error.c_str());
      return false;
    }
    out_handle->id = result->id;
    impl->last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setError(e.what());
    propagateError(out_error, impl->last_error.c_str());
    return false;
  } catch (...) {
    impl->setError("registerTopic: unknown exception");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
}

bool sourceObjectPushOwned(
    void* ctx, PJ_object_topic_handle_t topic, int64_t timestamp_ns, const uint8_t* data, uint64_t size,
    PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreSourceObjectWriteHostState*>(ctx);
  auto* target = impl->target.load(std::memory_order_acquire);
  try {
    std::vector<uint8_t> bytes;
    if (data != nullptr && size > 0) {
      bytes.assign(data, data + size);
    }
    auto result = target->pushOwned(ObjectTopicId{topic.id}, timestamp_ns, std::move(bytes));
    if (!result) {
      impl->setError(result.error());
      propagateError(out_error, impl->last_error.c_str());
      return false;
    }
    impl->last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setError(e.what());
    propagateError(out_error, impl->last_error.c_str());
    return false;
  } catch (...) {
    impl->setError("pushOwned: unknown exception");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
}

bool sourceObjectPushLazy(
    void* ctx, PJ_object_topic_handle_t topic, int64_t timestamp_ns, PJ_lazy_fetch_fn_t fetch_fn, void* fetch_ctx,
    void (*fetch_ctx_destroy)(void*), PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreSourceObjectWriteHostState*>(ctx);
  if (fetch_fn == nullptr) {
    if (fetch_ctx_destroy != nullptr) {
      fetch_ctx_destroy(fetch_ctx);
    }
    propagateError(out_error, "fetch_fn must not be null");
    return false;
  }
  auto* target = impl->target.load(std::memory_order_acquire);
  try {
    // shared_ptr keeps the ctx holder alive as long as ObjectStore keeps
    // the lambda; destructor runs exactly once when ObjectStore drops the
    // entry (retention, evict, removeTopic, clear, or store teardown).
    auto holder = std::make_shared<PluginFetchCtx>(fetch_fn, fetch_ctx, fetch_ctx_destroy);
    // Plugins return raw bytes via the C ABI; wrap them as a PayloadView whose
    // anchor is a shared_ptr<const vector<uint8_t>>, per the pushLazy contract.
    // Target pointer comes from the atomic swap layer (so writes follow the
    // current target store, not a captured-at-construction one).
    auto closure = [holder]() -> std::optional<sdk::PayloadView> {
      auto bytes = holder->invoke();
      if (!bytes.has_value()) {
        return std::nullopt;  // plugin fetch failed -> entry resolves as fetch_failed
      }
      return sdk::makePayloadView(std::move(*bytes));
    };
    auto result = target->pushLazy(ObjectTopicId{topic.id}, timestamp_ns, std::move(closure));
    if (!result) {
      impl->setError(result.error());
      propagateError(out_error, impl->last_error.c_str());
      // `holder` is the only reference to the ctx on failure; dropping it
      // runs fetch_ctx_destroy exactly once (the destructor already does it).
      return false;
    }
    impl->last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setError(e.what());
    propagateError(out_error, impl->last_error.c_str());
    // On exception before the ObjectStore took ownership, PluginFetchCtx's
    // destructor runs as part of shared_ptr teardown — single destroy call.
    return false;
  } catch (...) {
    impl->setError("pushLazy: unknown exception");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
}

void sourceObjectSetRetentionBudget(
    void* ctx, PJ_object_topic_handle_t topic, int64_t time_window_ns, uint64_t max_memory_bytes) noexcept {
  auto* impl = static_cast<DatastoreSourceObjectWriteHostState*>(ctx);
  auto* target = impl->target.load(std::memory_order_acquire);
  try {
    RetentionBudget budget{};
    budget.time_window_ns = time_window_ns;
    budget.max_memory_bytes = max_memory_bytes;
    target->setRetentionBudget(ObjectTopicId{topic.id}, budget);
  } catch (...) {
    // Infallible by contract — swallow any exception from the store.
  }
}

// ---------------------------------------------------------------------------
// Toolbox object read host trampolines
// ---------------------------------------------------------------------------

// PJ_object_bytes_handle_t is a heap-allocated sdk::PayloadView: its anchor
// keeps the buffer alive until the plugin calls release_bytes. No wrapper
// struct needed — PayloadView already carries the Span + anchor.
PJ_object_topic_handle_t toolboxObjectLookupTopic(void* ctx, PJ_string_view_t topic_name) noexcept {
  auto* impl = static_cast<DatastoreToolboxObjectReadHostState*>(ctx);
  try {
    const auto needle = toStringView(topic_name);
    for (const auto id : impl->store.listTopics()) {
      if (impl->store.descriptor(id).topic_name == needle) {
        return PJ_object_topic_handle_t{id.id};
      }
    }
  } catch (...) {
    // Fall through to invalid handle.
  }
  return PJ_object_topic_handle_t{0};
}

// Dataset-scoped lookup: object-topic identity is (dataset, name), so this
// resolves the topic owned by `dataset_id` rather than the first name match.
PJ_object_topic_handle_t toolboxObjectLookupTopicOnDataset(
    void* ctx, uint32_t dataset_id, PJ_string_view_t topic_name) noexcept {
  auto* impl = static_cast<DatastoreToolboxObjectReadHostState*>(ctx);
  try {
    if (const auto id = impl->store.findTopic(dataset_id, toStringView(topic_name))) {
      return PJ_object_topic_handle_t{id->id};
    }
  } catch (...) {
    // Fall through to invalid handle.
  }
  return PJ_object_topic_handle_t{0};
}

bool toolboxObjectListTopics(
    void* ctx, PJ_object_topic_handle_t* out_buffer, uint64_t buffer_capacity, uint64_t* out_count,
    PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreToolboxObjectReadHostState*>(ctx);
  if (out_count == nullptr) {
    propagateError(out_error, "out_count must not be null");
    return false;
  }
  try {
    const auto ids = impl->store.listTopics();
    *out_count = ids.size();
    if (out_buffer != nullptr) {
      // buffer_capacity is uint64_t (ABI); ids.size() is size_t. Compare in
      // uint64_t, then index with size_t (n <= ids.size(), so it fits).
      const std::size_t n = static_cast<std::size_t>(std::min<uint64_t>(buffer_capacity, ids.size()));
      for (std::size_t i = 0; i < n; ++i) {
        out_buffer[i] = PJ_object_topic_handle_t{ids[i].id};
      }
    }
    return true;
  } catch (const std::exception& e) {
    impl->setError(e.what());
    propagateError(out_error, impl->last_error.c_str());
    return false;
  } catch (...) {
    impl->setError("listTopics: unknown exception");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
}

const char* toolboxObjectTopicMetadata(void* ctx, PJ_object_topic_handle_t topic) noexcept {
  auto* impl = static_cast<DatastoreToolboxObjectReadHostState*>(ctx);
  try {
    // descriptor() returns a copy; stash it on the host so the returned pointer
    // outlives this call (stable until the next call on this host) rather than
    // dangling into a destroyed temporary.
    impl->last_metadata = impl->store.descriptor(ObjectTopicId{topic.id}).metadata_json;
    return impl->last_metadata.c_str();
  } catch (...) {
    return nullptr;
  }
}

bool toolboxObjectReadLatestAt(
    void* ctx, PJ_object_topic_handle_t topic, int64_t timestamp_ns, PJ_object_bytes_handle_t* out_handle,
    int64_t* out_timestamp, PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreToolboxObjectReadHostState*>(ctx);
  if (out_handle == nullptr) {
    propagateError(out_error, "out_handle must not be null");
    return false;
  }
  *out_handle = nullptr;
  try {
    auto entry = impl->store.latestAt(ObjectTopicId{topic.id}, timestamp_ns);
    if (!entry.has_value() || entry->payload.anchor == nullptr) {
      impl->setError("no entry at-or-before timestamp");
      propagateError(out_error, impl->last_error.c_str());
      return false;
    }
    auto* payload_handle = new sdk::PayloadView(std::move(entry->payload));
    *out_handle = reinterpret_cast<PJ_object_bytes_handle_t>(payload_handle);
    if (out_timestamp != nullptr) {
      *out_timestamp = entry->timestamp;
    }
    impl->last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setError(e.what());
    propagateError(out_error, impl->last_error.c_str());
    return false;
  } catch (...) {
    impl->setError("readLatestAt: unknown exception");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
}

void toolboxObjectGetBytes(PJ_object_bytes_handle_t handle, const uint8_t** out_data, uint64_t* out_size) noexcept {
  if (out_data != nullptr) {
    *out_data = nullptr;
  }
  if (out_size != nullptr) {
    *out_size = 0;
  }
  if (handle == nullptr) {
    return;
  }
  const auto* payload = reinterpret_cast<const sdk::PayloadView*>(handle);
  if (payload->anchor == nullptr) {
    return;
  }
  if (out_data != nullptr) {
    *out_data = payload->bytes.data();
  }
  if (out_size != nullptr) {
    *out_size = payload->bytes.size();
  }
}

void toolboxObjectReleaseBytes(PJ_object_bytes_handle_t handle) noexcept {
  if (handle == nullptr) {
    return;
  }
  delete reinterpret_cast<sdk::PayloadView*>(handle);
}

uint64_t toolboxObjectEntryCount(void* ctx, PJ_object_topic_handle_t topic) noexcept {
  auto* impl = static_cast<DatastoreToolboxObjectReadHostState*>(ctx);
  try {
    return impl->store.entryCount(ObjectTopicId{topic.id});
  } catch (...) {
    return 0;
  }
}

bool toolboxObjectTimeRange(
    void* ctx, PJ_object_topic_handle_t topic, int64_t* out_min_ts, int64_t* out_max_ts) noexcept {
  auto* impl = static_cast<DatastoreToolboxObjectReadHostState*>(ctx);
  try {
    if (impl->store.entryCount(ObjectTopicId{topic.id}) == 0) {
      return false;
    }
    const auto range = impl->store.timeRange(ObjectTopicId{topic.id});
    if (out_min_ts != nullptr) {
      *out_min_ts = range.first;
    }
    if (out_max_ts != nullptr) {
      *out_max_ts = range.second;
    }
    return true;
  } catch (...) {
    return false;
  }
}

// ---------------------------------------------------------------------------
// Parser object write host trampolines — topic bound at service-create time.
// ---------------------------------------------------------------------------

bool parserObjectPushOwned(
    void* ctx, int64_t timestamp_ns, const uint8_t* data, uint64_t size, PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreParserObjectWriteHostState*>(ctx);
  auto* target = impl->target.load(std::memory_order_acquire);
  try {
    std::vector<uint8_t> bytes;
    if (data != nullptr && size > 0) {
      bytes.assign(data, data + size);
    }
    auto result = target->pushOwned(impl->bound_topic, timestamp_ns, std::move(bytes));
    if (!result) {
      impl->setError(result.error());
      propagateError(out_error, impl->last_error.c_str());
      return false;
    }
    impl->last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setError(e.what());
    propagateError(out_error, impl->last_error.c_str());
    return false;
  } catch (...) {
    impl->setError("parser pushOwned: unknown exception");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
}

bool parserObjectPushLazy(
    void* ctx, int64_t timestamp_ns, PJ_lazy_fetch_fn_t fetch_fn, void* fetch_ctx, void (*fetch_ctx_destroy)(void*),
    PJ_error_t* out_error) noexcept {
  auto* impl = static_cast<DatastoreParserObjectWriteHostState*>(ctx);
  if (fetch_fn == nullptr) {
    if (fetch_ctx_destroy != nullptr) {
      fetch_ctx_destroy(fetch_ctx);
    }
    propagateError(out_error, "fetch_fn must not be null");
    return false;
  }
  auto* target = impl->target.load(std::memory_order_acquire);
  try {
    auto holder = std::make_shared<PluginFetchCtx>(fetch_fn, fetch_ctx, fetch_ctx_destroy);
    auto closure = [holder]() -> std::optional<sdk::PayloadView> {
      auto bytes = holder->invoke();
      if (!bytes.has_value()) {
        return std::nullopt;  // plugin fetch failed -> entry resolves as fetch_failed
      }
      return sdk::makePayloadView(std::move(*bytes));
    };
    auto result = target->pushLazy(impl->bound_topic, timestamp_ns, std::move(closure));
    if (!result) {
      impl->setError(result.error());
      propagateError(out_error, impl->last_error.c_str());
      return false;
    }
    impl->last_error.clear();
    return true;
  } catch (const std::exception& e) {
    impl->setError(e.what());
    propagateError(out_error, impl->last_error.c_str());
    return false;
  } catch (...) {
    impl->setError("parser pushLazy: unknown exception");
    propagateError(out_error, impl->last_error.c_str());
    return false;
  }
}

const PJ_source_write_host_vtable_t kSourceWriteVTable = {
    PJ_PLUGIN_DATA_API_VERSION, sizeof(PJ_source_write_host_vtable_t),
    sourceEnsureTopic,          sourceEnsureField,
    sourceAppendRecord,         sourceAppendBoundRecord,
    sourceAppendArrowStream,
};

const PJ_parser_write_host_vtable_t kParserWriteVTable = {
    PJ_PLUGIN_DATA_API_VERSION, sizeof(PJ_parser_write_host_vtable_t),
    parserEnsureField,          parserAppendRecord,
    parserAppendBoundRecord,    parserAppendArrowStream,
};

const PJ_toolbox_host_vtable_t kToolboxVTable = {
    PJ_PLUGIN_DATA_API_VERSION,
    sizeof(PJ_toolbox_host_vtable_t),
    toolboxCreateDataSource,
    toolboxEnsureTopic,
    toolboxEnsureField,
    toolboxAppendRecord,
    toolboxAppendBoundRecord,
    toolboxAppendArrowStream,
    toolboxAcquireCatalogSnapshot,
    toolboxReadSeriesArrow,
    toolboxRegisterObjectTopic,
    toolboxPushOwnedObject,
    toolboxRegisterObjectTopicOnDataset,
    toolboxSetObjectTopicRetention,
};

const PJ_object_write_host_vtable_t kSourceObjectWriteVTable = {
    PJ_PLUGIN_DATA_API_VERSION, sizeof(PJ_object_write_host_vtable_t), sourceObjectRegisterTopic, sourceObjectPushOwned,
    sourceObjectPushLazy,       sourceObjectSetRetentionBudget,
};

const PJ_object_read_host_vtable_t kToolboxObjectReadVTable = {
    PJ_PLUGIN_DATA_API_VERSION,
    sizeof(PJ_object_read_host_vtable_t),
    toolboxObjectLookupTopic,
    toolboxObjectListTopics,
    toolboxObjectTopicMetadata,
    toolboxObjectReadLatestAt,
    toolboxObjectGetBytes,
    toolboxObjectReleaseBytes,
    toolboxObjectEntryCount,
    toolboxObjectTimeRange,
    toolboxObjectLookupTopicOnDataset,
};

const PJ_parser_object_write_host_vtable_t kParserObjectWriteVTable = {
    PJ_PLUGIN_DATA_API_VERSION,
    sizeof(PJ_parser_object_write_host_vtable_t),
    parserObjectPushOwned,
    parserObjectPushLazy,
};

DatastoreSourceWriteHost::DatastoreSourceWriteHost(DataEngine& engine, DataSourceHandle source)
    : state_(std::make_unique<DatastoreSourceWriteHostState>(engine, source)) {}
DatastoreSourceWriteHost::~DatastoreSourceWriteHost() = default;
DatastoreSourceWriteHost::DatastoreSourceWriteHost(DatastoreSourceWriteHost&&) noexcept = default;
DatastoreSourceWriteHost& DatastoreSourceWriteHost::operator=(DatastoreSourceWriteHost&&) noexcept = default;

PJ_source_write_host_t DatastoreSourceWriteHost::raw() noexcept {
  return PJ_source_write_host_t{.ctx = state_.get(), .vtable = &kSourceWriteVTable};
}

void DatastoreSourceWriteHost::flushPending() {
  state_->core.load(std::memory_order_acquire)->flushPending();
}

void DatastoreSourceWriteHost::setTarget(DataEngine* target) {
  // Seal + commit any open chunk to the current engine so no rows are lost,
  // then rebind to the new engine with a fresh WriteCore (its writer and
  // per-engine caches must not carry over). Re-wire the new WriteCore's
  // secondary_engine_ pointer to whichever engine is NOT the target so any
  // new topic/field created while pointed at `target` mirrors back to the
  // other side — both engines keep their TopicId/FieldId assignments in
  // lockstep regardless of which one is currently active.
  state_->core.load(std::memory_order_acquire)->flushPending();
  // Build + fully wire the replacement BEFORE publishing it, so the ingest worker
  // only ever observes a complete WriteCore. The atomic store (release) hands it
  // off; a worker still inside the old core keeps it alive via its own strong
  // reference until that call returns (in-flight appends finish on the old target,
  // matching the object-store path's accepted semantics).
  auto next = std::make_shared<WriteCore>(*target);
  if (target == state_->primary_engine) {
    next->secondary_engine = state_->secondary_engine;
  } else if (target == state_->secondary_engine) {
    next->secondary_engine = state_->primary_engine;
  } else {
    // Unknown target (e.g. an in-place dataset replace) — no mirror.
    next->secondary_engine = nullptr;
    state_->primary_engine = target;
  }
  state_->core.store(std::move(next), std::memory_order_release);
}

void DatastoreSourceWriteHost::setSecondaryEngine(DataEngine* secondary) {
  // Wire the secondary engine for streaming pause/resume lockstep. Mirroring
  // is bidirectional: the active WriteCore mirrors to whichever engine is
  // the other one. Called once by the streaming runtime host right after
  // construction; passing nullptr disables mirroring.
  state_->secondary_engine = secondary;
  // Called once right after construction, before any ingest worker runs, so
  // mutating the live core's mirror target in place is safe (no concurrent reader
  // yet); load() merely unwraps the atomic.
  if (auto core = state_->core.load(std::memory_order_acquire)) {
    // Whichever direction we're currently pointed at, mirror to the other.
    core->secondary_engine = (&core->engine == state_->primary_engine) ? secondary : state_->primary_engine;
  }
}

DatastoreParserWriteHost::DatastoreParserWriteHost(DataEngine& engine, TopicHandle topic)
    : state_(std::make_unique<DatastoreParserWriteHostState>(engine, topic)) {}
DatastoreParserWriteHost::~DatastoreParserWriteHost() = default;
DatastoreParserWriteHost::DatastoreParserWriteHost(DatastoreParserWriteHost&&) noexcept = default;
DatastoreParserWriteHost& DatastoreParserWriteHost::operator=(DatastoreParserWriteHost&&) noexcept = default;

PJ_parser_write_host_t DatastoreParserWriteHost::raw() noexcept {
  return PJ_parser_write_host_t{.ctx = state_.get(), .vtable = &kParserWriteVTable};
}

void DatastoreParserWriteHost::flushPending() {
  state_->core.load(std::memory_order_acquire)->flushPending();
}

void DatastoreParserWriteHost::setTarget(DataEngine* target) {
  // Seal + commit any open chunk to the current engine so no rows are lost,
  // then rebind to the new engine with a fresh WriteCore. The bound topic is
  // expected to already exist in `target` with the same TopicId. Re-wires
  // the new WriteCore's secondary_engine_ pointer to the OTHER engine for
  // bidirectional lockstep mirroring (parsers that cache FieldHandle — e.g.
  // parser_protobuf — would otherwise see stale ids after pause/resume).
  state_->core.load(std::memory_order_acquire)->flushPending();
  // Build + fully wire the replacement before publishing it atomically — see
  // DatastoreSourceWriteHost::setTarget for the swap-safety rationale.
  auto next = std::make_shared<WriteCore>(*target);
  if (target == state_->primary_engine) {
    next->secondary_engine = state_->secondary_engine;
  } else if (target == state_->secondary_engine) {
    next->secondary_engine = state_->primary_engine;
  } else {
    next->secondary_engine = nullptr;
    state_->primary_engine = target;
  }
  state_->core.store(std::move(next), std::memory_order_release);
}

void DatastoreParserWriteHost::setSecondaryEngine(DataEngine* secondary) {
  // See DatastoreSourceWriteHost::setSecondaryEngine — same semantics.
  state_->secondary_engine = secondary;
  // Construction-time only (no concurrent reader yet) — see the source host.
  if (auto core = state_->core.load(std::memory_order_acquire)) {
    core->secondary_engine = (&core->engine == state_->primary_engine) ? secondary : state_->primary_engine;
  }
}

DatastoreToolboxHost::DatastoreToolboxHost(DataEngine& engine, ObjectStore& object_store)
    : state_(std::make_unique<DatastoreToolboxHostState>(engine, object_store)) {}
DatastoreToolboxHost::~DatastoreToolboxHost() = default;
DatastoreToolboxHost::DatastoreToolboxHost(DatastoreToolboxHost&&) noexcept = default;
DatastoreToolboxHost& DatastoreToolboxHost::operator=(DatastoreToolboxHost&&) noexcept = default;

PJ_toolbox_host_t DatastoreToolboxHost::raw() noexcept {
  return PJ_toolbox_host_t{.ctx = state_.get(), .vtable = &kToolboxVTable};
}

void DatastoreToolboxHost::flushPending() {
  state_->core.write.flushPending();
}

DatastoreSourceObjectWriteHost::DatastoreSourceObjectWriteHost(ObjectStore& store, DatasetId dataset_id)
    : state_(std::make_unique<DatastoreSourceObjectWriteHostState>(store, dataset_id)) {}
DatastoreSourceObjectWriteHost::~DatastoreSourceObjectWriteHost() = default;
DatastoreSourceObjectWriteHost::DatastoreSourceObjectWriteHost(DatastoreSourceObjectWriteHost&&) noexcept = default;
DatastoreSourceObjectWriteHost& DatastoreSourceObjectWriteHost::operator=(DatastoreSourceObjectWriteHost&&) noexcept =
    default;

PJ_object_write_host_t DatastoreSourceObjectWriteHost::raw() noexcept {
  return PJ_object_write_host_t{.ctx = state_.get(), .vtable = &kSourceObjectWriteVTable};
}

void DatastoreSourceObjectWriteHost::setTarget(ObjectStore* target) noexcept {
  state_->target.store(target, std::memory_order_release);
}

DatastoreToolboxObjectReadHost::DatastoreToolboxObjectReadHost(ObjectStore& store)
    : state_(std::make_unique<DatastoreToolboxObjectReadHostState>(store)) {}
DatastoreToolboxObjectReadHost::~DatastoreToolboxObjectReadHost() = default;
DatastoreToolboxObjectReadHost::DatastoreToolboxObjectReadHost(DatastoreToolboxObjectReadHost&&) noexcept = default;
DatastoreToolboxObjectReadHost& DatastoreToolboxObjectReadHost::operator=(DatastoreToolboxObjectReadHost&&) noexcept =
    default;

PJ_object_read_host_t DatastoreToolboxObjectReadHost::raw() noexcept {
  return PJ_object_read_host_t{.ctx = state_.get(), .vtable = &kToolboxObjectReadVTable};
}

DatastoreParserObjectWriteHost::DatastoreParserObjectWriteHost(ObjectStore& store, uint32_t topic_id)
    : state_(std::make_unique<DatastoreParserObjectWriteHostState>(store, ObjectTopicId{topic_id})) {}
DatastoreParserObjectWriteHost::~DatastoreParserObjectWriteHost() = default;
DatastoreParserObjectWriteHost::DatastoreParserObjectWriteHost(DatastoreParserObjectWriteHost&&) noexcept = default;
DatastoreParserObjectWriteHost& DatastoreParserObjectWriteHost::operator=(DatastoreParserObjectWriteHost&&) noexcept =
    default;

PJ_parser_object_write_host_t DatastoreParserObjectWriteHost::raw() noexcept {
  return PJ_parser_object_write_host_t{.ctx = state_.get(), .vtable = &kParserObjectWriteVTable};
}

void DatastoreParserObjectWriteHost::setTarget(ObjectStore* target) noexcept {
  state_->target.store(target, std::memory_order_release);
}

}  // namespace PJ
