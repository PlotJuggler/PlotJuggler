// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/RecipeGraph.h"
#include "pj_scripting/object_script.h"

namespace PJ {

/// The store timestamp of a finding (D26): a pinned node's single entry sits here, so every
/// at-or-before read returns it at any `t`. Its payload keeps the real stamp. A sentinel, never a
/// time: no consumer may do arithmetic on it (a lag, a lifetime or a range reads the payload stamp).
inline constexpr Timestamp kFindingTimestamp = std::numeric_limits<Timestamp>::min();

/// One serialized object output per object output of a recipe, in declaration order; nullopt where the value is
/// not an object of the declared type (an unavailable output's tombstone needs the GUI thread's frame).
using ResultPayloads = std::vector<std::optional<std::vector<uint8_t>>>;

/// How `ResultHistory::publish` writes a result.
struct ResultPublishOptions {
  /// A result already in the ring at the stamp stays, and this one is only cached: it is a late result that does
  /// not serve the cursor's request, and the one there does. When nothing sits at the stamp, the result is
  /// written as usual and may be shown until the cursor's result lands.
  bool cache_only = false;
  /// `ResultHistory::serializeOutputs(bundle)`, made off the GUI thread; empty: serialized at the publication.
  ResultPayloads payloads;
};

/// Where the on-demand results of every node live (docs/derived_objects/ARCHITECTURE.md §9, §10):
/// the ring (each object output's derived topic in the ObjectStore, serialized) and the compute cache
/// (decoded bundles, without the object outputs the ring holds). Each has one global byte budget and the same rule: a
/// window around the node's cursor, then, while over budget, the result farthest from its node's cursor goes, across
/// nodes. Nothing is refused. A node's cursor is its last requested time (`noteRequest`; a node's first result sets it
/// when no request did).
///
/// GUI thread only: no Qt, no threads, no script runs. Number and string outputs never reach the store.
class ResultHistory {
 public:
  /// What determined a result (§10). The request's key leaves the observations empty; a stored key
  /// also records what its run observed.
  struct CacheKey {
    uint64_t revision = 0;         ///< `RecipeGraph::Node::revision`
    uint64_t epoch = 0;            ///< the dataset's, bumped when its storage is replaced
    std::vector<uint64_t> inputs;  ///< opaque input identity (entry uids, parsers, scalar samples...)
    /// The content revision of each bound video topic (`ObjectStore::contentIdentity`): a run's `frame_at` may read
    /// any packet of the topic.
    std::vector<uint64_t> video_content;
    std::optional<Timestamp> query_clock;  ///< the requested `t`, when the run read the query clock
    std::optional<uint64_t> tf_revision;   ///< the TF snapshot's revision, when the run read the TF tree

    bool operator==(const CacheKey&) const = default;
    /// This stored key serves `request` (a key with no observations) at `t` under the TF snapshot `tf_revision`:
    /// the base fields are equal, and what the run observed is the same now.
    [[nodiscard]] bool serves(const CacheKey& request, Timestamp t, uint64_t tf_revision_now) const {
      return video_content == request.video_content && completes(request, t, tf_revision_now);
    }
    /// `serves`, but a moved video content revision alone does not count: a run's completion still serves the current
    /// request, since what it superseded is new packets, not the meaning of the request (D30).
    [[nodiscard]] bool completes(const CacheKey& request, Timestamp t, uint64_t tf_revision_now) const {
      return revision == request.revision && epoch == request.epoch && inputs == request.inputs &&
             (!query_clock || *query_clock == t) && (!tf_revision || *tf_revision == tf_revision_now);
    }
  };

  struct Limits {
    std::size_t ring_bytes = std::size_t{256} << 20;   ///< serialized bytes, every derived topic together
    std::size_t cache_bytes = std::size_t{256} << 20;  ///< decoded bytes (estimated), every node together
    Timestamp window = 30'000'000'000;                 ///< results kept within ± this of the cursor, ns
  };

  /// A result `lookup` found.
  struct Hit {
    Timestamp stamp = 0;  ///< the result time
    /// The cached result, shared with the cache; null when only the ring holds it. When `in_ring`, its object
    /// outputs are `Unavailable` placeholders (the store shows them) beside its number and string outputs;
    /// otherwise it is whole, and an output may be an object a parser made (an input passed through): hold
    /// `bundle` as long as any object copied out of it.
    std::shared_ptr<const scripting::ScriptBundle> bundle;
    /// The ring holds this result at its stamp, so the store shows it already; false: only the cache holds it
    /// (a late result, `ResultPublishOptions::cache_only`), and republishing `bundle` under `key` shows it.
    bool in_ring = false;
    /// The stored key, to republish `bundle` under; set only when `in_ring` is false.
    std::optional<CacheKey> key;
  };

  /// What the ring shows of a node at a time.
  struct Shown {
    Timestamp stamp = 0;     ///< the result time; a finding's real stamp
    bool available = false;  ///< an object output is available: something is drawn
  };

  explicit ResultHistory(ObjectStore& store) : ResultHistory(store, Limits{}) {}
  ResultHistory(ObjectStore& store, Limits limits);

  /// Registers `<key>/<output>` for each object output in the node's dataset (`derivedTopicMetadata`; a
  /// topic of that name and kind is reused). A new revision drops the previous revision's results: an
  /// output it keeps (same name, type and dataset) keeps its topic id, one it drops, renames or retypes has
  /// its topic retired. Returns the retired ids; announcing them is the caller's. The same revision again
  /// changes nothing. A new output whose name a topic of another kind holds (one this install does not
  /// retire), or a store failure dropping the previous results, fails with nothing changed: the previous
  /// revision stays installed with its topics and results. Only another writer taking one of the names
  /// after that check fails with the previous results dropped; `publish` then refuses the node until an
  /// install succeeds.
  [[nodiscard]] Expected<std::vector<ObjectTopicId>> install(const RecipeGraph::Node& node);
  /// Drops the node's results and retires its topics; returns the retired ids (none for an unknown key).
  std::vector<ObjectTopicId> forget(std::string_view key);
  /// Drops every result of the nodes bound to `dataset`; their topics stay. A store failure leaves a
  /// node's results to the next clear.
  void clearDataset(DatasetId dataset);
  /// `clearDataset` for one node, whose results an upstream's new meaning made stale.
  void clear(std::string_view key);

  /// Moves the node's cursor to `t`, its last requested time: the centre of both windows and of the
  /// eviction distance, and where its displayed result is read. Call it on every request, hit or not.
  void noteRequest(std::string_view key, Timestamp t);

  /// Records one result of `node` (its installed revision), requested at `requested_t`, whose newest
  /// resolved input is at `stamp`. Writes its object outputs to the ring in one `pushOwnedBundle`, so a
  /// rejected publication writes nothing (an allocation failure inside the store is its documented
  /// exception). A payload keeps the stamp its script left; an unavailable object output writes an empty
  /// object of its type stamped `stamp` (a tombstone; a point cloud's goes in the frame the output was last
  /// published in). A finding is written at `kFindingTimestamp`. Writes nothing to the store when `key`
  /// already sits at that stamp with every entry still present (the dedup ledger), except to trim the window;
  /// a different result at the same stamp replaces it, unless `options.cache_only` (a late result that does
  /// not serve the cursor's request: the entry there does; see `ResultPublishOptions`). Then caches `bundle`, without a
  /// decoded copy of the object outputs the ring now holds: a result the ring does not hold is cached whole.
  /// `keepalives` hold the plugin libraries the run's inputs need: an input passed through to an output may be an
  /// object a parser made, so the cached bundle drops its outputs before them. Only the node's first result sets its
  /// cursor (to `requested_t`): a late result keeps the one the last request set. Then trims both windows around the
  /// cursor, sparing the ring's displayed result, and enforces both budgets. On error nothing is cached or written.
  [[nodiscard]] Status publish(
      const RecipeGraph::Node& node, Timestamp requested_t, Timestamp stamp, const CacheKey& key,
      scripting::ScriptBundle bundle, std::vector<std::shared_ptr<const void>> keepalives = {},
      ResultPublishOptions options = {});
  /// Any thread: the object outputs of `bundle` (declared as `outputs`) serialized for `publish`.
  [[nodiscard]] static ResultPayloads serializeOutputs(
      Span<const scripting::OutputSpec> outputs, const scripting::ScriptBundle& bundle);

  /// The result for `request` (a key with no observations) at `t`, whose resolved inputs make it a result at
  /// `stamp`: one of that stamp whose stored key `serves` it, in the compute cache (placed at `stamp`, or at `t`
  /// for a run that read the clock) or in the ring (an entry evicted from the store counts as absent, and so does
  /// a cached result whose objects only that entry held). O(log n); never deserializes nor copies a bundle.
  /// nullopt: a miss.
  [[nodiscard]] std::optional<Hit> lookup(
      std::string_view key, const CacheKey& request, Timestamp stamp, Timestamp t, uint64_t tf_revision) const;

  /// The newest result of `key` at or before `t` whose entries are still in the store (a finding by its real
  /// stamp); nullopt when there is none.
  [[nodiscard]] std::optional<Shown> shownAt(std::string_view key, Timestamp t) const;
  /// The result at `stamp` serves the request at `t`: removes the tombstones (results whose key has no input
  /// identity: what the scheduler writes for a missing input or a failure) stamped after it, up to `t`, which
  /// would hide it. Returns whether any went.
  bool supersedeTombstones(std::string_view key, Timestamp stamp, Timestamp t);
  /// Whether `key`'s stored results were dropped (a new revision's install, `clear`, `clearDataset`) with nothing
  /// written since: a layer may still draw one of them, which the ring no longer shows.
  [[nodiscard]] bool resultsDropped(std::string_view key) const;

  /// One object output of a stored result, as a downstream input reads it: from its ring entry, since the
  /// compute cache keeps no decoded copy of it.
  struct OutputAt {
    Timestamp stamp = 0;  ///< the result time; a finding's real stamp, never `kFindingTimestamp`
    ObjectTopicId topic;
    SequentialUID uid;  ///< the output's entry in the ring; a new revision's results get new ones
    /// Why the output was unavailable (D25): its ring entry is only a tombstone, not an input.
    std::optional<std::string> unavailable;
  };
  /// Object output `output` of the newest result of `key` whose stamp is at or before `t` and whose entries
  /// are still in the store. A finding is compared by its real stamp, so before it was computed there is
  /// none (decided 2026-10-18). Never deserializes. nullopt: no such result or output. The scheduler asks
  /// after the upstream's own request at `t`, so this is the upstream's displayed result, which the ring
  /// never evicts.
  [[nodiscard]] std::optional<OutputAt> resultAt(std::string_view key, std::string_view output, Timestamp t) const;

  /// The derived topics of `key`'s object outputs, in declaration order; empty for an unknown key.
  [[nodiscard]] std::vector<ObjectTopicId> topicsOf(std::string_view key) const;
  /// The node whose object output `topic` is; nullopt for any other topic. Scans every node's outputs.
  [[nodiscard]] std::optional<std::string> keyOf(ObjectTopicId topic) const;

  /// Serialized bytes the ring holds, every node together.
  [[nodiscard]] std::size_t ringBytes() const {
    return ring_bytes_;
  }
  /// Serialized bytes the ring holds for `key`; 0 for an unknown key. Walks its ledger.
  [[nodiscard]] std::size_t ringBytesOf(std::string_view key) const;
  /// The ring's bytes past its budget. After a publication's budget pass, that is what the results the
  /// pass may not evict (each node's displayed result, findings) hold beyond the budget.
  [[nodiscard]] std::size_t protectedExcessBytes() const {
    return ring_bytes_ > limits_.ring_bytes ? ring_bytes_ - limits_.ring_bytes : 0;
  }
  /// Estimated decoded bytes the compute cache holds, every node together.
  [[nodiscard]] std::size_t cacheBytes() const {
    return cache_bytes_;
  }

 private:
  /// One result in the ring: all of a node's object outputs at one store timestamp.
  struct Stored {
    CacheKey key;
    Timestamp stamp = 0;  ///< the result time (a finding's store timestamp is not)
    /// One per object output, in `NodeResults::outputs` order: its entry, and why the output was unavailable
    /// (the entry is a tombstone; nullopt when available).
    struct Entry {
      SequentialUID uid;
      std::optional<std::string> unavailable;
    };
    std::vector<Entry> entries;
    std::size_t bytes = 0;  ///< serialized
  };
  struct Cached {
    CacheKey key;
    Timestamp stamp = 0;
    /// The ring holds this result at its stamp: `bundle` keeps no decoded object output (placeholders), and the
    /// entry goes with its ring entry (`eraseStored`), since it cannot serve a hit alone.
    bool ring_held = false;
    /// Holds its run's input keepalives too, which it drops after its outputs (`publish`).
    std::shared_ptr<const scripting::ScriptBundle> bundle;
    std::size_t bytes = 0;  ///< estimated, decoded, plus the entry itself
  };
  /// One object output and its derived topic.
  struct Output {
    std::string name;
    sdk::BuiltinObjectType type = sdk::BuiltinObjectType::kNone;
    ObjectTopicId topic;
    std::size_t index = 0;  ///< its position among the recipe's outputs, and a bundle's
    std::string frame;      ///< the frame its last point cloud was in, where its tombstone goes
  };
  using Ledger = std::map<Timestamp, Stored>;  ///< by store timestamp; follows the committed store
  /// By window position: the instant the run observed (`query_clock`), else the result time.
  using Cache = std::multimap<Timestamp, Cached>;
  struct NodeResults {
    uint64_t revision = 0;  ///< the installed one; 0 until an install succeeds
    DatasetId dataset = 0;
    bool pinned = false;
    std::optional<Timestamp> cursor;
    std::vector<Output> outputs;  ///< the object outputs, in declaration order
    Ledger ledger;
    Cache cache;
    bool dropped = false;  ///< `resultsDropped`
  };

  /// Writes one result's object outputs, or only trims the window when it is already stored.
  [[nodiscard]] Status writeRing(
      NodeResults& node, Timestamp stamp, const CacheKey& key, const scripting::ScriptBundle& bundle,
      ResultPublishOptions options);
  /// Caches `bundle` unless `key` is cached already as `ring_held` says, then trims the cache to the window
  /// around the cursor, sparing this publication's entry.
  void cacheResult(
      NodeResults& node, const CacheKey& key, Timestamp stamp, std::shared_ptr<const scripting::ScriptBundle> bundle,
      bool ring_held);
  /// True when every entry of `stored` is still in the store.
  [[nodiscard]] bool present(const NodeResults& node, const Stored& stored) const;
  /// The newest ledger entry at or before `t` (by store timestamp) still in the store; the ledger's end if none.
  [[nodiscard]] Ledger::const_iterator newestPresentAtOrBefore(const NodeResults& node, Timestamp t) const;
  /// The cached result of `key` at `stamp`; the cache's end if none.
  [[nodiscard]] static Cache::const_iterator findCached(const NodeResults& node, const CacheKey& key, Timestamp stamp);
  /// Removes every entry of the node's remaining topics in one bundle, then `releaseResults`. On a store
  /// error nothing changes.
  [[nodiscard]] Status dropResults(NodeResults& node);
  /// Forgets the node's ledger and cache and their bytes, leaving the store alone.
  void releaseResults(NodeResults& node);
  // Every byte count changes through these four. `eraseStored` also erases the result's `ring_held` cache entry.
  void addStored(NodeResults& node, Timestamp at, Stored stored);
  Ledger::iterator eraseStored(NodeResults& node, Ledger::iterator entry);
  Cache::iterator addCached(NodeResults& node, Timestamp position, Cached cached);
  Cache::iterator eraseCached(NodeResults& node, Cache::const_iterator entry);
  /// Past the budget: forgets entries gone from the store, then evicts the results farthest from their
  /// node's cursor, sparing each node's displayed result and findings, every victim in one bundle.
  void enforceRingBudget();
  /// Past the budget: evicts the cached results farthest from their node's cursor.
  void enforceCacheBudget();

  ObjectStore& store_;
  Limits limits_;
  std::map<std::string, NodeResults, std::less<>> nodes_;
  std::size_t ring_bytes_ = 0;
  std::size_t cache_bytes_ = 0;
};

}  // namespace PJ
