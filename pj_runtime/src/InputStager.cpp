// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/InputStager.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <utility>

#include "ObjectDecode.h"

namespace PJ {
namespace {

Unexpected<std::string> missingInput(const std::string& alias, const std::string& reason = {}) {
  return unexpected("on-demand: missing input '" + alias + "'" + (reason.empty() ? "" : ": " + reason));
}

}  // namespace

InputStager::InputStager(
    const ObjectStore& store, const ResultHistory& history, ScalarSampler scalars, ParserLookup parsers)
    : store_(store), history_(history), scalars_(std::move(scalars)), parsers_(std::move(parsers)) {}

Expected<InputStager::Staged> InputStager::stage(const RecipeGraph::Node& node, Timestamp t, uint64_t epoch) const {
  Staged staged;
  staged.key.revision = node.revision;
  staged.key.epoch = epoch;
  staged.stamp = std::numeric_limits<Timestamp>::min();
  for (size_t index = 0; index < node.bindings.size(); ++index) {
    const RecipeGraph::Binding& binding = node.bindings[index];
    Input input{node.recipe.inputs[index], 0.0};
    Timestamp resolved = 0;
    if (!binding.upstream.empty()) {
      auto result = history_.resultAt(binding.upstream, binding.name, t);
      if (!result) {
        return missingInput(input.alias);
      }
      if (result->unavailable) {  // D25: its tombstone is no input
        return missingInput(input.alias, *result->unavailable);
      }
      staged.key.inputs.push_back(result->uid.value);
      resolved = result->stamp;
      input.value = Entry{result->topic, result->uid, binding.object_type, {}};
    } else if (const std::optional<Timestamp> raw = stageRaw(node, binding, t, staged.key.inputs, input)) {
      resolved = *raw;
    } else {
      return missingInput(input.alias);
    }
    staged.stamp = std::max(staged.stamp, resolved);
    staged.inputs.push_back(std::move(input));
  }
  return staged;
}

std::optional<Timestamp> InputStager::stageRaw(
    const RecipeGraph::Node& node, const RecipeGraph::Binding& binding, Timestamp t, std::vector<uint64_t>& key,
    Input& input) const {
  if (binding.object_type == sdk::BuiltinObjectType::kNone) {
    const std::optional<ScalarSample> sample = scalars_(node.dataset, binding.name, t);
    if (!sample) {
      return std::nullopt;
    }
    key.insert(
        key.end(), {sample->topic, sample->column, static_cast<uint64_t>(sample->timestamp),
                    std::bit_cast<uint64_t>(sample->value)});
    input.value = sample->value;
    return sample->timestamp;
  }
  const std::optional<ObjectTopicId> topic = topicOf(node.dataset, binding.name);
  const auto entry = topic ? store_.latestEntryIdAt(*topic, t) : std::nullopt;
  if (!entry) {
    return std::nullopt;
  }
  SessionManager::ParserBinding parser = parsers_(*topic);
  // Another parser (a reload re-registers the topic's) may decode the same bytes differently.
  // ponytail: identity by address; a parser replaced in place while the entry UIDs stay (a stream topic
  // retyped) may reuse a freed one's address. A reload gets fresh UIDs and a new epoch (6a-5). Upgrade:
  // a generation number in the parser slot.
  const auto parser_identity = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(parser.parser));
  key.insert(key.end(), {topic->id, entry->uid.value, parser_identity});
  input.value = Entry{*topic, entry->uid, binding.object_type, std::move(parser)};
  return entry->timestamp;
}

bool InputStager::appendRawIdentities(const RecipeGraph::Node& node, Timestamp t, std::vector<uint64_t>& out) const {
  bool complete = true;
  for (const RecipeGraph::Binding& binding : node.bindings) {
    Input scratch;
    if (binding.upstream.empty() && !stageRaw(node, binding, t, out, scratch)) {
      out.push_back(std::numeric_limits<uint64_t>::max());  // nothing at or before `t`
      complete = false;
    }
  }
  return complete;
}

std::optional<ObjectTopicId> InputStager::topicOf(DatasetId dataset, const std::string& name) const {
  auto& names = topics_[dataset];
  if (const auto known = names.find(name); known != names.end() && store_.isTopic(known->second, dataset, name)) {
    return known->second;
  }
  // Not seen yet, removed, or registered again under a new id: one scan, remembered.
  const std::optional<ObjectTopicId> found = store_.findTopic(dataset, name);
  if (found) {
    names.insert_or_assign(name, *found);
  }
  return found;
}

std::optional<Timestamp> InputStager::newestInputStamp(const RecipeGraph::Node& node) const {
  constexpr Timestamp kNewest = std::numeric_limits<Timestamp>::max();
  std::optional<Timestamp> newest;
  const auto take = [&newest](Timestamp stamp) { newest = std::max(newest.value_or(stamp), stamp); };
  for (const RecipeGraph::Binding& binding : node.bindings) {
    if (!binding.upstream.empty()) {
      continue;
    }
    if (binding.object_type != sdk::BuiltinObjectType::kNone) {
      const std::optional<ObjectTopicId> topic = topicOf(node.dataset, binding.name);
      if (const auto entry = topic ? store_.latestEntryIdAt(*topic, kNewest) : std::nullopt) {
        take(entry->timestamp);
      }
    } else if (const std::optional<ScalarSample> sample = scalars_(node.dataset, binding.name, kNewest)) {
      take(sample->timestamp);
    }
  }
  return newest;
}

Expected<InputStager::Decoded> InputStager::decode(const ObjectStore& store, const Staged& staged) {
  Decoded decoded;
  decoded.values.reserve(staged.inputs.size());
  for (const Input& input : staged.inputs) {
    if (const auto* scalar = std::get_if<double>(&input.value)) {
      decoded.values.push_back({input.alias, *scalar});
    } else {
      const Entry& entry = std::get<Entry>(input.value);
      // A lazy entry's fetch and chunk decode run here, on the calling thread.
      const std::optional<ResolvedObjectEntry> resolved = store.at(entry.topic, entry.uid);
      if (!resolved || resolved->fetch_failed) {
        return missingInput(input.alias);  // evicted since staging, or its source could not re-read it
      }
      auto parsed = decodeObjectEntry(entry.parser, entry.type, resolved->timestamp, resolved->payload);
      if (!parsed) {
        return unexpected("on-demand: cannot decode input '" + input.alias + "': " + parsed.error());
      }
      if (entry.parser) {
        decoded.keepalives.push_back(entry.parser.keepalive);
      }
      decoded.values.push_back({input.alias, std::move(*parsed), resolved->payload_stamp_shift});
    }
  }
  return decoded;
}

}  // namespace PJ
