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
    } else if (binding.object_type != sdk::BuiltinObjectType::kNone) {
      const std::optional<ObjectTopicId> topic = store_.findTopic(node.dataset, binding.name);
      const auto entry = topic ? store_.latestEntryIdAt(*topic, t) : std::nullopt;
      if (!entry) {
        return missingInput(input.alias);
      }
      SessionManager::ParserBinding parser = parsers_(*topic);
      // Another parser (a reload re-registers the topic's) may decode the same bytes differently.
      // ponytail: identity by address; a parser replaced in place while the entry UIDs stay (a stream topic
      // retyped) may reuse a freed one's address. A reload gets fresh UIDs and a new epoch (6a-5). Upgrade:
      // a generation number in the parser slot.
      const auto parser_identity = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(parser.parser));
      staged.key.inputs.insert(staged.key.inputs.end(), {topic->id, entry->uid.value, parser_identity});
      resolved = entry->timestamp;
      input.value = Entry{*topic, entry->uid, binding.object_type, std::move(parser)};
    } else {
      const std::optional<ScalarSample> sample = scalars_(node.dataset, binding.name, t);
      if (!sample) {
        return missingInput(input.alias);
      }
      staged.key.inputs.insert(
          staged.key.inputs.end(), {sample->topic, sample->column, static_cast<uint64_t>(sample->timestamp),
                                    std::bit_cast<uint64_t>(sample->value)});
      resolved = sample->timestamp;
      input.value = sample->value;
    }
    staged.stamp = std::max(staged.stamp, resolved);
    staged.inputs.push_back(std::move(input));
  }
  return staged;
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
