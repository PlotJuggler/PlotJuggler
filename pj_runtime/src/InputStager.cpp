// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/InputStager.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

#include "ObjectDecode.h"
#include "pj_datastore/reader.hpp"

namespace PJ {
namespace {

Unexpected<std::string> missingInput(const std::string& alias, const std::string& reason = {}) {
  return unexpected("on-demand: missing input '" + alias + "'" + (reason.empty() ? "" : ": " + reason));
}

}  // namespace

std::optional<InputStager::ScalarSample> InputStager::sampleAt(
    const DataEngine& engine, TopicId topic, std::size_t column, Timestamp t) {
  const DataReader reader(engine);
  if (auto values = reader.series(topic, column)) {
    const auto sample = values->sampleAtOrBeforeTime(t);
    if (!sample) {
      return std::nullopt;
    }
    return ScalarSample{topic, column, sample->timestamp, sample->chunk->id, sample->row_index, sample->value};
  }
  // Not a numeric series: a string one, or nothing.
  auto text = reader.latestStringAt(QueryPoint{.topic_id = topic, .t = t}, column);
  if (!text || !*text) {
    return std::nullopt;
  }
  StringSample& cell = **text;
  ScalarSample sample{topic, column, cell.timestamp, cell.chunk_id, cell.row_index, {}};
  if (cell.value) {
    sample.value = std::move(*cell.value);
  }
  return sample;
}

InputStager::InputStager(
    const ObjectStore& store, const ResultHistory& history, ScalarSampler scalars, ParserLookup parsers)
    : store_(store), history_(history), scalars_(std::move(scalars)), parsers_(std::move(parsers)) {}

Expected<InputStager::Staged> InputStager::stage(
    const RecipeGraph::Node& node, Timestamp t, uint64_t epoch, const Overrides& overrides) const {
  Staged staged;
  staged.key.revision = node.revision;
  staged.key.epoch = epoch;
  staged.stamp = std::numeric_limits<Timestamp>::min();
  for (size_t index = 0; index < node.bindings.size(); ++index) {
    const RecipeGraph::Binding& binding = node.bindings[index];
    Input input{node.recipe.inputs[index], 0.0};
    Timestamp resolved = 0;
    const Override* override =
        !binding.upstream.empty() && overrides ? overrides(binding.upstream, binding.name) : nullptr;
    if (override != nullptr) {
      if (const auto* unavailable = std::get_if<scripting::Unavailable>(&override->value)) {
        return missingInput(input.alias, unavailable->reason);
      }
      staged.key.inputs.insert(staged.key.inputs.end(), override->identity.begin(), override->identity.end());
      resolved = override->stamp;
      if (const auto* entry = std::get_if<Entry>(&override->value)) {
        input.value = *entry;
      } else {
        input.value = std::get<Object>(override->value);
      }
    } else if (!binding.upstream.empty()) {
      auto result = history_.resultAt(binding.upstream, binding.name, t);
      if (!result) {
        return missingInput(input.alias);
      }
      if (result->unavailable) {  // D25: its tombstone is no input
        return missingInput(input.alias, *result->unavailable);
      }
      staged.key.inputs.insert(staged.key.inputs.end(), {kRingIdentity, result->uid.value});
      resolved = result->stamp;
      input.value = Entry{result->topic, result->uid, binding.object_type, {}};
    } else if (
        const std::optional<Timestamp> raw =
            stageRaw(node, binding, t, staged.key.inputs, staged.key.video_content, input)) {
      resolved = *raw;
    } else {
      return missingInput(input.alias);
    }
    staged.stamp = std::max(staged.stamp, resolved);
    input.resolved = resolved;
    staged.inputs.push_back(std::move(input));
  }
  return staged;
}

std::optional<Timestamp> InputStager::stageRaw(
    const RecipeGraph::Node& node, const RecipeGraph::Binding& binding, Timestamp t, std::vector<uint64_t>& key,
    std::vector<uint64_t>& video_content, Input& input) const {
  if (binding.object_type == sdk::BuiltinObjectType::kNone) {
    std::optional<ScalarSample> sample = scalars_(node.dataset, binding.name, t);
    if (!sample || std::holds_alternative<std::monostate>(sample->value)) {
      return std::nullopt;
    }
    key.insert(key.end(), {sample->topic, sample->column, sample->chunk, sample->row});
    if (const auto* number = std::get_if<double>(&sample->value)) {
      input.value = *number;
    } else {
      input.value = std::move(std::get<std::string>(sample->value));
    }
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
  if (binding.object_type == sdk::BuiltinObjectType::kVideoFrame) {
    // `frame_at` may read any packet of the topic, and which ones is a function of this key: its content joins it.
    video_content.push_back(store_.contentIdentity(*topic).content);
  }
  input.value = Entry{*topic, entry->uid, binding.object_type, std::move(parser)};
  return entry->timestamp;
}

bool InputStager::appendRawIdentities(const RecipeGraph::Node& node, Timestamp t, std::vector<uint64_t>& out) const {
  bool complete = true;
  for (const RecipeGraph::Binding& binding : node.bindings) {
    Input scratch;
    if (binding.upstream.empty() && !stageRaw(node, binding, t, out, out, scratch)) {
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

void InputStager::visitNewestStamps(
    const RecipeGraph::Node& node, const std::function<void(std::optional<Timestamp>)>& visit) const {
  constexpr Timestamp kNewest = std::numeric_limits<Timestamp>::max();
  for (const RecipeGraph::Binding& binding : node.bindings) {
    if (!binding.upstream.empty()) {
      continue;
    }
    if (binding.object_type != sdk::BuiltinObjectType::kNone) {
      const std::optional<ObjectTopicId> topic = topicOf(node.dataset, binding.name);
      const auto entry = topic ? store_.latestEntryIdAt(*topic, kNewest) : std::nullopt;
      visit(entry ? std::optional(entry->timestamp) : std::nullopt);
    } else {
      const std::optional<ScalarSample> sample = scalars_(node.dataset, binding.name, kNewest);
      visit(sample ? std::optional(sample->timestamp) : std::nullopt);
    }
  }
}

std::optional<Timestamp> InputStager::newestInputStamp(const RecipeGraph::Node& node) const {
  std::optional<Timestamp> newest;
  visitNewestStamps(node, [&newest](std::optional<Timestamp> stamp) {
    if (stamp) {
      newest = std::max(newest.value_or(*stamp), *stamp);
    }
  });
  return newest;
}

std::optional<Timestamp> InputStager::laggingInputStamp(const RecipeGraph::Node& node) const {
  std::optional<Timestamp> lagging = std::numeric_limits<Timestamp>::max();
  visitNewestStamps(node, [&lagging](std::optional<Timestamp> stamp) {
    lagging = lagging && stamp ? std::optional(std::min(*lagging, *stamp)) : std::nullopt;
  });
  return lagging;
}

namespace {

/// One keepalive owning all of `owners` (null for none), for an input a script's proxies hold past its run.
InputStager::Keepalive grouped(const std::vector<InputStager::Keepalive>& owners) {
  if (owners.size() <= 1) {
    return owners.empty() ? nullptr : owners.front();
  }
  return std::make_shared<const std::vector<InputStager::Keepalive>>(owners);
}

}  // namespace

Expected<InputStager::Decoded> InputStager::decode(const ObjectStore& store, const Staged& staged) {
  Decoded decoded;
  decoded.values.reserve(staged.inputs.size());
  for (const Input& input : staged.inputs) {
    if (const auto* scalar = std::get_if<double>(&input.value)) {
      decoded.values.push_back({.name = input.alias, .value = *scalar});
    } else if (const auto* text = std::get_if<std::string>(&input.value)) {
      decoded.values.push_back({.name = input.alias, .value = *text});
    } else if (const auto* object = std::get_if<Object>(&input.value)) {
      decoded.keepalives.insert(decoded.keepalives.end(), object->keepalives.begin(), object->keepalives.end());
      decoded.values.push_back(
          {.name = input.alias, .keepalive = grouped(object->keepalives), .value = object->object});
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
      Keepalive library = entry.parser ? entry.parser.keepalive : nullptr;
      if (library != nullptr) {
        decoded.keepalives.push_back(library);
      }
      decoded.values.push_back(
          {.name = input.alias,
           .keepalive = std::move(library),
           .value = std::move(*parsed),
           .stamp_shift = resolved->payload_stamp_shift,
           .topic = store.descriptor(entry.topic).topic_name,
           .topic_id = entry.topic});
    }
  }
  return decoded;
}

}  // namespace PJ
