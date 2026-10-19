// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/RecipeGraph.h"
#include "pj_runtime/ResultHistory.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scripting/object_script.h"

namespace PJ {

/// Resolves a node's inputs at a requested time on the GUI thread without fetching or decoding any
/// object payload (`stage`), and decodes them later on any thread (`decode`), so a lazy entry's fetch
/// and chunk decode run on a worker (docs/derived_objects/ARCHITECTURE.md §8, §9 requirement 5). Each
/// input resolves independently at its newest sample at or before the requested time (§7).
class InputStager {
 public:
  /// One scalar sample and the series it was read from.
  struct ScalarSample {
    TopicId topic = 0;
    std::size_t column = 0;
    Timestamp timestamp = 0;
    double value = 0.0;
  };
  /// The sample at or before `t` of the scalar series `name` (bare: the dataset qualifier stripped) names
  /// in `dataset`; nullopt when the series is unknown or has none.
  using ScalarSampler =
      std::function<std::optional<ScalarSample>(DatasetId dataset, std::string_view name, Timestamp t)>;
  /// The topic's parser binding (`SessionManager::parserBindingForObjectTopic`); empty when its entries are
  /// canonical bytes.
  using ParserLookup = std::function<SessionManager::ParserBinding(ObjectTopicId)>;
  /// Holds a plugin library: an object a parser made can run its code when destroyed.
  using Keepalive = std::shared_ptr<const void>;

  /// An object entry, staged by identity: `decode` fetches and decodes it.
  struct Entry {
    ObjectTopicId topic;
    SequentialUID uid;
    sdk::BuiltinObjectType type = sdk::BuiltinObjectType::kNone;
    /// Taken at staging, on the GUI thread: its keepalive holds the parser through the decode, and `decode`
    /// hands it on with the object. Empty: the canonical codec decodes the entry.
    SessionManager::ParserBinding parser;
  };
  /// An object decoded already (an upstream output its caller evaluated), with the libraries it needs. Declared
  /// first, the keepalives outlive the object.
  struct Object {
    std::vector<Keepalive> keepalives;
    sdk::BuiltinObject object;
  };
  struct Input {
    std::string alias;  ///< `recipe.inputs[i]`: the name the script reads
    /// A scalar value copied at staging, an entry to decode (an upstream's output is its ring entry), or an object
    /// an override supplied.
    std::variant<double, Entry, Object> value;
    Timestamp resolved = 0;  ///< the time of the sample it resolved to (a finding's real stamp)
  };
  struct Staged {
    ResultHistory::CacheKey key;  ///< the request's key, with no observations
    Timestamp stamp = 0;          ///< the result time: the newest resolved input
    std::vector<Input> inputs;    ///< parallel to `recipe.inputs`
  };
  /// The decoded inputs and the libraries their objects need: keep `keepalives` until every object made
  /// from `values` is gone (pass them to `ResultHistory::publish` with the result). Declared first, so the
  /// struct itself destroys `values` before them.
  struct Decoded {
    std::vector<Keepalive> keepalives;
    std::vector<scripting::NamedValue> values;
  };

  /// Tags an upstream's ring entry in a cache key: `{kRingIdentity, uid}`. An override's identity carries a tag of its
  /// own, so a caller reading the ring stages the identity this stager does, and a stored result matches it.
  static constexpr uint64_t kRingIdentity = 0;

  /// An upstream output its caller evaluated itself, read instead of the history's ring: how an explicit evaluation
  /// feeds a reader without touching an installed upstream (`GridRunner`).
  struct Override {
    Timestamp stamp = 0;  ///< the upstream result's time, at or before the requested one
    /// Enters the reader's cache key in place of a ring entry's uid: equal for one result, distinct between two.
    std::vector<uint64_t> identity;
    /// A ring entry to decode, the decoded object, or why the output was unavailable (D25: a missing input).
    std::variant<Entry, Object, scripting::Unavailable> value;
  };
  /// The override for output `output` of upstream `upstream`; null: read the history.
  using Overrides = std::function<const Override*(std::string_view upstream, std::string_view output)>;

  /// Keeps references to the store and the history; they must outlive the stager.
  InputStager(const ObjectStore& store, const ResultHistory& history, ScalarSampler scalars, ParserLookup parsers);

  /// GUI thread. Resolves every input of `node` at `t` and builds the request's cache key (§10) under
  /// `epoch`, the dataset's: an object topic by the identity of its entry at or before `t` and of its parser
  /// (nothing is fetched), a scalar by its sample, copied; an upstream output by its ring entry in the
  /// newest result at or before `t` (`ResultHistory::resultAt`), compared by the result's stamp, so a pinned
  /// finding is an input only from its real stamp (decided 2026-10-18). Error `on-demand: missing input '<alias>'` when
  /// an input has nothing at or before `t`, followed by `: <reason>` when it is an upstream output that was unavailable
  /// (D6, D25: no run, the caller clears).
  /// With `overrides`, an upstream output they cover is read from them instead (its stamp, its identity, its value).
  [[nodiscard]] Expected<Staged> stage(
      const RecipeGraph::Node& node, Timestamp t, uint64_t epoch, const Overrides& overrides = {}) const;
  /// GUI thread. The newest stamp among `node`'s raw inputs (an object topic's newest entry, a scalar's newest
  /// sample), found by the same lookups as `stage`, so nothing is fetched; an upstream's output is skipped (the
  /// caller follows the upstream's own inputs). nullopt when none has a sample: any stamp, zero and negative
  /// included, is one.
  [[nodiscard]] std::optional<Timestamp> newestInputStamp(const RecipeGraph::Node& node) const;
  /// GUI thread. Appends to `out` the identity each raw input of `node` (upstream outputs skipped) resolves to at `t`,
  /// as `stage` keys it, or a marker for one with nothing at or before `t`; false when any had nothing.
  bool appendRawIdentities(const RecipeGraph::Node& node, Timestamp t, std::vector<uint64_t>& out) const;

  /// Any thread: touches only `store` (thread-safe) and the parser bindings and objects `staged` carries. Fetches and
  /// decodes each staged entry (an object staged decoded is passed on, its keepalives with it), an object through its
  /// parser under the parser's per-topic lock and a canonical point cloud (an upstream's output among them) as a view
  /// over the store's bytes; each value carries its alias, its entry's `payload_stamp_shift` and its store topic. An
  /// entry gone since staging (evicted), or whose lazy fetch failed, is the same missing-input error as `stage`'s.
  [[nodiscard]] static Expected<Decoded> decode(const ObjectStore& store, const Staged& staged);

 private:
  /// Resolves the raw input `binding` at `t` as `stage` does: appends its identity to `key` and sets `input`'s value.
  /// The stamp it resolved; nullopt when it has nothing at or before `t`.
  std::optional<Timestamp> stageRaw(
      const RecipeGraph::Node& node, const RecipeGraph::Binding& binding, Timestamp t, std::vector<uint64_t>& key,
      Input& input) const;
  /// The object topic `name` of `dataset`, remembered by id and checked against the store's id index on each use, so
  /// a topic removed or registered again under the name is found again (one scan) and the common case scans nothing.
  std::optional<ObjectTopicId> topicOf(DatasetId dataset, const std::string& name) const;

  /// Hashes a string and a string_view alike, so `topics_` is looked up without a copy.
  struct NameHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view name) const {
      return std::hash<std::string_view>{}(name);
    }
  };

  const ObjectStore& store_;
  const ResultHistory& history_;
  ScalarSampler scalars_;
  ParserLookup parsers_;
  /// `topicOf`'s memory, per dataset; GUI thread only, like every caller.
  mutable std::unordered_map<DatasetId, std::unordered_map<std::string, ObjectTopicId, NameHash, std::equal_to<>>>
      topics_;
};

}  // namespace PJ
