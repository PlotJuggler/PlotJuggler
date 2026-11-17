// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/incremental_engine.hpp"

#include <fmt/format.h>
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "pj_base/type_tree.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// The PrimitiveType of flat leaf column `target`. Fixed arrays expand element-wise and variable-length ones add no
// column, as in countLeafFields(), so `target` is the engine's column index. `seen` counts the leaves visited (start
// at 0).
static std::optional<PJ::PrimitiveType> nthLeafPrimitive(
    const PJ::TypeTreeNode& node, std::size_t target, std::size_t& seen) {
  switch (node.kind) {
    case PJ::TypeKind::kPrimitive:
    case PJ::TypeKind::kEnum:
      if (seen == target) {
        return node.primitive_type;
      }
      ++seen;
      return std::nullopt;
    case PJ::TypeKind::kStruct:
      for (const auto& child : node.children) {
        if (auto r = nthLeafPrimitive(*child, target, seen)) {
          return r;
        }
      }
      return std::nullopt;
    case PJ::TypeKind::kArray:
      if (node.element_type && node.fixed_array_size.has_value()) {
        for (uint32_t i = 0; i < *node.fixed_array_size; ++i) {
          if (auto r = nthLeafPrimitive(*node.element_type, target, seen)) {
            return r;
          }
        }
      }
      return std::nullopt;  // dynamic array contributes no columns
  }
  return std::nullopt;
}

// One leaf column's layout on an input topic: from the schema, else the stored column layout (a schemaless topic with
// no committed chunk yet), else the first sealed chunk. column_count == 0: no layout is derivable; leaf_primitive is
// set only when `column` is in range and resolvable.
struct LeafColumnLayout {
  std::size_t column_count = 0;
  std::optional<PJ::PrimitiveType> leaf_primitive;
};

static LeafColumnLayout resolveLeafColumnLayout(
    const DataEngine& engine, const TopicStorage& storage, std::size_t column) {
  LeafColumnLayout layout;
  if (storage.descriptor().schema_id != 0) {
    if (const PJ::TypeTreeNode* root = engine.typeRegistry().lookup(storage.descriptor().schema_id)) {
      layout.column_count = PJ::countLeafFields(*root);
      if (column < layout.column_count) {
        std::size_t seen = 0;
        layout.leaf_primitive = nthLeafPrimitive(*root, column, seen);
      }
    }
  }
  if (layout.column_count == 0 && !storage.columnDescriptors().empty()) {
    layout.column_count = storage.columnDescriptors().size();
    if (column < layout.column_count) {
      layout.leaf_primitive = storage.columnDescriptors()[column].logical_type;
    }
  }
  if (layout.column_count == 0 && !storage.sealedChunks().empty() && !storage.sealedChunks().front().columns.empty()) {
    layout.column_count = storage.sealedChunks().front().columns.size();
    if (column < layout.column_count && storage.sealedChunks().front().columns[column].descriptor != nullptr) {
      layout.leaf_primitive = storage.sealedChunks().front().columns[column].descriptor->logical_type;
    }
  }
  return layout;
}

static PJ::PrimitiveType storageKindToPrimitive(StorageKind k) {
  switch (k) {
    case StorageKind::kFloat32:
      return PJ::PrimitiveType::kFloat32;
    case StorageKind::kFloat64:
      return PJ::PrimitiveType::kFloat64;
    case StorageKind::kInt32:
      return PJ::PrimitiveType::kInt32;
    case StorageKind::kInt64:
      return PJ::PrimitiveType::kInt64;
    case StorageKind::kUint64:
      return PJ::PrimitiveType::kUint64;
    case StorageKind::kBool:
      return PJ::PrimitiveType::kBool;
    case StorageKind::kString:
      return PJ::PrimitiveType::kString;
  }
  return PJ::PrimitiveType::kFloat64;
}

static VarValue decodeAsVarvalue(const TopicChunk& chunk, std::size_t col, std::size_t row, StorageKind kind) {
  switch (kind) {
    case StorageKind::kFloat32:
    case StorageKind::kFloat64:
      return chunk.readNumericAsDouble(col, row);
    case StorageKind::kInt32:
    case StorageKind::kInt64:
      return chunk.readNumericAsInt64(col, row);
    case StorageKind::kUint64:
      return chunk.readNumericAsUint64(col, row);
    case StorageKind::kBool:
      return static_cast<int64_t>(chunk.readBool(col, row) ? 1 : 0);
    case StorageKind::kString:
      return std::string(chunk.readString(col, row));
  }
  return 0.0;
}

// Coerces `val` to out_kind: a non-string into a string column writes nothing, a string into a numeric one writes 0.
static void writeVarvalue(
    DataWriter& writer, PJ::TopicId tid, std::size_t col, const VarValue& val, StorageKind out_kind) {
  if (out_kind == StorageKind::kString) {
    if (const auto* s = std::get_if<std::string>(&val)) {
      writer.set(tid, col, std::string_view(*s));
    }
    return;
  }

  // Integer→integer fast paths: avoid the lossy double round-trip.
  if (out_kind == StorageKind::kUint64) {
    if (const auto* u = std::get_if<uint64_t>(&val)) {
      writer.set(tid, col, *u);
      return;
    }
    if (const auto* i = std::get_if<int64_t>(&val)) {
      writer.set(tid, col, static_cast<uint64_t>(*i));
      return;
    }
  }
  if (out_kind == StorageKind::kInt64) {
    if (const auto* i = std::get_if<int64_t>(&val)) {
      writer.set(tid, col, *i);
      return;
    }
    if (const auto* u = std::get_if<uint64_t>(&val)) {
      writer.set(tid, col, static_cast<int64_t>(*u));
      return;
    }
  }
  if (out_kind == StorageKind::kInt32) {
    if (const auto* i = std::get_if<int64_t>(&val)) {
      writer.set(tid, col, static_cast<int32_t>(*i));
      return;
    }
    if (const auto* u = std::get_if<uint64_t>(&val)) {
      writer.set(tid, col, static_cast<int32_t>(*u));
      return;
    }
  }

  double dval = std::visit(
      [](const auto& v) -> double {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return 0.0;
        } else {
          return static_cast<double>(v);
        }
      },
      val);

  switch (out_kind) {
    case StorageKind::kFloat32:
      writer.set(tid, col, static_cast<float>(dval));
      break;
    case StorageKind::kFloat64:
      writer.set(tid, col, dval);
      break;
    case StorageKind::kInt32:
      writer.set(tid, col, static_cast<int32_t>(dval));
      break;
    case StorageKind::kInt64:
      writer.set(tid, col, static_cast<int64_t>(dval));
      break;
    case StorageKind::kUint64:
      writer.set(tid, col, static_cast<uint64_t>(dval));
      break;
    case StorageKind::kBool:
      writer.set(tid, col, dval != 0.0);
      break;
    case StorageKind::kString:
      break;  // handled above
  }
}

// ---------------------------------------------------------------------------
// Internal data structures
// ---------------------------------------------------------------------------

struct DerivedNode {
  PJ::NodeId id = PJ::kInvalidNodeId;
  bool is_mimo = false;

  // SISO fields
  PJ::TopicId siso_input_topic_id = 0;
  std::size_t siso_input_column_index = 0;  // which leaf column of the input topic feeds the op
  StorageKind siso_input_kind = StorageKind::kFloat64;
  StorageKind siso_output_kind = StorageKind::kFloat64;
  std::unique_ptr<ISISOTransform> siso_op;

  // MIMO fields
  std::vector<PJ::TopicId> mimo_input_topic_ids;
  std::vector<std::size_t> mimo_input_columns;  // leaf column per input (parallel to mimo_input_topic_ids)
  std::vector<StorageKind> mimo_input_kinds;
  std::vector<StorageKind> mimo_output_kinds;
  std::unique_ptr<IMIMOTransform> mimo_op;
  // Grouped: the M calculate() results are M named columns of one topic (output_topic_ids[0]); otherwise each is its
  // own scalar topic.
  bool mimo_grouped = false;

  // Common
  std::vector<PJ::TopicId> all_input_topic_ids;  // unified input list for all types
  std::vector<PJ::TopicId> output_topic_ids;     // 1 for SISO, M for MIMO
  bool dirty = true;
  // Set while the watermarks below do not describe the output: a failed recompute rolled the output back, or
  // restoreInputBindingState() rewound them. The incremental path appends and would duplicate history, so a flagged
  // node takes the full detach-replay recompute. Cleared only by a successful recompute.
  bool needs_full_recompute = false;
  PJ::ChunkId last_processed_chunk_id = 0;                                 // SISO: chunk watermark
  PJ::Timestamp mimo_last_ts = std::numeric_limits<PJ::Timestamp>::min();  // MIMO: timestamp watermark
  // A not-yet-processed input chunk at or before these watermarks forces a reset and full replay: transforms need
  // ascending timestamps.
  PJ::Timestamp siso_last_ts = std::numeric_limits<PJ::Timestamp>::min();  // SISO: last input ts fed
  PJ::ChunkId mimo_last_chunk_id = 0;                                      // MIMO: last input chunk considered
  // MIMO: newest chunk id seen on each input (parallel to mimo_input_topic_ids; empty = none yet). mimo_last_chunk_id
  // is their maximum, so a chunk numbered below it can still be new to its input.
  std::vector<PJ::ChunkId> mimo_input_chunk_ids;
  // SISO: a chunk a time-sliced step stopped inside (0 = none), and its first unfed row; the per-chunk path resumes
  // there. A node with one is deferred: materializeStep()'s until its replay catches up, and the scheduling passes
  // skip it.
  PJ::ChunkId resume_chunk_id = 0;
  uint32_t resume_row = 0;
  // Error of this node's own last run (empty after a successful one); see IncrementalEngine::nodeError().
  std::string error;
  // Retention gaps recorded by runNodeLocked(), bounded by recordGap().
  std::vector<IncrementalEngine::Gap> gaps;

  // Reused across rows, so decoding allocates nothing per row.
  VarValue in_val_buf = 0.0;           // SISO input
  VarValue out_val_buf = 0.0;          // SISO output
  std::vector<VarValue> mimo_in_buf;   // MIMO inputs
  std::vector<VarValue> mimo_out_buf;  // MIMO outputs
};

static void recordGap(DerivedNode& node, const IncrementalEngine::Gap& gap) {
  constexpr std::size_t kMaxGapsPerNode = 16;
  node.gaps.push_back(gap);
  if (node.gaps.size() > kMaxGapsPerNode) {
    node.gaps.erase(node.gaps.begin());
  }
}

struct DatasetNameHash {
  std::size_t operator()(const std::pair<PJ::DatasetId, std::string>& key) const noexcept {
    std::size_t h1 = std::hash<PJ::DatasetId>{}(key.first);
    std::size_t h2 = std::hash<std::string>{}(key.second);
    return h1 ^ (h2 << 1);
  }
};

struct IncrementalEngineImpl {
  tsl::robin_map<PJ::NodeId, DerivedNode> nodes;

  // downstream_of[N] = list of nodes whose inputs include an output of N
  tsl::robin_map<PJ::NodeId, std::vector<PJ::NodeId>> downstream_of;

  // topic_to_nodes[T] = list of nodes that use T as an input
  tsl::robin_map<PJ::TopicId, std::vector<PJ::NodeId>> topic_to_nodes;

  // output_topic_to_node[T] = node that produces T
  tsl::robin_map<PJ::TopicId, PJ::NodeId> output_topic_to_node;

  // Name uniqueness within dataset: (dataset_id, topic_name) → topic_id
  tsl::robin_map<std::pair<PJ::DatasetId, std::string>, PJ::TopicId, DatasetNameHash> registered_output_names;
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

IncrementalEngine::IncrementalEngine(DataEngine& engine)
    : engine_(engine), impl_(std::make_unique<IncrementalEngineImpl>()) {}

IncrementalEngine::~IncrementalEngine() = default;

// ---------------------------------------------------------------------------
// Cycle detection (DFS)
// ---------------------------------------------------------------------------
// The error if a node `input_topics → output_topics` would close a cycle, else empty.
static std::string checkCycle(
    const IncrementalEngineImpl& impl, const std::vector<PJ::TopicId>& input_topics,
    const std::vector<PJ::TopicId>& output_topics) {
  tsl::robin_set<PJ::TopicId> outputs(output_topics.begin(), output_topics.end());

  // DFS upstream from each input (topic → producing node → its inputs); reaching a topic in `outputs` is a cycle.
  std::vector<PJ::TopicId> stack(input_topics.begin(), input_topics.end());
  tsl::robin_set<PJ::TopicId> visited;

  while (!stack.empty()) {
    PJ::TopicId t = stack.back();
    stack.pop_back();
    if (!visited.insert(t).second) {
      continue;
    }

    if (outputs.contains(t)) {
      return fmt::format("cycle detected: topic {} is both an input and an output", t);
    }

    auto it = impl.output_topic_to_node.find(t);
    if (it == impl.output_topic_to_node.end()) {
      continue;  // source topic, no upstream
    }

    PJ::NodeId producer = it->second;
    auto nit = impl.nodes.find(producer);
    if (nit == impl.nodes.end()) {
      continue;
    }

    for (PJ::TopicId in : nit->second.all_input_topic_ids) {
      if (!visited.contains(in)) {
        stack.push_back(in);
      }
    }
  }
  return "";  // no cycle
}

// ---------------------------------------------------------------------------
// Node creation
// ---------------------------------------------------------------------------

PJ::Expected<PJ::NodeId> IncrementalEngine::addSisoTransform(
    PJ::TopicId input_topic_id, std::string output_topic_name, PJ::DatasetId output_dataset_id,
    std::unique_ptr<ISISOTransform> op, std::size_t input_column_index) {
  // One engine lock across the input read, the type registration and createTopic(), against worker ingest;
  // createTopic() re-acquires it (recursive).
  auto lock = engine_.lockEngine();
  const TopicStorage* in_storage = engine_.getTopicStorage(input_topic_id);
  if (!in_storage) {
    return PJ::unexpected(fmt::format("add_siso_transform: input topic {} not found", input_topic_id));
  }

  const LeafColumnLayout layout = resolveLeafColumnLayout(engine_, *in_storage, input_column_index);
  if (layout.column_count == 0) {
    return PJ::unexpected(
        fmt::format(
            "add_siso_transform: cannot determine column layout for topic {}"
            " (no schema_id, no stored column layout, and no committed chunks)",
            input_topic_id));
  }
  if (input_column_index >= layout.column_count) {
    return PJ::unexpected(
        fmt::format(
            "add_siso_transform: input column {} out of range (topic {} has {} columns)", input_column_index,
            input_topic_id, layout.column_count));
  }
  if (!layout.leaf_primitive) {
    return PJ::unexpected("add_siso_transform: could not determine leaf primitive type for the selected column");
  }
  StorageKind in_kind = storageKindOf(*layout.leaf_primitive);

  StorageKind out_kind = op->outputKind(in_kind);

  auto name_key = std::make_pair(output_dataset_id, output_topic_name);
  if (impl_->registered_output_names.contains(name_key)) {
    return PJ::unexpected(
        fmt::format(
            "add_siso_transform: output topic '{}' already registered in dataset {}", output_topic_name,
            output_dataset_id));
  }

  std::string cycle_err = checkCycle(*impl_, {input_topic_id}, {});  // output topic doesn't exist yet
  if (!cycle_err.empty()) {
    return PJ::unexpected(cycle_err);
  }

  PJ::PrimitiveType out_primitive = storageKindToPrimitive(out_kind);
  std::string schema_name = fmt::format("derived_siso_{}_{}", output_topic_name, next_node_id_);
  auto out_type_tree = PJ::makePrimitive("value", out_primitive);
  auto out_schema_or = engine_.typeRegistry().registerOrGet(schema_name, out_type_tree);
  if (!out_schema_or.has_value()) {
    return PJ::unexpected(out_schema_or.error());
  }

  auto out_topic_or = engine_.createTopic(
      output_dataset_id,
      TopicDescriptor{.name = output_topic_name, .schema_id = *out_schema_or, .dataset_id = output_dataset_id});
  if (!out_topic_or.has_value()) {
    return PJ::unexpected(out_topic_or.error());
  }
  PJ::TopicId out_topic_id = *out_topic_or;

  PJ::NodeId node_id = next_node_id_++;
  DerivedNode node;
  node.id = node_id;
  node.is_mimo = false;
  node.siso_input_topic_id = input_topic_id;
  node.siso_input_column_index = input_column_index;
  node.siso_input_kind = in_kind;
  node.siso_output_kind = out_kind;
  node.siso_op = std::move(op);
  node.all_input_topic_ids = {input_topic_id};
  node.output_topic_ids = {out_topic_id};
  node.dirty = true;

  impl_->registered_output_names[name_key] = out_topic_id;
  impl_->topic_to_nodes[input_topic_id].push_back(node_id);
  impl_->output_topic_to_node[out_topic_id] = node_id;

  auto prod_it = impl_->output_topic_to_node.find(input_topic_id);
  if (prod_it != impl_->output_topic_to_node.end()) {
    impl_->downstream_of[prod_it->second].push_back(node_id);
  }

  impl_->nodes[node_id] = std::move(node);
  return node_id;
}

PJ::Expected<PJ::NodeId> IncrementalEngine::addMimoTransform(
    std::vector<PJ::TopicId> input_topic_ids, std::vector<std::string> output_topic_names,
    PJ::DatasetId output_dataset_id, std::unique_ptr<IMIMOTransform> op, std::vector<std::size_t> input_columns,
    std::string output_topic_group) {
  const bool grouped = !output_topic_group.empty();
  // One engine lock across the input reads, the type registration and createTopic(), as in addSisoTransform().
  auto lock = engine_.lockEngine();
  if (input_topic_ids.empty()) {
    return PJ::unexpected("add_mimo_transform: requires at least one input topic");
  }
  if (output_topic_names.empty()) {
    return PJ::unexpected("add_mimo_transform: requires at least one output topic name");
  }
  if (!op) {
    return PJ::unexpected("add_mimo_transform: null transform op");
  }
  // Default each input to its first leaf column; otherwise one column per input.
  if (input_columns.empty()) {
    input_columns.assign(input_topic_ids.size(), 0);
  } else if (input_columns.size() != input_topic_ids.size()) {
    return PJ::unexpected(
        fmt::format(
            "add_mimo_transform: input_columns has {} entries but {} input topics", input_columns.size(),
            input_topic_ids.size()));
  }

  std::vector<StorageKind> input_kinds;
  input_kinds.reserve(input_topic_ids.size());

  for (std::size_t i = 0; i < input_topic_ids.size(); ++i) {
    const PJ::TopicId tid = input_topic_ids[i];
    const std::size_t col = input_columns[i];
    const TopicStorage* storage = engine_.getTopicStorage(tid);
    if (!storage) {
      return PJ::unexpected(fmt::format("add_mimo_transform: input topic {} not found", tid));
    }

    const LeafColumnLayout layout = resolveLeafColumnLayout(engine_, *storage, col);
    if (layout.column_count == 0) {
      return PJ::unexpected(fmt::format("add_mimo_transform: cannot determine column layout for input topic {}", tid));
    }
    if (col >= layout.column_count) {
      return PJ::unexpected(
          fmt::format(
              "add_mimo_transform: column index {} out of range for input topic {} ({} columns)", col, tid,
              layout.column_count));
    }
    if (!layout.leaf_primitive) {
      return PJ::unexpected(fmt::format("add_mimo_transform: cannot determine primitive type for input topic {}", tid));
    }
    input_kinds.push_back(storageKindOf(*layout.leaf_primitive));
  }

  // A grouped node owns one topic (output_topic_group), an ungrouped one a topic per output name.
  {
    std::vector<std::string> owned_topics = grouped ? std::vector<std::string>{output_topic_group} : output_topic_names;
    for (const auto& name : owned_topics) {
      auto key = std::make_pair(output_dataset_id, name);
      if (impl_->registered_output_names.contains(key)) {
        return PJ::unexpected(
            fmt::format(
                "add_mimo_transform: output topic '{}' already registered in dataset {}", name, output_dataset_id));
      }
    }
  }

  {
    std::string cycle_err = checkCycle(*impl_, input_topic_ids, {});
    if (!cycle_err.empty()) {
      return PJ::unexpected(cycle_err);
    }
  }

  std::vector<StorageKind> output_kinds = op->outputKinds(PJ::Span<const StorageKind>(input_kinds));
  if (output_kinds.size() != output_topic_names.size()) {
    return PJ::unexpected(
        fmt::format(
            "add_mimo_transform: op->outputKinds() returned {} kinds but {} output names provided", output_kinds.size(),
            output_topic_names.size()));
  }

  PJ::NodeId node_id = next_node_id_++;
  std::vector<PJ::TopicId> out_topic_ids;

  if (grouped) {
    // One topic whose struct schema has M columns, named by output_topic_names.
    std::vector<std::shared_ptr<PJ::TypeTreeNode>> fields;
    fields.reserve(output_topic_names.size());
    for (std::size_t k = 0; k < output_topic_names.size(); ++k) {
      fields.push_back(PJ::makePrimitive(output_topic_names[k], storageKindToPrimitive(output_kinds[k])));
    }
    std::string schema_name = fmt::format("derived_mimo_{}_grouped", node_id);
    auto out_schema_or =
        engine_.typeRegistry().registerOrGet(schema_name, PJ::makeStruct(schema_name, std::move(fields)));
    if (!out_schema_or.has_value()) {
      return PJ::unexpected(out_schema_or.error());
    }
    auto out_topic_or = engine_.createTopic(
        output_dataset_id,
        TopicDescriptor{.name = output_topic_group, .schema_id = *out_schema_or, .dataset_id = output_dataset_id});
    if (!out_topic_or.has_value()) {
      return PJ::unexpected(out_topic_or.error());
    }
    out_topic_ids.push_back(*out_topic_or);
  } else {
    // One scalar topic (single "value" column) per output name.
    out_topic_ids.reserve(output_topic_names.size());
    for (std::size_t k = 0; k < output_topic_names.size(); ++k) {
      PJ::PrimitiveType out_primitive = storageKindToPrimitive(output_kinds[k]);
      std::string schema_name = fmt::format("derived_mimo_{}_{}", node_id, k);
      auto out_type_tree = PJ::makePrimitive("value", out_primitive);
      auto out_schema_or = engine_.typeRegistry().registerOrGet(schema_name, out_type_tree);
      if (!out_schema_or.has_value()) {
        return PJ::unexpected(out_schema_or.error());
      }
      auto out_topic_or = engine_.createTopic(
          output_dataset_id,
          TopicDescriptor{.name = output_topic_names[k], .schema_id = *out_schema_or, .dataset_id = output_dataset_id});
      if (!out_topic_or.has_value()) {
        return PJ::unexpected(out_topic_or.error());
      }
      out_topic_ids.push_back(*out_topic_or);
    }
  }

  DerivedNode node;
  node.id = node_id;
  node.is_mimo = true;
  node.mimo_input_topic_ids = input_topic_ids;
  node.mimo_input_columns = std::move(input_columns);
  node.mimo_input_kinds = std::move(input_kinds);
  node.mimo_output_kinds = std::move(output_kinds);
  node.mimo_op = std::move(op);
  node.mimo_grouped = grouped;
  node.mimo_last_ts = std::numeric_limits<PJ::Timestamp>::min();
  node.all_input_topic_ids = std::move(input_topic_ids);
  node.output_topic_ids = std::move(out_topic_ids);
  node.dirty = true;

  if (grouped) {
    impl_->registered_output_names[std::make_pair(output_dataset_id, output_topic_group)] = node.output_topic_ids[0];
  } else {
    for (std::size_t k = 0; k < output_topic_names.size(); ++k) {
      impl_->registered_output_names[std::make_pair(output_dataset_id, output_topic_names[k])] =
          node.output_topic_ids[k];
    }
  }

  for (PJ::TopicId in_tid : node.all_input_topic_ids) {
    impl_->topic_to_nodes[in_tid].push_back(node_id);
  }

  for (PJ::TopicId out_tid : node.output_topic_ids) {
    impl_->output_topic_to_node[out_tid] = node_id;
  }

  for (PJ::TopicId in_tid : node.all_input_topic_ids) {
    auto prod_it = impl_->output_topic_to_node.find(in_tid);
    if (prod_it != impl_->output_topic_to_node.end()) {
      auto& list = impl_->downstream_of[prod_it->second];
      if (std::find(list.begin(), list.end(), node_id) == list.end()) {
        list.push_back(node_id);
      }
    }
  }

  impl_->nodes[node_id] = std::move(node);
  return node_id;
}

// ---------------------------------------------------------------------------
// Node management
// ---------------------------------------------------------------------------

PJ::Status IncrementalEngine::removeNode(PJ::NodeId id) {
  auto it = impl_->nodes.find(id);
  if (it == impl_->nodes.end()) {
    return PJ::unexpected(fmt::format("remove_node: node {} not found", id));
  }

  const DerivedNode& node = it->second;

  for (PJ::TopicId in_tid : node.all_input_topic_ids) {
    auto& v = impl_->topic_to_nodes[in_tid];
    v.erase(std::remove(v.begin(), v.end(), id), v.end());
  }

  for (PJ::TopicId out_tid : node.output_topic_ids) {
    impl_->output_topic_to_node.erase(out_tid);
    for (auto sit = impl_->registered_output_names.begin(); sit != impl_->registered_output_names.end(); ++sit) {
      if (sit->second == out_tid) {
        impl_->registered_output_names.erase(sit);
        break;
      }
    }
  }

  impl_->downstream_of.erase(id);
  for (auto dit = impl_->downstream_of.begin(); dit != impl_->downstream_of.end(); ++dit) {
    auto& list = dit.value();
    list.erase(std::remove(list.begin(), list.end(), id), list.end());
  }

  impl_->nodes.erase(it);
  return PJ::okStatus();
}

bool IncrementalEngine::hasNode(PJ::NodeId id) const noexcept {
  return impl_->nodes.contains(id);
}

std::vector<PJ::TopicId> IncrementalEngine::outputTopics(PJ::NodeId id) const {
  auto it = impl_->nodes.find(id);
  if (it == impl_->nodes.end()) {
    return {};
  }
  return it->second.output_topic_ids;
}

// ---------------------------------------------------------------------------
// Topological order (Kahn's algorithm)
// ---------------------------------------------------------------------------

std::vector<PJ::NodeId> IncrementalEngine::topologicalOrder() const {
  tsl::robin_map<PJ::NodeId, int> in_degree;
  for (const auto& [id, _] : impl_->nodes) {
    in_degree[id] = 0;
  }

  for (const auto& [upstream, downstream_list] : impl_->downstream_of) {
    for (PJ::NodeId downstream : downstream_list) {
      if (impl_->nodes.contains(downstream)) {
        in_degree[downstream]++;
      }
    }
  }

  // In-degree 0 nodes, sorted for a deterministic order.
  std::vector<PJ::NodeId> ready;
  ready.reserve(in_degree.size());
  for (const auto& [id, deg] : in_degree) {
    if (deg == 0) {
      ready.push_back(id);
    }
  }
  std::sort(ready.begin(), ready.end());

  std::vector<PJ::NodeId> order;
  order.reserve(impl_->nodes.size());
  std::size_t head = 0;

  while (head < ready.size()) {
    PJ::NodeId n = ready[head++];
    order.push_back(n);

    auto it = impl_->downstream_of.find(n);
    if (it == impl_->downstream_of.end()) {
      continue;
    }

    std::vector<PJ::NodeId> newly_ready;
    for (PJ::NodeId m : it->second) {
      if (!impl_->nodes.contains(m)) {
        continue;
      }
      if (--in_degree[m] == 0) {
        newly_ready.push_back(m);
      }
    }
    std::sort(newly_ready.begin(), newly_ready.end());
    for (PJ::NodeId m : newly_ready) {
      ready.push_back(m);
    }
  }

  return order;
}

// ---------------------------------------------------------------------------
// Source commits
// ---------------------------------------------------------------------------

void IncrementalEngine::onSourceCommitted(PJ::Span<const PJ::TopicId> changed_topics) {
  for (PJ::TopicId tid : changed_topics) {
    auto it = impl_->topic_to_nodes.find(tid);
    if (it == impl_->topic_to_nodes.end()) {
      continue;
    }
    for (PJ::NodeId nid : it->second) {
      auto nit = impl_->nodes.find(nid);
      if (nit != impl_->nodes.end()) {
        nit.value().dirty = true;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// SISO run
// ---------------------------------------------------------------------------

using SteadyTimePoint = std::chrono::steady_clock::time_point;

// Samples fed between two clock reads of a time-sliced step: bounds the overshoot to a few
// calculate() calls while keeping clock reads (a JS call in the browser) off the per-row path.
constexpr uint32_t kRowsPerDeadlineCheck = 16;

static bool isDeferred(const DerivedNode& node) {
  return node.resume_chunk_id != 0;
}

// Feeds the node's unprocessed input rows to its op and commits the output. With a `deadline` the per-chunk path stops
// at the first clock check past it, leaving resume_chunk_id/row for the next call (the node is then deferred).
static PJ::Status runSisoIncremental(
    IncrementalEngineImpl& /*impl*/, DataEngine& engine, DerivedNode& node, const SteadyTimePoint* deadline = nullptr) {
  const TopicStorage* in_storage = engine.getTopicStorage(node.siso_input_topic_id);
  if (!in_storage) {
    return PJ::unexpected(fmt::format("run_siso_incremental: input topic {} not found", node.siso_input_topic_id));
  }

  const std::deque<TopicChunk>& all_chunks = in_storage->sealedChunks();

  DataWriter writer = engine.createWriter();
  PJ::TopicId out_tid = node.output_topic_ids[0];
  PJ::ChunkId max_seen = node.last_processed_chunk_id;
  PJ::ChunkId resume_chunk_id = 0;
  uint32_t resume_row = 0;
  bool wrote_any = false;
  PJ::Timestamp out_ts = 0;
  PJ::Status status = PJ::okStatus();
  // Rows under the input's retention floor are evicted even while their straddling chunk remains: a replay must not
  // feed them.
  const PJ::Timestamp retention_floor = in_storage->retentionFloor();

  auto feed_row = [&](const TopicChunk& chunk, std::size_t row) {
    if (!status.has_value()) {
      return;  // an earlier row already failed; drain remaining callbacks
    }
    // A chunk sealed before the column appeared carries no sample for it.
    if (node.siso_input_column_index >= chunk.columns.size()) {
      return;
    }
    // A null cell is no sample either: its payload is a placeholder.
    if (chunk.isNull(node.siso_input_column_index, row)) {
      return;
    }
    const PJ::Timestamp ts = chunk.timestamps[row];
    if (ts < retention_floor) {
      return;
    }
    node.in_val_buf = decodeAsVarvalue(chunk, node.siso_input_column_index, row, node.siso_input_kind);
    node.siso_last_ts = std::max(node.siso_last_ts, ts);

    if (node.siso_op->calculate(ts, node.in_val_buf, out_ts, node.out_val_buf)) {
      auto s = writer.beginRow(out_tid, out_ts);
      if (!s.has_value()) {
        status = std::move(s);
        return;
      }
      writeVarvalue(writer, out_tid, 0, node.out_val_buf, node.siso_output_kind);
      s = writer.finishRow(out_tid);
      if (!s.has_value()) {
        status = std::move(s);
        return;
      }
      wrote_any = true;
    }
  };

  if (node.last_processed_chunk_id == 0 && node.resume_chunk_id == 0 && deadline == nullptr) {
    // Fresh node or post-reset replay: chunks may overlap in time (out-of-order ingest), so the merge cursor feeds the
    // rows in the ascending order transforms need.
    for (const TopicChunk& chunk : all_chunks) {
      max_seen = std::max(max_seen, chunk.id);
    }
    rangeQuery(all_chunks, std::numeric_limits<PJ::Timestamp>::min(), std::numeric_limits<PJ::Timestamp>::max())
        .forEach([&feed_row](const SampleRow& row) { feed_row(*row.chunk, row.row_index); });
  } else {
    // Incremental, or a time-sliced replay: the caller checked that the unprocessed chunks are time-ordered (it resets
    // and replays otherwise), so per-chunk iteration matches the merge cursor's order, ties to the earlier chunk too.
    uint32_t rows_fed = 0;
    for (const TopicChunk& chunk : all_chunks) {
      if (chunk.id <= node.last_processed_chunk_id) {
        continue;
      }
      uint32_t row = chunk.id == node.resume_chunk_id ? node.resume_row : 0;
      for (; row < chunk.stats.row_count; ++row) {
        if (deadline != nullptr && ++rows_fed % kRowsPerDeadlineCheck == 0 &&
            std::chrono::steady_clock::now() >= *deadline) {
          break;
        }
        feed_row(chunk, row);
      }
      if (row < chunk.stats.row_count) {
        resume_chunk_id = chunk.id;
        resume_row = row;
        break;
      }
      max_seen = std::max(max_seen, chunk.id);
    }
  }

  if (!status.has_value()) {
    return status;  // hard writer error: no commit, and the watermark stays for a retry
  }
  // Evicted chunks count as consumed, so a replay over a fully evicted input is not a gap
  // again next pass. Not mid-chunk: the resume chunk must stay above the watermark.
  if (resume_chunk_id == 0) {
    max_seen = std::max(max_seen, in_storage->evictedThroughChunkId());
  }
  // A sticky op failure (a script's runtime error, say) fails every later calculate(): the error is returned and the
  // staged rows, a truncated batch, are dropped by not flushing the local writer. On a post-reset replay the caller
  // restores the prior output. The watermark still advances, so a failed node does not re-run every commit.
  node.last_processed_chunk_id = max_seen;
  node.resume_chunk_id = resume_chunk_id;
  node.resume_row = resume_row;
  if (node.siso_op && node.siso_op->failed()) {
    return PJ::unexpected(node.siso_op->error());
  }
  if (wrote_any) {
    auto chunks = writer.flushAll();
    engine.commitChunksLocked(std::move(chunks));  // the caller holds lockEngine()
  }
  return PJ::okStatus();
}

// ---------------------------------------------------------------------------
// MIMO run
// ---------------------------------------------------------------------------

/// Newest chunk id the storage has ever held, retained or evicted.
static PJ::ChunkId newestChunkId(const TopicStorage& storage) {
  const std::deque<TopicChunk>& chunks = storage.sealedChunks();
  return std::max(storage.evictedThroughChunkId(), chunks.empty() ? PJ::ChunkId{0} : chunks.back().id);
}

static PJ::Status runMimoIncremental(IncrementalEngineImpl& /*impl*/, DataEngine& engine, DerivedNode& node) {
  const std::size_t num_inputs = node.mimo_input_topic_ids.size();
  if (num_inputs == 0) {
    return PJ::okStatus();
  }
  // A sticky-failed transform produces no rows until a replace or full recompute resets it, so the join is skipped. Its
  // input still counts as consumed, as for a failed SISO node, or retention would pass the frozen watermark and read as
  // a gap on every tick.
  if (node.mimo_op && node.mimo_op->failed()) {
    node.mimo_input_chunk_ids.resize(num_inputs, 0);
    for (std::size_t i = 0; i < num_inputs; ++i) {
      if (const TopicStorage* storage = engine.getTopicStorage(node.mimo_input_topic_ids[i])) {
        node.mimo_input_chunk_ids[i] = std::max(node.mimo_input_chunk_ids[i], newestChunkId(*storage));
        node.mimo_last_chunk_id = std::max(node.mimo_last_chunk_id, node.mimo_input_chunk_ids[i]);
      }
    }
    return PJ::unexpected(node.mimo_op->error());
  }

  // Each input's rows strictly newer than the timestamp watermark.
  struct SampleLoc {
    PJ::Timestamp ts;
    const TopicChunk* chunk;
    uint32_t row;
  };
  std::vector<std::vector<SampleLoc>> per_topic(num_inputs);

  PJ::ChunkId max_chunk_seen = node.mimo_last_chunk_id;
  std::vector<PJ::ChunkId> input_chunk_ids = node.mimo_input_chunk_ids;
  input_chunk_ids.resize(num_inputs, 0);
  for (std::size_t i = 0; i < num_inputs; ++i) {
    const TopicStorage* storage = engine.getTopicStorage(node.mimo_input_topic_ids[i]);
    if (!storage) {
      return PJ::unexpected(
          fmt::format("run_mimo_incremental: input topic {} not found", node.mimo_input_topic_ids[i]));
    }
    const std::size_t column = node.mimo_input_columns[i];
    const PJ::Timestamp retention_floor = storage->retentionFloor();
    input_chunk_ids[i] = std::max(input_chunk_ids[i], storage->evictedThroughChunkId());  // evicted counts as consumed
    for (const TopicChunk& chunk : storage->sealedChunks()) {
      input_chunk_ids[i] = std::max(input_chunk_ids[i], chunk.id);
      if (chunk.stats.t_max <= node.mimo_last_ts || chunk.stats.t_max < retention_floor) {
        continue;  // already processed, or evicted
      }
      for (uint32_t r = 0; r < chunk.stats.row_count; ++r) {
        PJ::Timestamp ts = chunk.timestamps[r];
        if (ts <= node.mimo_last_ts || ts < retention_floor) {
          continue;
        }
        // A null cell, or a chunk sealed before this column existed, carries no
        // sample of this input: its timestamp must not join.
        if (column >= chunk.columns.size() || chunk.isNull(column, r)) {
          continue;
        }
        per_topic[i].push_back({ts, &chunk, r});
      }
    }
    max_chunk_seen = std::max(max_chunk_seen, input_chunk_ids[i]);
  }
  // Every committed chunk was considered; the regression check compares against this. Updated even without a join, so
  // a fruitless chunk is not flagged again.
  node.mimo_last_chunk_id = max_chunk_seen;
  node.mimo_input_chunk_ids = std::move(input_chunk_ids);
  for (std::size_t i = 0; i < num_inputs; ++i) {
    // If any topic has no new data, no new join is possible.
    if (per_topic[i].empty()) {
      return PJ::okStatus();
    }
  }
  // Gathered in commit order, which out-of-order ingest makes differ from time order. joined_ts comes from topic 0, so
  // it is sorted, stably: equal timestamps keep commit order for the last-write-wins lookups.
  std::stable_sort(
      per_topic[0].begin(), per_topic[0].end(), [](const SampleLoc& a, const SampleLoc& b) { return a.ts < b.ts; });

  // The timestamps present in every input, from topic 0's sorted ones.
  std::vector<PJ::Timestamp> joined_ts;
  joined_ts.reserve(per_topic[0].size());
  for (const auto& s : per_topic[0]) {
    joined_ts.push_back(s.ts);
  }

  for (std::size_t i = 1; i < num_inputs; ++i) {
    tsl::robin_set<PJ::Timestamp> topic_set;
    topic_set.reserve(per_topic[i].size());
    for (const auto& s : per_topic[i]) {
      topic_set.insert(s.ts);
    }
    auto new_end =
        std::remove_if(joined_ts.begin(), joined_ts.end(), [&](PJ::Timestamp t) { return !topic_set.contains(t); });
    joined_ts.erase(new_end, joined_ts.end());
    if (joined_ts.empty()) {
      return PJ::okStatus();
    }
  }

  // Topic 0 can hold one timestamp twice; each joined timestamp is processed once.
  {
    auto new_end = std::unique(joined_ts.begin(), joined_ts.end());
    joined_ts.erase(new_end, joined_ts.end());
  }
  if (joined_ts.empty()) {
    return PJ::okStatus();
  }

  // Per input, timestamp → (chunk, row); a timestamp repeated within an input keeps its last row.
  std::vector<tsl::robin_map<PJ::Timestamp, std::pair<const TopicChunk*, uint32_t>>> lookups(num_inputs);
  for (std::size_t i = 0; i < num_inputs; ++i) {
    lookups[i].reserve(per_topic[i].size());
    for (const auto& s : per_topic[i]) {
      lookups[i].insert_or_assign(s.ts, std::make_pair(s.chunk, s.row));
    }
  }

  // num_outputs is the count of calculate() results: M columns of one topic when grouped, else M scalar topics.
  const std::size_t num_outputs = node.mimo_output_kinds.size();
  node.mimo_in_buf.resize(num_inputs);
  node.mimo_out_buf.resize(num_outputs);

  DataWriter writer = engine.createWriter();
  bool wrote_any = false;

  for (PJ::Timestamp ts : joined_ts) {
    // A defensive bound check: the gather already skipped the rows of chunks without the column.
    bool row_has_all_inputs = true;
    for (std::size_t i = 0; i < num_inputs; ++i) {
      const auto& [chp, row] = lookups[i].at(ts);
      if (node.mimo_input_columns[i] >= chp->columns.size()) {
        row_has_all_inputs = false;
        break;
      }
    }
    if (!row_has_all_inputs) {
      continue;
    }
    for (std::size_t i = 0; i < num_inputs; ++i) {
      const auto& [chp, row] = lookups[i].at(ts);
      node.mimo_in_buf[i] = decodeAsVarvalue(*chp, node.mimo_input_columns[i], row, node.mimo_input_kinds[i]);
    }

    PJ::Timestamp out_ts = ts;
    if (!node.mimo_op->calculate(ts, node.mimo_in_buf, out_ts, node.mimo_out_buf)) {
      continue;
    }
    if (node.mimo_grouped) {
      // One row, M columns, into the single grouped topic.
      const PJ::TopicId out_tid = node.output_topic_ids[0];
      auto s = writer.beginRow(out_tid, out_ts);
      if (!s.has_value()) {
        return s;
      }
      for (std::size_t k = 0; k < num_outputs; ++k) {
        writeVarvalue(writer, out_tid, k, node.mimo_out_buf[k], node.mimo_output_kinds[k]);
      }
      s = writer.finishRow(out_tid);
      if (!s.has_value()) {
        return s;
      }
      wrote_any = true;
    } else {
      for (std::size_t k = 0; k < num_outputs; ++k) {
        auto s = writer.beginRow(node.output_topic_ids[k], out_ts);
        if (!s.has_value()) {
          return s;
        }
        writeVarvalue(writer, node.output_topic_ids[k], 0, node.mimo_out_buf[k], node.mimo_output_kinds[k]);
        s = writer.finishRow(node.output_topic_ids[k]);
        if (!s.has_value()) {
          return s;
        }
      }
      wrote_any = true;
    }
  }

  // Suppression and failure both return false from calculate(). A sticky
  // failure invalidates the staged batch, while suppression is successful.
  if (node.mimo_op && node.mimo_op->failed()) {
    node.mimo_last_ts = joined_ts.back();
    return PJ::unexpected(node.mimo_op->error());
  }

  if (wrote_any) {
    engine.commitChunksLocked(writer.flushAll());  // caller holds lockEngine()
  }

  // Input arriving later at or before this watermark is a regression: nodeInputRegressed() turns it into a replay.
  node.mimo_last_ts = joined_ts.back();

  return PJ::okStatus();
}

// ---------------------------------------------------------------------------
// Out-of-order input detection
// ---------------------------------------------------------------------------

// True when a not-yet-processed input chunk lands at or before data the node already consumed. Applied incrementally it
// would reach the transform out of order (SISO) or skip joined rows (MIMO), so the node is reset and replayed instead.
static bool nodeInputRegressed(DataEngine& engine, const DerivedNode& node) {
  if (!node.is_mimo) {
    const TopicStorage* in_storage = engine.getTopicStorage(node.siso_input_topic_id);
    if (in_storage == nullptr) {
      return false;
    }
    // In commit order, each unprocessed chunk must start at or after everything fed before it, earlier chunks of this
    // batch included.
    PJ::Timestamp watermark = node.siso_last_ts;
    for (const TopicChunk& chunk : in_storage->sealedChunks()) {
      if (chunk.id <= node.last_processed_chunk_id || chunk.stats.row_count == 0) {
        continue;
      }
      // A chunk a time-sliced step stopped inside was checked when its first rows were fed;
      // its remaining rows sort after them.
      if (chunk.id != node.resume_chunk_id && chunk.stats.t_min < watermark) {
        return true;
      }
      watermark = std::max(watermark, chunk.stats.t_max);
    }
    return false;
  }
  for (PJ::TopicId in_tid : node.mimo_input_topic_ids) {
    const TopicStorage* in_storage = engine.getTopicStorage(in_tid);
    if (in_storage == nullptr) {
      continue;
    }
    for (const TopicChunk& chunk : in_storage->sealedChunks()) {
      // <= : the MIMO gather skips rows at the watermark timestamp, so a late
      // row exactly at it would otherwise be lost.
      if (chunk.id > node.mimo_last_chunk_id && chunk.stats.row_count > 0 && chunk.stats.t_min <= node.mimo_last_ts) {
        return true;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Retention-gap detection
// ---------------------------------------------------------------------------

// True when retention took rows of the chunk a time-sliced step stopped inside that the
// node has not fed yet: the chunk was dropped, or the floor reached its first unfed row.
// A floor inside the fed prefix loses the node nothing.
static bool retentionTookUnfedRows(const TopicStorage& storage, const DerivedNode& node) {
  for (const TopicChunk& chunk : storage.sealedChunks()) {
    if (chunk.id == node.resume_chunk_id) {
      return chunk.timestamps[node.resume_row] < storage.retentionFloor();
    }
  }
  return true;
}

// The gap retention left by evicting input chunks the node never consumed, if any. Tested on chunk ids
// (TopicStorage::evictedThroughChunkId() against the node's chunk watermark), not timestamps: the feed watermarks move
// only on a non-null row (SISO) or an exact join (MIMO), so a node over an all-null column would look permanently
// behind. A never-run node replays whatever is retained, so it has nothing to fall behind, unless a time-sliced step
// stopped inside its first chunk; a node stopped inside a chunk lost something only when retention took that chunk's
// unfed rows. `from` is the node's feed watermark (Timestamp min when it fed nothing); `to` is the lagging input's
// first retained timestamp (MIMO: the latest across them).
//
// The MIMO watermark is the largest chunk id across all inputs, and ids come from one global counter, so a newer chunk
// on one input can mask an eviction on another: a gap may be missed, never invented.
static std::optional<IncrementalEngine::Gap> findRetentionGap(DataEngine& engine, const DerivedNode& node) {
  const PJ::ChunkId consumed_through = node.is_mimo ? node.mimo_last_chunk_id : node.last_processed_chunk_id;
  const bool mid_chunk = isDeferred(node);
  if (consumed_through == 0 && !mid_chunk) {
    return std::nullopt;
  }
  std::optional<PJ::Timestamp> first_retained;
  for (const PJ::TopicId in_tid : node.all_input_topic_ids) {
    const TopicStorage* in_storage = engine.getTopicStorage(in_tid);
    if (in_storage == nullptr) {
      continue;
    }
    const PJ::ChunkId evicted_through = in_storage->evictedThroughChunkId();
    if (evicted_through <= consumed_through ||
        (mid_chunk && evicted_through == node.resume_chunk_id && !retentionTookUnfedRows(*in_storage, node))) {
      continue;
    }
    const PJ::Timestamp resumes_at = in_storage->empty() ? in_storage->retentionFloor() : in_storage->timeMin();
    first_retained = std::max(first_retained.value_or(resumes_at), resumes_at);
  }
  if (!first_retained) {
    return std::nullopt;
  }
  return IncrementalEngine::Gap{node.is_mimo ? node.mimo_last_ts : node.siso_last_ts, *first_retained};
}

// ---------------------------------------------------------------------------
// Idle-node gate
// ---------------------------------------------------------------------------

// True when the input holds no chunk numbered above `consumed_through`, evicted ones
// included. An input that cannot be read is not idle: running the node reports it.
static bool inputHasNothingNew(DataEngine& engine, PJ::TopicId topic_id, PJ::ChunkId consumed_through) {
  const TopicStorage* storage = engine.getTopicStorage(topic_id);
  return storage != nullptr && newestChunkId(*storage) <= consumed_through;
}

// True when running the node would change nothing, decided from each input's newest chunk id rather than a scan over
// every retained chunk (a streaming tick marks every node dirty, and most are idle). Input reaches a node only as a
// committed chunk numbered by one global counter above the node's watermark (an append, a late write, a refill), and
// retention raises evictedThroughChunkId(), which newestChunkId() includes, so a gap is never hidden. A MIMO node
// compares each input with that input's own newest id: a chunk sealed before another input's can be committed after
// it. A node with a replay to finish or a failure to report is not idle.
static bool nodeHasNothingNew(DataEngine& engine, const DerivedNode& node) {
  if (node.needs_full_recompute || isDeferred(node) || !node.error.empty()) {
    return false;
  }
  if (!node.is_mimo) {
    return inputHasNothingNew(engine, node.siso_input_topic_id, node.last_processed_chunk_id);
  }
  for (std::size_t i = 0; i < node.mimo_input_topic_ids.size(); ++i) {
    const PJ::ChunkId consumed_through = i < node.mimo_input_chunk_ids.size() ? node.mimo_input_chunk_ids[i] : 0;
    if (!inputHasNothingNew(engine, node.mimo_input_topic_ids[i], consumed_through)) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------

PJ::Status IncrementalEngine::scheduleAll() {
  return scheduleActive({});
}

PJ::Status IncrementalEngine::scheduleActive(const std::unordered_set<PJ::NodeId>& active_nodes) {
  // One engine lock for the whole pass: the run, regression and recompute helpers read inputs and commit outputs
  // through non-locking engine calls. IncrementalEngine runs on the GUI thread only, so the lock holds off the ingest
  // worker.
  auto lock = engine_.lockEngine();
  return scheduleActiveLocked(active_nodes);
}

PJ::Status IncrementalEngine::scheduleActiveLocked(const std::unordered_set<PJ::NodeId>& active_nodes) {
  auto order = topologicalOrder();

  // The nodes to consider: active_nodes and their transitive upstream.
  tsl::robin_set<PJ::NodeId> filter;
  if (!active_nodes.empty()) {
    std::queue<PJ::NodeId> bfs;
    for (PJ::NodeId n : active_nodes) {
      if (impl_->nodes.contains(n)) {
        filter.insert(n);
        bfs.push(n);
      }
    }
    while (!bfs.empty()) {
      PJ::NodeId curr = bfs.front();
      bfs.pop();
      auto nit = impl_->nodes.find(curr);
      if (nit == impl_->nodes.end()) {
        continue;
      }
      for (PJ::TopicId in_tid : nit->second.all_input_topic_ids) {
        auto prod_it = impl_->output_topic_to_node.find(in_tid);
        if (prod_it == impl_->output_topic_to_node.end()) {
          continue;
        }
        PJ::NodeId prod = prod_it->second;
        if (filter.insert(prod).second) {
          bfs.push(prod);
        }
      }
    }
  }

  // A failed node does not stop the others, so one bad filter does not freeze every derived series; the first error is
  // returned.
  PJ::Status first_error = PJ::okStatus();
  for (PJ::NodeId node_id : order) {
    if (!active_nodes.empty() && !filter.contains(node_id)) {
      continue;
    }

    const DerivedNode& node = impl_->nodes.at(node_id);
    if (!node.dirty || isDeferred(node)) {
      continue;  // a deferred node is materializeStep()'s until its replay catches up
    }
    if (PJ::Status s = runNodeLocked(node_id, nullptr); !s.has_value() && first_error.has_value()) {
      first_error = std::move(s);
    }
  }

  return first_error;
}

// ---------------------------------------------------------------------------
// Recompute
// ---------------------------------------------------------------------------

namespace {

// Holds output chunks pulled aside during a replay and puts them back on any exit (an error Status, or an exception
// from a transform's reset()/calculate() or an allocation) unless dismiss() accepts the replay. Without it, unwinding
// would destroy the detached chunks while their TopicStorage sits empty.
class DetachedOutputGuard {
 public:
  /// `capacity`: how many storages adopt() may take, reserved so adopt() does not regrow the vector.
  explicit DetachedOutputGuard(std::size_t capacity) {
    detached_.reserve(capacity);
  }
  DetachedOutputGuard(const DetachedOutputGuard&) = delete;
  DetachedOutputGuard& operator=(const DetachedOutputGuard&) = delete;

  ~DetachedOutputGuard() {
    for (auto& [storage, state] : detached_) {
      storage->restoreChunks(std::move(state));
    }
  }

  /// Pulls `storage`'s chunks aside. The slot is allocated before the detach, so both throwing steps (the vector growth
  /// and detachChunks()' snapshot copies) run while `storage` still owns its chunks; only the non-throwing move-assign
  /// hands them over.
  void adopt(TopicStorage& storage) {
    static_assert(
        std::is_nothrow_move_assignable_v<TopicStorage::DetachedState>,
        "a throwing move would strand detached chunks between the storage and the guard");
    detached_.emplace_back(&storage, TopicStorage::DetachedState{});
    try {
      detached_.back().second = storage.detachChunks();
    } catch (...) {
      // detachChunks()' strong guarantee left `storage` owning its chunks; the placeholder would restore an empty state
      // over them.
      detached_.pop_back();
      throw;
    }
  }

  /// Accepts the replay: the detached chunks are dropped instead of restored.
  void dismiss() noexcept {
    detached_.clear();
  }

 private:
  std::vector<std::pair<TopicStorage*, TopicStorage::DetachedState>> detached_;
};

}  // namespace

// Resets ONE node (its op and watermarks) and replays its retained input into its output topics; downstream nodes are
// recomputeBatchLocked()'s job. detachChunks(), not clearChunks()/retireTopic(), keeps every TopicStorage alive, so
// cached reader pointers stay valid.
//
// On failure (an error Status, such as a hard writer error or a failing processor, or an exception) every output topic
// is left exactly as it was: DetachedOutputGuard puts the prior chunks back. The node then stays dirty and flagged
// needs_full_recompute, since its watermarks were reset while its output was rolled back.
static PJ::Status recomputeNodeSelfOnly(IncrementalEngineImpl& impl, DataEngine& engine, DerivedNode& node) {
  DetachedOutputGuard detached_outputs(node.output_topic_ids.size());
  for (PJ::TopicId out_tid : node.output_topic_ids) {
    TopicStorage* storage = engine.getTopicStorage(out_tid);
    if (storage) {
      detached_outputs.adopt(*storage);
    }
  }

  if (!node.is_mimo) {
    if (node.siso_op) {
      node.siso_op->reset();
    }
  } else {
    if (node.mimo_op) {
      node.mimo_op->reset();
    }
  }

  // Flagged before the replay, so every exit but the successful one, an exception included, keeps the node on the
  // full recompute path.
  node.dirty = true;
  node.needs_full_recompute = true;
  node.last_processed_chunk_id = 0;
  node.resume_chunk_id = 0;
  node.resume_row = 0;
  node.siso_last_ts = std::numeric_limits<PJ::Timestamp>::min();
  if (node.is_mimo) {
    node.mimo_last_ts = std::numeric_limits<PJ::Timestamp>::min();
    node.mimo_last_chunk_id = 0;
    node.mimo_input_chunk_ids.clear();
  }

  PJ::Status s = PJ::okStatus();
  if (!node.is_mimo) {
    s = runSisoIncremental(impl, engine, node);
  } else {
    s = runMimoIncremental(impl, engine, node);
  }

  if (!s.has_value()) {
    node.error = s.error();
    return s;  // ~DetachedOutputGuard restores the pre-call output
  }
  detached_outputs.dismiss();
  node.dirty = false;
  node.needs_full_recompute = false;
  node.error.clear();
  return PJ::okStatus();
}

static void markDownstreamDirty(IncrementalEngineImpl& impl, PJ::NodeId node_id) {
  if (auto dit = impl.downstream_of.find(node_id); dit != impl.downstream_of.end()) {
    for (PJ::NodeId downstream : dit->second) {
      if (auto dnit = impl.nodes.find(downstream); dnit != impl.nodes.end()) {
        dnit.value().dirty = true;
      }
    }
  }
}

PJ::Status IncrementalEngine::runNodeLocked(PJ::NodeId node_id, const SteadyTimePoint* deadline) {
  DerivedNode& node = impl_->nodes.at(node_id);
  if (nodeHasNothingNew(engine_, node)) {
    node.dirty = false;  // its outputs did not change, so nothing downstream is stale
    return PJ::okStatus();
  }
  PJ::Status s = PJ::okStatus();
  const bool regressed = nodeInputRegressed(engine_, node);
  // Checked only when nothing else forces a replay: the gap's `from` is the current watermark, which a pending
  // recompute or a regression resets anyway.
  std::optional<Gap> gap;
  if (!node.needs_full_recompute && !regressed) {
    gap = findRetentionGap(engine_, node);
  }
  if (gap) {
    recordGap(node, *gap);
  }
  const bool full_recompute = node.needs_full_recompute || regressed || gap.has_value();
  if (full_recompute) {
    // A rolled-back recompute (the incremental path would append history on top), late input behind the watermark (out
    // of order for the transform), or an eviction the node never saw (the gap above): each needs a reset and a full
    // replay of what is retained.
    // ponytail: the merge cursor cannot stop midway, so a time-sliced step replays it in
    // one go; slice it if large out-of-order creates show up.
    s = recomputeBatchLocked(node_id);  // already holding engine_.lockEngine()
  } else if (!node.is_mimo) {
    s = runSisoIncremental(*impl_, engine_, node, deadline);
  } else {
    // ponytail: the MIMO join gathers every pending row up front, so it ignores the
    // deadline; slice it (bounded gather horizon) if large multi-input creates show up.
    s = runMimoIncremental(*impl_, engine_, node);
  }

  // Processed even on failure, so a sticky-failed node does not re-run every commit; new input re-dirties it. A failed
  // recompute's rollback stays durable: needs_full_recompute, not dirty, keeps the node off the appending path.
  node.dirty = false;
  if (!s.has_value()) {
    // A failed node gives up its replay: the chunk a step stopped inside counts as consumed, like the rest of a sticky
    // failure's input, so the node is not deferred. Its downstream is left as is.
    if (isDeferred(node)) {
      node.last_processed_chunk_id = std::max(node.last_processed_chunk_id, node.resume_chunk_id);
      node.resume_chunk_id = 0;
      node.resume_row = 0;
    }
    // A recompute recorded each cascaded node's own outcome, and its status may be a downstream node's failure.
    if (!full_recompute) {
      node.error = s.error();
    }
    return s;
  }
  node.error.clear();
  markDownstreamDirty(*impl_, node_id);
  return s;
}

PJ::Expected<bool> IncrementalEngine::materializeStep(PJ::NodeId node_id, std::chrono::nanoseconds budget) {
  const SteadyTimePoint deadline = std::chrono::steady_clock::now() + budget;
  auto lock = engine_.lockEngine();
  auto it = impl_->nodes.find(node_id);
  if (it == impl_->nodes.end()) {
    return PJ::unexpected(fmt::format("materialize_step: node {} not found", node_id));
  }
  if (!it->second.dirty && !isDeferred(it->second)) {
    return true;  // caught up (or processed and failed): same as a scheduling pass skipping it
  }
  if (PJ::Status s = runNodeLocked(node_id, &deadline); !s.has_value()) {
    return PJ::unexpected(s.error());
  }
  return !isDeferred(impl_->nodes.at(node_id));
}

std::vector<PJ::NodeId> IncrementalEngine::deferredNodes() const {
  std::vector<PJ::NodeId> deferred;
  for (const auto& [id, node] : impl_->nodes) {
    if (isDeferred(node)) {
      deferred.push_back(id);
    }
  }
  std::sort(deferred.begin(), deferred.end());
  return deferred;
}

std::vector<IncrementalEngine::Gap> IncrementalEngine::gaps(PJ::NodeId node_id) const {
  const auto it = impl_->nodes.find(node_id);
  return it != impl_->nodes.end() ? it->second.gaps : std::vector<Gap>{};
}

std::string IncrementalEngine::nodeError(PJ::NodeId node_id) const {
  const auto it = impl_->nodes.find(node_id);
  return it != impl_->nodes.end() ? it->second.error : std::string{};
}

PJ::Status IncrementalEngine::recomputeBatch(PJ::NodeId node_id) {
  auto lock = engine_.lockEngine();  // one lock for the whole replay cascade
  return recomputeBatchLocked(node_id);
}

PJ::Status IncrementalEngine::recomputeBatch(PJ::Span<const PJ::NodeId> root_node_ids) {
  auto lock = engine_.lockEngine();
  return recomputeBatchLocked(root_node_ids);
}

// Transitive downstream set over the node graph; seeds join the result only
// when include_seeds (the single-root cascade wants strictly-downstream).
static tsl::robin_set<PJ::NodeId> downstreamClosure(
    const IncrementalEngineImpl& impl, PJ::Span<const PJ::NodeId> seeds, bool include_seeds) {
  tsl::robin_set<PJ::NodeId> reachable;
  std::queue<PJ::NodeId> pending;
  for (const PJ::NodeId seed : seeds) {
    if (!include_seeds || reachable.insert(seed).second) {
      pending.push(seed);
    }
  }
  while (!pending.empty()) {
    const PJ::NodeId current_node_id = pending.front();
    pending.pop();
    const auto downstream = impl.downstream_of.find(current_node_id);
    if (downstream == impl.downstream_of.end()) {
      continue;
    }
    for (const PJ::NodeId child_node_id : downstream->second) {
      if (impl.nodes.contains(child_node_id) && reachable.insert(child_node_id).second) {
        pending.push(child_node_id);
      }
    }
  }
  return reachable;
}

// Resets and replays `nodes` in `topo_order`, never incrementally: the watermark would skip a rewritten upstream
// output. Like scheduleActive(), keeps going after a failure and returns the first error.
static PJ::Status replayNodes(
    IncrementalEngineImpl& impl, DataEngine& engine, const std::vector<PJ::NodeId>& topo_order,
    const tsl::robin_set<PJ::NodeId>& nodes) {
  PJ::Status first_error = PJ::okStatus();
  for (const PJ::NodeId node_id : topo_order) {
    if (!nodes.contains(node_id)) {
      continue;
    }
    PJ::Status status = recomputeNodeSelfOnly(impl, engine, impl.nodes.at(node_id));
    if (!status.has_value() && first_error.has_value()) {
      first_error = std::move(status);
    }
  }
  return first_error;
}

PJ::Status IncrementalEngine::recomputeBatchLocked(PJ::Span<const PJ::NodeId> root_node_ids) {
  for (const PJ::NodeId root_node_id : root_node_ids) {
    if (!impl_->nodes.contains(root_node_id)) {
      return PJ::unexpected(fmt::format("recompute_batch: node {} not found", root_node_id));
    }
  }
  return replayNodes(
      *impl_, engine_, topologicalOrder(), downstreamClosure(*impl_, root_node_ids, /*include_seeds=*/true));
}

PJ::Status IncrementalEngine::recomputeBatchLocked(PJ::NodeId node_id) {
  auto it = impl_->nodes.find(node_id);
  if (it == impl_->nodes.end()) {
    return PJ::unexpected(fmt::format("recompute_batch: node {} not found", node_id));
  }

  // The node first: if it fails, its downstream's input is bad, so the cascade stops.
  PJ::Status self = recomputeNodeSelfOnly(*impl_, engine_, it.value());
  if (!self.has_value()) {
    return self;
  }

  // A rewritten output propagates to every chained node downstream, or they go stale.
  const std::array<PJ::NodeId, 1> seed{node_id};
  const tsl::robin_set<PJ::NodeId> downstream =
      downstreamClosure(*impl_, PJ::Span<const PJ::NodeId>(seed.data(), seed.size()), /*include_seeds=*/false);
  if (downstream.empty()) {
    return PJ::okStatus();
  }
  return replayNodes(*impl_, engine_, topologicalOrder(), downstream);
}

// ---------------------------------------------------------------------------
// Replacement and input bindings
// ---------------------------------------------------------------------------

PJ::Status IncrementalEngine::replaceSisoTransform(PJ::NodeId node_id, std::unique_ptr<ISISOTransform> op) {
  if (!op) {
    return PJ::unexpected("replace_siso_transform: null transform op");
  }
  auto lock = engine_.lockEngine();
  auto it = impl_->nodes.find(node_id);
  if (it == impl_->nodes.end()) {
    return PJ::unexpected(fmt::format("replace_siso_transform: node {} not found", node_id));
  }
  DerivedNode& node = it.value();
  if (node.is_mimo) {
    return PJ::unexpected(fmt::format("replace_siso_transform: node {} is not a SISO node", node_id));
  }

  std::unique_ptr<ISISOTransform> old_op = std::exchange(node.siso_op, std::move(op));
  return recomputeOrRevertLocked(
      node_id, "replace_siso_transform", [&node, &old_op]() { node.siso_op = std::move(old_op); });
}

PJ::Status IncrementalEngine::replaceMimoTransform(PJ::NodeId node_id, std::unique_ptr<IMIMOTransform> op) {
  if (!op) {
    return PJ::unexpected("replace_mimo_transform: null transform op");
  }
  auto lock = engine_.lockEngine();
  auto it = impl_->nodes.find(node_id);
  if (it == impl_->nodes.end()) {
    return PJ::unexpected(fmt::format("replace_mimo_transform: node {} not found", node_id));
  }
  DerivedNode& node = it.value();
  if (!node.is_mimo) {
    return PJ::unexpected(fmt::format("replace_mimo_transform: node {} is not a MIMO node", node_id));
  }

  std::unique_ptr<IMIMOTransform> old_op = std::exchange(node.mimo_op, std::move(op));
  return recomputeOrRevertLocked(
      node_id, "replace_mimo_transform", [&node, &old_op]() { node.mimo_op = std::move(old_op); });
}

PJ::Status IncrementalEngine::recomputeOrRevertLocked(
    PJ::NodeId node_id, std::string_view op_name, const std::function<void()>& revert) {
  PJ::Status applied = recomputeBatchLocked(node_id);
  if (applied.has_value()) {
    return applied;
  }
  // Reverted and replayed again, so the node and its downstream are rebuilt from the restored state; if that replay
  // fails too, both errors go back and the graph state is unknown.
  const std::string apply_error = applied.error();
  revert();
  PJ::Status rolled_back = recomputeBatchLocked(node_id);
  if (!rolled_back.has_value()) {
    return PJ::unexpected(fmt::format("{}: {}; rollback failed: {}", op_name, apply_error, rolled_back.error()));
  }
  return PJ::unexpected(apply_error);
}

PJ::Expected<IncrementalEngine::InputBindingState> IncrementalEngine::inputBindingState(PJ::NodeId node_id) const {
  auto lock = engine_.lockEngine();
  const auto node = impl_->nodes.find(node_id);
  if (node == impl_->nodes.end()) {
    return PJ::unexpected(fmt::format("input_binding_state: node {} not found", node_id));
  }
  if (node->second.is_mimo) {
    return InputBindingState{.columns = node->second.mimo_input_columns, .kinds = node->second.mimo_input_kinds};
  }
  return InputBindingState{.columns = {node->second.siso_input_column_index}, .kinds = {node->second.siso_input_kind}};
}

PJ::Expected<IncrementalEngine::InputBindingState> IncrementalEngine::resolvedInputBindingState(
    PJ::NodeId node_id, const std::vector<std::size_t>& columns) const {
  auto lock = engine_.lockEngine();
  const auto node = impl_->nodes.find(node_id);
  if (node == impl_->nodes.end()) {
    return PJ::unexpected(fmt::format("resolved_input_binding_state: node {} not found", node_id));
  }
  const std::vector<PJ::TopicId>& input_topics =
      node->second.is_mimo ? node->second.mimo_input_topic_ids : node->second.all_input_topic_ids;
  if (columns.size() != input_topics.size()) {
    return PJ::unexpected(fmt::format("resolved_input_binding_state: invalid column count for node {}", node_id));
  }

  std::vector<StorageKind> kinds;
  kinds.reserve(columns.size());
  for (std::size_t input_index = 0; input_index < columns.size(); ++input_index) {
    const TopicStorage* storage = engine_.getTopicStorage(input_topics[input_index]);
    if (storage == nullptr) {
      return PJ::unexpected(
          fmt::format("resolved_input_binding_state: input topic {} not found", input_topics[input_index]));
    }

    const LeafColumnLayout layout = resolveLeafColumnLayout(engine_, *storage, columns[input_index]);
    if (columns[input_index] >= layout.column_count || !layout.leaf_primitive.has_value()) {
      return PJ::unexpected(
          fmt::format(
              "resolved_input_binding_state: column {} unavailable on topic {}", columns[input_index],
              input_topics[input_index]));
    }
    kinds.push_back(storageKindOf(*layout.leaf_primitive));
  }
  return InputBindingState{.columns = columns, .kinds = std::move(kinds)};
}

PJ::Status IncrementalEngine::restoreInputBindingState(PJ::NodeId node_id, const InputBindingState& state) {
  auto lock = engine_.lockEngine();
  auto node = impl_->nodes.find(node_id);
  if (node == impl_->nodes.end()) {
    return PJ::unexpected(fmt::format("restore_input_binding_state: node {} not found", node_id));
  }

  DerivedNode& binding_node = node.value();
  const std::size_t expected_binding_count = binding_node.is_mimo ? binding_node.mimo_input_topic_ids.size() : 1;
  if (state.columns.size() != expected_binding_count || state.kinds.size() != expected_binding_count) {
    return PJ::unexpected(fmt::format("restore_input_binding_state: invalid state for node {}", node_id));
  }
  if (binding_node.is_mimo) {
    binding_node.mimo_input_columns = state.columns;
    binding_node.mimo_input_kinds = state.kinds;
  } else {
    binding_node.siso_input_column_index = state.columns.front();
    binding_node.siso_input_kind = state.kinds.front();
  }
  binding_node.dirty = true;
  // The rewound watermarks below no longer match the untouched output, so the node replays in full rather than
  // appending a second copy of its history.
  binding_node.needs_full_recompute = true;
  binding_node.last_processed_chunk_id = 0;
  binding_node.resume_chunk_id = 0;
  binding_node.resume_row = 0;
  binding_node.siso_last_ts = std::numeric_limits<PJ::Timestamp>::min();
  binding_node.mimo_last_ts = std::numeric_limits<PJ::Timestamp>::min();
  binding_node.mimo_last_chunk_id = 0;
  binding_node.mimo_input_chunk_ids.clear();
  return PJ::okStatus();
}

}  // namespace PJ
