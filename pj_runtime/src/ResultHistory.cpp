// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/ResultHistory.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>
#include <variant>

#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_datastore/saturating_time.hpp"
#include "pj_runtime/DerivedTopics.h"

namespace PJ {
namespace {

using scripting::OutputType;

std::pair<Timestamp, Timestamp> windowAround(Timestamp t, Timestamp window) {
  return {saturatingSub(t, window), saturatingAdd(t, window)};
}

/// |a - b|: any two Timestamps are at most 2^64 - 1 apart, which a signed difference cannot hold.
uint64_t distance(Timestamp a, Timestamp b) {
  return a > b ? static_cast<uint64_t>(a) - static_cast<uint64_t>(b)
               : static_cast<uint64_t>(b) - static_cast<uint64_t>(a);
}

/// Where the compute cache places a result: a run that read the clock at the instant it read, so many such
/// results of one stamp spread over the window instead of piling up at the stamp.
Timestamp cachePosition(const ResultHistory::CacheKey& key, Timestamp stamp) {
  return key.query_clock.value_or(stamp);
}

/// What a cached bundle keeps of an object output the ring holds.
const scripting::Unavailable kHeldByTheRing{"held by the ring"};

/// A cached bundle and what its outputs need alive. Members go in reverse order: the outputs before the
/// plugin libraries that made them.
struct OwnedBundle {
  std::vector<std::shared_ptr<const void>> keepalives;
  scripting::ScriptBundle bundle;
};

/// What a layer shows at `stamp` for an unavailable output: nothing, never the previous geometry (D6). At
/// stamp 0 a tombstone can serialize to zero bytes (empty annotations), which the SDK 0.36 decoders refuse;
/// the scheduler stamps a missing-input tombstone at the requested time, and the decoder fix is an SDK
/// follow-up.
Expected<std::vector<uint8_t>> emptyObjectBytes(
    sdk::BuiltinObjectType type, Timestamp stamp, const std::string& frame) {
  sdk::BuiltinObject empty;
  switch (type) {
    case sdk::BuiltinObjectType::kSceneEntities: {
      sdk::SceneEntities scene;
      scene.deletions.push_back({sdk::SceneEntityDeletion::Type::kAll, stamp, {}});
      empty = std::move(scene);
      break;
    }
    case sdk::BuiltinObjectType::kPointCloud: {
      sdk::PointCloud cloud;
      cloud.frame_id = frame;
      cloud.timestamp_ns = stamp;
      empty = std::move(cloud);
      break;
    }
    case sdk::BuiltinObjectType::kImageAnnotations: {
      sdk::ImageAnnotations annotations;
      annotations.timestamp = stamp;
      empty = std::move(annotations);
      break;
    }
    case sdk::BuiltinObjectType::kImage: {
      sdk::Image image;
      image.timestamp_ns = stamp;
      empty = std::move(image);
      break;
    }
    case sdk::BuiltinObjectType::kDepthImage: {
      sdk::DepthImage depth;
      depth.timestamp_ns = stamp;
      empty = std::move(depth);
      break;
    }
    case sdk::BuiltinObjectType::kCameraInfo: {
      sdk::CameraInfo info;
      info.timestamp_ns = stamp;
      empty = std::move(info);
      break;
    }
    case sdk::BuiltinObjectType::kVoxelGrid: {
      sdk::VoxelGrid grid;
      grid.frame_id = frame;
      grid.timestamp_ns = stamp;
      empty = std::move(grid);
      break;
    }
    default:
      return unexpected("no empty " + std::string(sdk::name(type)) + " to clear an unavailable output with");
  }
  return serializeBuiltinObject(empty);
}

/// The entries of one node an eviction pass may take, sorted by position with the cursor among them:
/// the farthest from the cursor is always at the front or the back.
template <class Map, class Owner>
struct EvictionRange {
  Owner* owner = nullptr;
  Timestamp cursor = 0;
  typename Map::iterator front;   ///< the first entry not taken
  typename Map::iterator back;    ///< one past the last entry not taken
  typename Map::iterator spared;  ///< never taken; the map's end for none
};

/// Takes entries from both ends of the ranges, the farthest from its cursor first, until `excess` bytes
/// are covered or nothing is left. What a range gave up is [begin, front) and [back, end), less `spared`.
template <class Map, class Owner>
void takeFarthest(std::vector<EvictionRange<Map, Owner>>& ranges, size_t excess) {
  for (size_t taken = 0; taken < excess;) {
    EvictionRange<Map, Owner>* best = nullptr;
    bool from_front = false;
    uint64_t farthest = 0;
    for (auto& range : ranges) {
      if (range.front != range.back && range.front == range.spared) {
        ++range.front;
      }
      if (range.front != range.back && std::prev(range.back) == range.spared) {
        --range.back;
      }
      if (range.front == range.back) {
        continue;
      }
      const uint64_t front_gap = distance(range.front->first, range.cursor);
      const uint64_t back_gap = distance(std::prev(range.back)->first, range.cursor);
      if (best == nullptr || std::max(front_gap, back_gap) > farthest) {
        best = &range;
        from_front = front_gap >= back_gap;
        farthest = std::max(front_gap, back_gap);
      }
    }
    if (best == nullptr) {
      return;
    }
    taken += (from_front ? best->front++ : --best->back)->second.bytes;
  }
}

/// Calls `visit` on each entry `range` gave up; `visit` returns the iterator after the entry (it may erase it).
template <class Map, class Owner, class Visit>
void forEachTaken(Map& map, const EvictionRange<Map, Owner>& range, Visit&& visit) {
  for (auto entry = map.begin(); entry != range.front;) {
    entry = entry == range.spared ? std::next(entry) : visit(entry);
  }
  for (auto entry = range.back; entry != map.end();) {
    entry = entry == range.spared ? std::next(entry) : visit(entry);
  }
}

}  // namespace

ResultHistory::ResultHistory(ObjectStore& store, Limits limits) : store_(store), limits_(limits) {
  limits_.window = std::max<Timestamp>(0, limits_.window);
}

Expected<std::vector<ObjectTopicId>> ResultHistory::install(const RecipeGraph::Node& graph_node) {
  const std::string key = graph_node.recipe.key();
  const auto found = nodes_.find(key);
  if (found != nodes_.end() && found->second.revision == graph_node.revision) {
    return std::vector<ObjectTopicId>{};
  }

  // An output keeps its topic when its name, type and dataset stay; the others' topics are retired.
  std::vector<Output> retired = found != nodes_.end() ? found->second.outputs : std::vector<Output>{};
  const bool same_dataset = found != nodes_.end() && found->second.dataset == graph_node.dataset;
  std::vector<Output> outputs;
  const auto& specs = graph_node.recipe.outputs;
  for (size_t index = 0; index < specs.size(); ++index) {
    if (specs[index].type != OutputType::kObject) {
      continue;
    }
    Output output{specs[index].name, specs[index].object_type, {}, index, {}};
    const auto match = std::find_if(retired.begin(), retired.end(), [&](const Output& old) {
      return same_dataset && old.name == output.name && old.type == output.type;
    });
    if (match != retired.end()) {
      output.topic = match->topic;
      retired.erase(match);
    }
    outputs.push_back(std::move(output));
  }
  // Before anything changes: a name held by a topic of another kind, unless this install retires it (a retyped
  // output), is what `findOrRegisterTopic` would refuse.
  for (const Output& output : outputs) {
    if (output.topic != ObjectTopicId{}) {  // kept; id 0 is never a topic's
      continue;
    }
    const std::string name = key + "/" + output.name;
    const std::optional<ObjectTopicId> taken = store_.findTopic(graph_node.dataset, name);
    if (taken && std::none_of(retired.begin(), retired.end(), [&](const Output& old) { return old.topic == *taken; }) &&
        store_.descriptor(*taken).metadata_json != derivedTopicMetadata(output.type)) {
      return unexpected("on-demand: cannot register '" + name + "': a topic of another kind holds the name");
    }
  }

  NodeResults& node = nodes_[key];
  if (Status dropped = dropResults(node); !dropped) {
    return unexpected("on-demand: cannot clear '" + key + "': " + dropped.error());
  }
  node.revision = 0;
  std::vector<ObjectTopicId> retired_ids;
  for (const Output& old : retired) {
    store_.removeTopic(old.topic);
    retired_ids.push_back(old.topic);
  }
  node.outputs = std::move(outputs);
  node.dataset = graph_node.dataset;

  // Fails only when another writer took a name since the check. The node holds every topic it registered:
  // `forget` or the next install retires it.
  std::string failure;
  for (Output& output : node.outputs) {
    if (output.topic != ObjectTopicId{}) {
      continue;
    }
    auto topic =
        store_.findOrRegisterTopic({graph_node.dataset, key + "/" + output.name, derivedTopicMetadata(output.type)});
    if (topic) {
      output.topic = *topic;
    } else {
      failure = "on-demand: cannot register '" + key + "/" + output.name + "': " + topic.error();
    }
  }
  if (!failure.empty()) {
    std::erase_if(node.outputs, [](const Output& output) { return output.topic == ObjectTopicId{}; });
    return unexpected(failure);
  }
  node.revision = graph_node.revision;
  node.pinned = graph_node.recipe.pinned_t.has_value();
  return retired_ids;
}

std::vector<ObjectTopicId> ResultHistory::forget(std::string_view key) {
  const auto found = nodes_.find(key);
  if (found == nodes_.end()) {
    return {};
  }
  std::vector<ObjectTopicId> retired;
  for (const Output& output : found->second.outputs) {
    store_.removeTopic(output.topic);
    retired.push_back(output.topic);
  }
  releaseResults(found->second);
  nodes_.erase(found);
  return retired;
}

void ResultHistory::clearDataset(DatasetId dataset) {
  for (auto& [key, node] : nodes_) {
    if (node.dataset == dataset) {
      (void)dropResults(node);
    }
  }
}

void ResultHistory::clear(std::string_view key) {
  if (const auto found = nodes_.find(key); found != nodes_.end()) {
    (void)dropResults(found->second);
  }
}

std::vector<ObjectTopicId> ResultHistory::topicsOf(std::string_view key) const {
  std::vector<ObjectTopicId> topics;
  if (const auto found = nodes_.find(key); found != nodes_.end()) {
    for (const Output& output : found->second.outputs) {
      topics.push_back(output.topic);
    }
  }
  return topics;
}

std::size_t ResultHistory::ringBytesOf(std::string_view key) const {
  const auto found = nodes_.find(key);
  if (found == nodes_.end()) {
    return 0;
  }
  std::size_t bytes = 0;
  for (const auto& entry : found->second.ledger) {
    bytes += entry.second.bytes;
  }
  return bytes;
}

std::optional<std::string> ResultHistory::keyOf(ObjectTopicId topic) const {
  for (const auto& [key, node] : nodes_) {
    if (std::ranges::any_of(node.outputs, [topic](const Output& output) { return output.topic == topic; })) {
      return key;
    }
  }
  return std::nullopt;
}

void ResultHistory::noteRequest(std::string_view key, Timestamp t) {
  if (const auto found = nodes_.find(key); found != nodes_.end()) {
    found->second.cursor = t;
  }
}

Status ResultHistory::publish(
    const RecipeGraph::Node& graph_node, Timestamp requested_t, Timestamp stamp, const CacheKey& key,
    scripting::ScriptBundle bundle, std::vector<std::shared_ptr<const void>> keepalives, ResultPublishOptions options) {
  // From here on, every path destroys the outputs before the libraries that made them.
  const auto owned = std::make_shared<OwnedBundle>(OwnedBundle{std::move(keepalives), std::move(bundle)});
  const scripting::ScriptBundle& result = owned->bundle;
  const std::string node_key = graph_node.recipe.key();
  const auto found = nodes_.find(node_key);
  if (found == nodes_.end() || found->second.revision != graph_node.revision) {
    return unexpected(
        "on-demand: '" + node_key + "' is not installed at revision " + std::to_string(graph_node.revision));
  }
  if (result.outputs.size() != graph_node.recipe.outputs.size()) {
    return unexpected(
        "on-demand: '" + node_key + "' published " + std::to_string(result.outputs.size()) + " outputs, it declares " +
        std::to_string(graph_node.recipe.outputs.size()));
  }
  NodeResults& node = found->second;
  if (!node.cursor) {
    node.cursor = requested_t;
  }
  if (Status written = writeRing(node, stamp, key, result, std::move(options)); !written) {
    return unexpected("on-demand: '" + node_key + "': " + written.error());
  }
  node.dropped = false;
  // The ring holds the objects serialized: the cache keeps no second, decoded copy of them.
  const auto held = node.ledger.find(node.pinned ? kFindingTimestamp : stamp);
  const bool ring_held = held != node.ledger.end() && held->second.key == key;
  if (ring_held) {
    for (const Output& output : node.outputs) {
      scripting::NamedValue& held_output = owned->bundle.outputs[output.index];
      held_output.value = kHeldByTheRing;
      held_output.keepalive.reset();  // a Python output's anchor holds the object too; the value goes first
    }
  }
  cacheResult(node, key, stamp, std::shared_ptr<const scripting::ScriptBundle>(owned, &owned->bundle), ring_held);
  enforceRingBudget();
  enforceCacheBudget();
  return okStatus();
}

Status ResultHistory::writeRing(
    NodeResults& node, Timestamp stamp, const CacheKey& key, const scripting::ScriptBundle& bundle,
    ResultPublishOptions options) {
  if (node.outputs.empty()) {
    return okStatus();
  }
  const Timestamp at = node.pinned ? kFindingTimestamp : stamp;
  const auto same = node.ledger.find(at);
  const bool stored = same != node.ledger.end() && present(node, same->second);
  if (stored && options.cache_only && same->second.key != key) {
    return okStatus();  // the entry there serves the cursor's request; this result is only cached
  }
  const bool duplicate = stored && same->second.key == key;
  const bool prepared = options.payloads.size() == node.outputs.size();
  // The result the cursor shows once this one lands (the newest entry at or before it still in the store)
  // stays whatever the window says, so a late publication never takes it away: at most that one entry
  // outlives the window.
  std::optional<Timestamp> displayed;
  if (const auto shown = newestPresentAtOrBefore(node, *node.cursor); shown != node.ledger.end()) {
    displayed = shown->first;
  }
  if (at <= *node.cursor && (!displayed || at > *displayed)) {
    displayed = at;
  }
  // One rule for the store's drop and the ledger: another result at this stamp is replaced, and an entry
  // farther than the window from the cursor goes, except this result's own, the displayed one and a finding.
  const auto [lo, hi] = windowAround(*node.cursor, limits_.window);
  const auto evicts = [pinned = node.pinned, duplicate, at, displayed, lo, hi](Timestamp t) {
    return t == at ? !duplicate : !pinned && t != displayed && (t < lo || t > hi);
  };
  const auto evicted = [&evicts](const auto& entry) { return evicts(entry.first); };
  const bool trims = std::any_of(node.ledger.begin(), node.ledger.lower_bound(lo), evicted) ||
                     std::any_of(node.ledger.upper_bound(hi), node.ledger.end(), evicted);
  if (duplicate && !trims) {
    return okStatus();  // stored already, and nothing to trim
  }
  // A drop scans every entry of its topic: pass one only when something goes, which an entry the ledger does not
  // know (a count that differs from it) may also need.
  const bool drops = trims || (same != node.ledger.end() && !duplicate) ||
                     std::any_of(node.outputs.begin(), node.outputs.end(), [&](const Output& output) {
                       return store_.entryCount(output.topic) != node.ledger.size();
                     });

  std::vector<OwnedObjectWrite> writes;
  std::vector<std::string> frames;  // each output's, once this result is committed
  std::vector<Stored::Entry> entries;
  size_t bytes = 0;
  for (size_t position = 0; position < node.outputs.size(); ++position) {
    const Output& output = node.outputs[position];
    OwnedObjectWrite write;
    write.topic = output.topic;
    write.timestamp = at;
    write.append = !duplicate;
    if (drops) {
      write.drop = [evicts](Timestamp t, SequentialUID) { return evicts(t); };
    }
    if (!duplicate) {
      const scripting::ScriptValue& value = bundle.outputs[output.index].value;
      const auto* result = std::get_if<sdk::BuiltinObject>(&value);
      auto payload = [&]() -> Expected<std::vector<uint8_t>> {
        if (prepared && options.payloads[position]) {
          return std::move(*options.payloads[position]);
        }
        if (result != nullptr && result->type() == output.type) {
          return serializeBuiltinObject(*result);
        }
        if (std::holds_alternative<scripting::Unavailable>(value)) {
          return emptyObjectBytes(output.type, stamp, output.frame);
        }
        return unexpected("output '" + output.name + "' is not a " + std::string(sdk::name(output.type)));
      }();
      if (!payload) {
        return unexpected(payload.error());
      }
      bytes += payload->size();
      write.payload = std::move(*payload);
      const sdk::PointCloud* cloud = result != nullptr ? result->get<sdk::PointCloud>() : nullptr;
      frames.push_back(cloud != nullptr ? cloud->frame_id : output.frame);
      const auto* reason = std::get_if<scripting::Unavailable>(&value);
      entries.push_back({{}, reason != nullptr ? std::optional<std::string>(reason->reason) : std::nullopt});
    }
    writes.push_back(std::move(write));
  }
  auto uids = store_.pushOwnedBundle(std::move(writes));
  if (!uids) {
    return unexpected("publication rejected: " + uids.error());
  }

  for (auto entry = node.ledger.begin(); drops && entry != node.ledger.end();) {
    entry = evicts(entry->first) ? eraseStored(node, entry) : std::next(entry);
  }
  if (!duplicate) {
    for (size_t index = 0; index < entries.size(); ++index) {
      entries[index].uid = (*uids)[index];
    }
    addStored(node, at, Stored{key, stamp, std::move(entries), bytes});
    for (size_t index = 0; index < frames.size(); ++index) {
      node.outputs[index].frame = std::move(frames[index]);
    }
  }
  return okStatus();
}

void ResultHistory::cacheResult(
    NodeResults& node, const CacheKey& key, Timestamp stamp, std::shared_ptr<const scripting::ScriptBundle> bundle,
    bool ring_held) {
  auto own = findCached(node, key, stamp);
  if (own != node.cache.end() && own->second.ring_held != ring_held) {
    eraseCached(node, own);  // a cache-only result the ring now holds: keep it without its objects
    own = node.cache.end();
  }
  if (own == node.cache.end()) {
    size_t bytes = sizeof(Cached) + (key.inputs.size() + key.video_content.size()) * sizeof(uint64_t);
    for (const scripting::NamedValue& output : bundle->outputs) {
      bytes += scripting::estimateBytes(output.value);
    }
    own = addCached(node, cachePosition(key, stamp), Cached{key, stamp, ring_held, std::move(bundle), bytes});
  }
  const auto [lo, hi] = windowAround(*node.cursor, limits_.window);
  for (auto entry = node.cache.begin(); entry != node.cache.end() && entry->first < lo;) {
    entry = entry == own ? std::next(entry) : eraseCached(node, entry);
  }
  for (auto entry = node.cache.upper_bound(hi); entry != node.cache.end();) {
    entry = entry == own ? std::next(entry) : eraseCached(node, entry);
  }
}

bool ResultHistory::present(const NodeResults& node, const Stored& stored) const {
  for (size_t index = 0; index < node.outputs.size(); ++index) {
    if (!store_.indexOf(node.outputs[index].topic, stored.entries[index].uid).has_value()) {  // decode-free
      return false;
    }
  }
  return true;
}

ResultHistory::Ledger::const_iterator ResultHistory::newestPresentAtOrBefore(
    const NodeResults& node, Timestamp t) const {
  for (auto entry = std::make_reverse_iterator(node.ledger.upper_bound(t)); entry != node.ledger.rend(); ++entry) {
    if (present(node, entry->second)) {
      return std::prev(entry.base());
    }
  }
  return node.ledger.end();
}

ResultHistory::Cache::const_iterator ResultHistory::findCached(
    const NodeResults& node, const CacheKey& key, Timestamp stamp) {
  const auto [first, last] = node.cache.equal_range(cachePosition(key, stamp));
  const auto found = std::find_if(
      first, last, [&](const auto& entry) { return entry.second.stamp == stamp && entry.second.key == key; });
  return found == last ? node.cache.end() : found;
}

Status ResultHistory::dropResults(NodeResults& node) {
  std::vector<OwnedObjectWrite> prune;
  for (const Output& output : node.outputs) {
    if (store_.entryCount(output.topic) == 0) {
      continue;  // nothing to prune, or the topic is gone with its dataset
    }
    OwnedObjectWrite write;
    write.topic = output.topic;
    write.append = false;
    write.drop = [](Timestamp, SequentialUID) { return true; };
    prune.push_back(std::move(write));
  }
  if (!prune.empty()) {
    if (auto pruned = store_.pushOwnedBundle(std::move(prune)); !pruned) {
      return unexpected(pruned.error());
    }
    node.dropped = true;
  }
  releaseResults(node);
  return okStatus();
}

void ResultHistory::releaseResults(NodeResults& node) {
  for (auto entry = node.ledger.begin(); entry != node.ledger.end();) {
    entry = eraseStored(node, entry);
  }
  for (auto entry = node.cache.begin(); entry != node.cache.end();) {
    entry = eraseCached(node, entry);
  }
}

void ResultHistory::addStored(NodeResults& node, Timestamp at, Stored stored) {
  ring_bytes_ += stored.bytes;
  node.ledger.emplace(at, std::move(stored));
}

ResultHistory::Ledger::iterator ResultHistory::eraseStored(NodeResults& node, Ledger::iterator entry) {
  if (const auto cached = findCached(node, entry->second.key, entry->second.stamp);
      cached != node.cache.end() && cached->second.ring_held) {
    eraseCached(node, cached);  // its objects leave with this entry
  }
  ring_bytes_ -= entry->second.bytes;
  return node.ledger.erase(entry);
}

ResultHistory::Cache::iterator ResultHistory::addCached(NodeResults& node, Timestamp position, Cached cached) {
  cache_bytes_ += cached.bytes;
  return node.cache.emplace(position, std::move(cached));
}

ResultHistory::Cache::iterator ResultHistory::eraseCached(NodeResults& node, Cache::const_iterator entry) {
  cache_bytes_ -= entry->second.bytes;
  return node.cache.erase(entry);
}

void ResultHistory::enforceRingBudget() {
  if (ring_bytes_ <= limits_.ring_bytes) {
    return;
  }
  // An entry no longer whole in the store (evicted behind the history's back) is neither displayed nor
  // charged: it leaves the ledger without a store call. A member that survived goes with a window trim.
  // The ledger mirrors the store unless something else touched it, which a count mismatch tells.
  for (auto& [key, node] : nodes_) {
    const bool touched = std::any_of(node.outputs.begin(), node.outputs.end(), [&](const Output& output) {
      return store_.entryCount(output.topic) != node.ledger.size();
    });
    for (auto entry = node.ledger.begin(); touched && entry != node.ledger.end();) {
      entry = present(node, entry->second) ? std::next(entry) : eraseStored(node, entry);
    }
  }
  if (ring_bytes_ <= limits_.ring_bytes) {
    return;
  }
  std::vector<EvictionRange<Ledger, NodeResults>> ranges;
  for (auto& [key, node] : nodes_) {
    if (node.pinned || !node.cursor || node.ledger.empty()) {
      continue;  // a finding is never evicted
    }
    // The displayed result: the newest at or before the cursor, what a layer reads now.
    const auto after = node.ledger.upper_bound(*node.cursor);
    const auto displayed = after == node.ledger.begin() ? node.ledger.end() : std::prev(after);
    ranges.push_back({&node, *node.cursor, node.ledger.begin(), node.ledger.end(), displayed});
  }
  takeFarthest(ranges, ring_bytes_ - limits_.ring_bytes);

  // A result is all of a node's outputs at one stamp: one bundle evicts every victim, never half a result.
  std::vector<OwnedObjectWrite> writes;
  for (const auto& range : ranges) {
    std::vector<Timestamp> stamps;
    forEachTaken(range.owner->ledger, range, [&](Ledger::iterator entry) {
      stamps.push_back(entry->first);  // ascending: the front, then the back
      return std::next(entry);
    });
    if (stamps.empty()) {
      continue;
    }
    for (const Output& output : range.owner->outputs) {
      OwnedObjectWrite write;
      write.topic = output.topic;
      write.append = false;
      write.drop = [stamps](Timestamp t, SequentialUID) { return std::binary_search(stamps.begin(), stamps.end(), t); };
      writes.push_back(std::move(write));
    }
  }
  // ponytail: a topic removed behind the history's back fails the whole pass; the ledger keeps its
  // results and the next publication retries. Evict per node if that ever matters.
  if (writes.empty() || !store_.pushOwnedBundle(std::move(writes))) {
    return;
  }
  for (const auto& range : ranges) {
    forEachTaken(range.owner->ledger, range, [&](Ledger::iterator entry) { return eraseStored(*range.owner, entry); });
  }
}

void ResultHistory::enforceCacheBudget() {
  if (cache_bytes_ <= limits_.cache_bytes) {
    return;
  }
  std::vector<EvictionRange<Cache, NodeResults>> ranges;
  for (auto& [key, node] : nodes_) {
    if (node.cursor && !node.cache.empty()) {
      ranges.push_back({&node, *node.cursor, node.cache.begin(), node.cache.end(), node.cache.end()});
    }
  }
  takeFarthest(ranges, cache_bytes_ - limits_.cache_bytes);
  for (const auto& range : ranges) {
    forEachTaken(range.owner->cache, range, [&](Cache::iterator entry) { return eraseCached(*range.owner, entry); });
  }
}

std::optional<ResultHistory::Hit> ResultHistory::lookup(
    std::string_view key, const CacheKey& request, Timestamp stamp, Timestamp t, uint64_t tf_revision) const {
  const auto found = nodes_.find(key);
  if (found == nodes_.end()) {
    return std::nullopt;
  }
  const NodeResults& node = found->second;
  const auto stored = node.ledger.find(node.pinned ? kFindingTimestamp : stamp);
  const bool ring_holds = stored != node.ledger.end() && present(node, stored->second);
  // A cached result sits at its stamp, or at the instant it read (`cachePosition`).
  for (const Timestamp position : {stamp, t}) {
    const auto [first, last] = node.cache.equal_range(position);
    for (auto entry = first; entry != last; ++entry) {
      const Cached& cached = entry->second;
      if (cached.stamp == stamp && cached.key.serves(request, t, tf_revision)) {
        const bool in_ring = node.outputs.empty() || (ring_holds && stored->second.key == cached.key);
        if (cached.ring_held && !in_ring) {
          continue;  // its objects left with its ring entry, behind the history's back
        }
        return Hit{cached.stamp, cached.bundle, in_ring, in_ring ? std::nullopt : std::optional<CacheKey>(cached.key)};
      }
    }
    if (stamp == t) {
      break;
    }
  }
  if (ring_holds && stored->second.key.serves(request, t, tf_revision)) {
    return Hit{stored->second.stamp, nullptr, true, std::nullopt};
  }
  return std::nullopt;
}

std::optional<ResultHistory::Shown> ResultHistory::shownAt(std::string_view key, Timestamp t) const {
  const auto found = nodes_.find(key);
  if (found == nodes_.end()) {
    return std::nullopt;
  }
  const auto shown = newestPresentAtOrBefore(found->second, t);
  if (shown == found->second.ledger.end() || shown->second.stamp > t) {
    return std::nullopt;
  }
  const auto& entries = shown->second.entries;
  const bool available = std::any_of(
      entries.begin(), entries.end(), [](const Stored::Entry& entry) { return !entry.unavailable.has_value(); });
  return Shown{shown->second.stamp, available};
}

bool ResultHistory::resultsDropped(std::string_view key) const {
  const auto found = nodes_.find(key);
  return found != nodes_.end() && found->second.dropped;
}

bool ResultHistory::supersedeTombstones(std::string_view key, Timestamp stamp, Timestamp t) {
  const auto found = nodes_.find(key);
  if (found == nodes_.end() || found->second.pinned || found->second.outputs.empty()) {
    return false;
  }
  NodeResults& node = found->second;
  std::vector<Timestamp> gone;  // ascending: the ledger's order
  for (auto entry = node.ledger.upper_bound(stamp); entry != node.ledger.end() && entry->first <= t; ++entry) {
    if (entry->second.key.inputs.empty()) {
      gone.push_back(entry->first);
    }
  }
  if (gone.empty()) {
    return false;
  }
  std::vector<OwnedObjectWrite> writes;
  for (const Output& output : node.outputs) {
    OwnedObjectWrite write;
    write.topic = output.topic;
    write.append = false;
    write.drop = [gone](Timestamp at, SequentialUID) { return std::binary_search(gone.begin(), gone.end(), at); };
    writes.push_back(std::move(write));
  }
  if (!store_.pushOwnedBundle(std::move(writes))) {
    return false;  // a store failure leaves them to the next result
  }
  for (const Timestamp at : gone) {
    eraseStored(node, node.ledger.find(at));
  }
  return true;
}

ResultPayloads ResultHistory::serializeOutputs(
    Span<const scripting::OutputSpec> outputs, const scripting::ScriptBundle& bundle) {
  ResultPayloads payloads;
  for (size_t index = 0; index < outputs.size(); ++index) {
    if (outputs[index].type != OutputType::kObject) {
      continue;
    }
    const auto* object =
        index < bundle.outputs.size() ? std::get_if<sdk::BuiltinObject>(&bundle.outputs[index].value) : nullptr;
    std::optional<std::vector<uint8_t>> bytes;
    if (object != nullptr && object->type() == outputs[index].object_type) {
      if (auto serialized = serializeBuiltinObject(*object)) {
        bytes = std::move(*serialized);
      }
    }
    payloads.push_back(std::move(bytes));
  }
  return payloads;
}

std::optional<ResultHistory::OutputAt> ResultHistory::resultAt(
    std::string_view key, std::string_view output_name, Timestamp t) const {
  const auto found = nodes_.find(key);
  if (found == nodes_.end()) {
    return std::nullopt;
  }
  const NodeResults& node = found->second;
  const auto output = std::find_if(
      node.outputs.begin(), node.outputs.end(), [&](const Output& candidate) { return candidate.name == output_name; });
  if (output == node.outputs.end()) {
    return std::nullopt;
  }
  // A finding's store timestamp is at or before every `t`: its real stamp decides.
  const auto shown = newestPresentAtOrBefore(node, t);
  if (shown == node.ledger.end() || shown->second.stamp > t) {
    return std::nullopt;
  }
  const Stored::Entry& entry = shown->second.entries[static_cast<size_t>(output - node.outputs.begin())];
  return OutputAt{shown->second.stamp, output->topic, entry.uid, entry.unavailable};
}

}  // namespace PJ
