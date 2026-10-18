#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/time.hpp"
#include "pj_tf/transform.h"

namespace pj::tf {

using Duration = PJ::Duration;

// Why setTransform rejected a single edge. Every value is a recoverable *data*
// error in a bulk feed (a real bag can carry any of these), not a programmer
// error — callers count/log and continue rather than aborting the whole ingest.
enum class SetTransformError {
  // Child is already parented to a *different* frame. Deliberate divergence from
  // tf2, which lets the latest published parent win: we drop every post-reparent
  // edge for that child *forever* (the frame freezes at its last pre-reparent
  // pose), rather than start a new history. Surfaced only as an aggregate
  // caller-side drop counter, never per-edge in the UI — see L.102 in the
  // 2026-06-12 scene3D review for the rationale and trade-off.
  kReparentConflict,
  kSelfLoop,              // child == parent (a frame relative to itself)
  kInvalidFrameName,      // parent_frame or child_frame is empty
  kInvalidRotation,       // quaternion has non-finite components or |q|^2 < 1e-12
  kNonFiniteTranslation,  // translation has a non-finite (NaN/Inf) component
};

// Why a TF lookup failed. An enum (not a string) keeps render-loop misses
// allocation-free; the throwing wrapper maps it back to a message.
enum class LookupError {
  kUnknownSource,   // source frame not present in the buffer
  kUnknownTarget,   // target frame not present in the buffer
  kDisconnected,    // both frames known but no common ancestor (incl. broken cycles)
  kNoSampleAtTime,  // an edge on the connecting path has no sample at the requested time
};

// The time base a lookup is expressed in. Every sample carries two stamps: its own
// header stamp, and the entry time of the message that carried it (the dataset's
// store timestamp, e.g. MCAP log time). They differ by the transport latency, or
// entirely when header stamps use simulated time; with header == entry for every
// sample both clocks give identical results.
enum class Clock {
  // The cursor's clock: per edge, the newest-stamped sample that had arrived by `t`.
  // For anything addressed by a query time: what the recording knew at t. Which
  // consumer uses which clock: docs/derived_objects/ARCHITECTURE.md §12.
  kEntry,
  // The transforms' own stamps: per edge, the newest sample stamped at or before `t`.
  // For placing an object at its own header stamp ("where was the frame when this
  // was measured").
  kHeader,
};

// Case-insensitive (ASCII) less for frame names. Frame identity stays
// case-sensitive in the buffer; this is *display order* only. It is the single
// definition of user-facing frame ordering: getFrameHierarchy()'s sibling sort
// and any dock-level merge sort (e.g. the fixed-frame combo's clusters) must
// agree, so both call this.
[[nodiscard]] bool frameNameLess(std::string_view a, std::string_view b) noexcept;

struct FrameRow {
  std::string name;
  int depth = 0;

  friend bool operator==(const FrameRow& a, const FrameRow& b) noexcept {
    return a.depth == b.depth && a.name == b.name;
  }
  friend bool operator!=(const FrameRow& a, const FrameRow& b) noexcept {
    return !(a == b);
  }
};

class TransformBuffer;

// One immutable state of a TransformBuffer: its frames, edges and sample histories.
// Only TransformBuffer::snapshot() hands one out, in O(1): it shares the buffer's
// current state, and the buffer copies that state before its next write (copy-on-
// write), so a snapshot never changes, whatever happens to the buffer afterwards.
// THREADING: read it from any number of threads at once, with no lock; keep it as
// long as needed (it owns its state, independently of the buffer's lifetime).
class TransformSnapshot {
 public:
  // Same contract as TransformBuffer::tryLookupTransform, on this state.
  [[nodiscard]] PJ::Expected<Transform, LookupError> tryLookupTransform(
      const std::string& target, const std::string& source, TimePoint stamp, Clock clock) const;

  [[nodiscard]] std::vector<std::string> getAllFrames() const;

  // The TransformBuffer::revision() of this state: equal revisions mean equal
  // lookups, across buffers and buffer replacements, so it serves as a cache key.
  [[nodiscard]] uint64_t revision() const noexcept {
    return revision_;
  }

 private:
  friend class TransformBuffer;

  explicit TransformSnapshot(uint64_t revision) : revision_(revision) {}
  TransformSnapshot(const TransformSnapshot&) = default;

  // Defined in-header (not forward-declared) because std::unordered_map requires
  // a complete mapped_type at member-declaration time on libstdc++ (GCC 11);
  // only newer libstdc++ tolerates an incomplete one. Keeping these inline keeps
  // the buffer portable across the supported toolchains.
  struct EdgeHistory {
    struct Sample {
      TimePoint stamp;
      // Entry-clock key: the earliest entry time by which this sample or a
      // later-stamped one (stored, or skipped as a repeat) had arrived. A suffix
      // minimum of the entry times, so it is non-decreasing along `samples` like
      // `stamp`, and the newest-stamped sample known by t is the last one with
      // entry <= t. Equals the sample's own entry time while arrival order matches
      // stamp order.
      TimePoint entry;
      Transform value;
    };

    std::deque<Sample> samples;  // ascending stamp
    // Newest stamp / entry time received, including repeats setTransform skipped;
    // may exceed samples.back()'s.
    TimePoint newest_stamp{};
    TimePoint newest_entry{};

    // True once the edge has received a stamp later than its oldest stored one.
    bool receivedMultipleStamps() const {
      return !samples.empty() && newest_stamp > samples.front().stamp;
    }

    static TimePoint key(const Sample& sample, Clock clock) {
      return clock == Clock::kHeader ? sample.stamp : sample.entry;
    }

    // Heterogeneous comparator over `key`, for lower_bound / upper_bound on `samples`.
    struct KeyLess {
      Clock clock;
      bool operator()(const Sample& sample, TimePoint time) const {
        return key(sample, clock) < time;
      }
      bool operator()(TimePoint time, const Sample& sample) const {
        return time < key(sample, clock);
      }
    };
  };

  struct ParentLink {
    std::string parent;
    EdgeHistory history;
  };

  // One hop in a frame's chain toward its root. Carries the edge already resolved
  // by chainToRoot so lookup composition never re-hashes the same name (the
  // pointer is valid only while parents_ is unmodified). `link` is null for the
  // topmost (root) frame, which has no edge.
  struct ChainHop {
    std::string frame;
    const ParentLink* link;
  };

  // Index into a source/target chain where the two first meet, found by linear
  // scan (chains are short, so this beats per-lookup hash containers).
  struct MeetPoint {
    std::size_t src_k = 0;
    std::size_t tgt_k = 0;
    bool found = false;
  };

  std::unordered_map<std::string, ParentLink> parents_;
  uint64_t revision_;

  static std::optional<Transform> sampleAt(const EdgeHistory& history, TimePoint time, Clock clock);
  // Walk `frame` to its root, recording each hop's resolved edge into `out`
  // (cleared at entry). Cycle-safe: a repeated frame stops the walk so a
  // malformed tree fails cleanly. Caller-supplied output so hot lookups can
  // reuse scratch storage instead of heap-allocating a vector per call.
  void chainToRoot(const std::string& frame, std::vector<ChainHop>& out) const;
  // Lowest common ancestor of two chains (shared by lookup + latestCommonTime).
  static MeetPoint findCommonAncestor(const std::vector<ChainHop>& src, const std::vector<ChainHop>& tgt);
  // True if `frame` appears anywhere in the buffer (as a child or a parent).
  // Used only on the lookup-failure path to classify Unknown* vs Disconnected.
  bool isKnownFrame(const std::string& frame) const;
  // getAllFrames() into the caller's vector (cleared first).
  void framesInto(std::vector<std::string>& out) const;
};

class TransformBuffer {
 public:
  // Pass as the cache window to disable eviction entirely (keep all samples).
  // Use this for bulk-ingested, bounded sources (e.g. a loaded file): the whole
  // recording's TF is fed in up front, so a rolling window would trim every
  // dynamic edge to its tail and break lookups earlier in the timeline. The
  // finite default suits live streaming, where it bounds memory on a growing buffer.
  static constexpr Duration kKeepAll = Duration::max();

  explicit TransformBuffer(Duration cache_window = std::chrono::seconds(10));
  ~TransformBuffer();

  // Insert or replace the edge for `tf.child_frame`. A newer sample exactly equal
  // (component-wise, no tolerance) to the edge's last stored one is not stored:
  // zero-order hold makes it redundant. The edge still records its stamp and entry
  // time (see getLatestSample), which advance the eviction cutoffs, and an entry
  // time earlier than the stored sample's counts for it in the entry clock, so the
  // repeat bumps revision() only if that evicts something or moves the entry clock.
  //
  // Returns the rejected-edge reason instead of throwing, so a malformed edge in a bulk feed drops one
  // edge rather than aborting the whole load. There is no static/dynamic flag: a
  // transform published once (/tf_static, however it is namespaced) is just a
  // single-sample history that resolves at every later time via nearest-previous.
  // Validation order (each a recoverable SetTransformError, see the enum):
  // InvalidFrameName -> InvalidRotation -> NonFiniteTranslation -> SelfLoop ->
  // ReparentConflict. The name + rotation + translation checks run *before* the
  // self-loop / reparent checks (and before glm::normalize), so a zero-length or
  // NaN quaternion never reaches normalize and never poisons a lookup.
  //
  // `entry_time` is when the sample entered the dataset (Clock::kEntry); the
  // overload without it uses tf.stamp, so both clocks coincide.
  [[nodiscard]] PJ::Expected<void, SetTransformError> setTransform(const StampedTransform& tf, TimePoint entry_time);
  [[nodiscard]] PJ::Expected<void, SetTransformError> setTransform(const StampedTransform& tf);

  // Throwing lookup (tf2 ergonomics): the SE(3) target<-source transform at
  // `stamp` in `clock`, or throws std::runtime_error if unavailable. Thin wrapper
  // over tryLookupTransform.
  [[nodiscard]] Transform lookupTransform(
      const std::string& target, const std::string& source, TimePoint stamp, Clock clock) const;

  // Non-throwing lookup — the primary accessor. Returns the transform at `stamp`,
  // read in `clock`, or a LookupError reason; lookupTransform is sugar over it.
  [[nodiscard]] PJ::Expected<Transform, LookupError> tryLookupTransform(
      const std::string& target, const std::string& source, TimePoint stamp, Clock clock) const;

  // True iff `target` and `source` share a common ancestor in the forest, i.e. a
  // transform between them could be composed at *some* time. A pure connectivity
  // predicate (no time argument, no per-edge sample bound) — strictly cheaper
  // than latestCommonTime for callers that only need reachability.
  [[nodiscard]] bool areConnected(const std::string& target, const std::string& source) const;

  // The root of `frame`'s tree: its topmost ancestor, or `frame` itself when it has
  // no parent or is unknown. Two connected frames share it. In a parent cycle, the
  // last frame reached before the walk repeats.
  [[nodiscard]] std::string rootOf(const std::string& frame) const;

  // Newest time, in `clock`, at which every edge between the two frames has a
  // sample (the newest stamp or entry time each edge received; edges published at a
  // single stamp hold for all time, so they impose no bound). nullopt if the frames
  // are disconnected. CONTRACT SURPRISE: when the connecting path is
  // made up *entirely* of single-sample edges (the all-static case), the result
  // is an ENGAGED TimePoint{} (the Unix epoch sentinel), meaning "connected and
  // valid at any time at or after the newest single-sample stamp on the path" —
  // NOT nullopt. A caller that feeds the returned time straight into
  // tryLookupTransform may therefore get NoSampleAtTime on a pure-static path
  // (real stamps are all > epoch); callers wanting only reachability should use
  // areConnected() instead.
  [[nodiscard]] std::optional<TimePoint> latestCommonTime(
      const std::string& target, const std::string& source, Clock clock) const;

  // Union of every edge's stored sample times in `clock` (a repeat of the previous value is not stored) along the
  // connecting path between `target` and `source` (both chains up to their common ancestor), clipped to [lo, hi]
  // inclusive, sorted ascending and deduplicated into `out` (cleared first): the instants at which a lookup in `clock`
  // can change. Empty when either frame is unknown, the two are disconnected (same reachability rule as
  // tryLookupTransform), or target == source.
  // This is the natural sampling grid for a motion trail: a frame whose own
  // edge is static still moves whenever an edge higher up the chain does.
  void chainSampleTimes(
      const std::string& target, const std::string& source, TimePoint lo, TimePoint hi, Clock clock,
      std::vector<TimePoint>& out) const;

  [[nodiscard]] std::vector<std::string> getAllFrames() const;
  // Same result as getAllFrames(), but refills the caller's vector (cleared
  // first) so it can reuse capacity across calls — used by per-frame render
  // passes to avoid a heap allocation every paint. Same locking as the
  // by-value overload, which now delegates here.
  void getAllFrames(std::vector<std::string>& out) const;
  [[nodiscard]] std::optional<std::string> getParent(const std::string& child) const;
  // Newest header stamp the edge received (Clock::kHeader), including repeats
  // setTransform skipped.
  [[nodiscard]] std::optional<TimePoint> getLatestSample(const std::string& child) const;

  // Depth-annotated DFS pre-order of the TF forest. Roots and same-parent
  // siblings are alphabetically sorted; cycle-safe via visited set. Frames inside
  // a parent cycle (no reachable root) are still emitted at depth 0 so a broken
  // tree never vanishes silently from the fixed-frame combo.
  [[nodiscard]] std::vector<FrameRow> getFrameHierarchy() const;

  // Same forest as `getFrameHierarchy`, shaped as nested JSON:
  //   [ { "name": "odom", "children": [ { "name": "base_link", ... } ] }, ... ]
  // Intended for debug dumps, scripting integration, and layout-file diffs.
  [[nodiscard]] nlohmann::json getFrameHierarchyJson() const;

  // Replace the rolling cache window and immediately trim every edge to it using
  // the SAME rule as setTransform's eviction (per-edge, per-clock cutoff = newest
  // stamp or entry time - `window`; a sample goes only when neither clock still
  // needs it), always keeping at least the last sample per edge — that invariant
  // is what keeps a stopped / once-published (static) frame resolvable forever.
  // Pass kKeepAll to disable eviction (and drop nothing). Used to switch a buffer
  // from file (kKeepAll) to live-streaming (finite) retention without rebuilding.
  void setCacheWindow(Duration window);
  [[nodiscard]] Duration cacheWindow() const;

  // Monotonic change-detection token: bumps under the write lock on every change
  // to the stored samples or to what lookups resolve — an insert, a replace with a
  // different value, an earlier entry time, an eviction (during setTransform or
  // setCacheWindow), and clear(). A skipped repeat that changes none of these does
  // not bump, although getLatestSample / latestCommonTime can advance without one.
  // Lets a caller gate expensive recomputation (orphan state, hierarchy) on "did
  // the buffer change since I last looked?" without diffing. A bump does NOT imply
  // connectivity changed (an in-place sample replace bumps it too). Every value
  // comes from one process-wide counter, so no two buffers ever report the same
  // revision: a revision stays a valid cache key when the buffer is replaced.
  [[nodiscard]] uint64_t revision() const noexcept;

  // The current state, frozen, in O(1) and without copying: the snapshot shares
  // the state until the next write, which copies it first (once per snapshot, the
  // whole state: the cost of a snapshot of a mutating buffer, ARCHITECTURE.md §19).
  // A buffer that is not written again (a loaded file) is never copied.
  [[nodiscard]] std::shared_ptr<const TransformSnapshot> snapshot() const;

  // Empties the buffer by replacing its state with a new, empty one: holders of the
  // buffer see the reset, snapshots keep the state they hold.
  void clear();

  // Replaces this buffer's state with `other`'s current one, in O(1) and without a
  // copy (the first write to either buffer copies the then-shared state): holders of
  // this buffer see the replacement, snapshots keep theirs. Keeps this buffer's cache
  // window. A write to `other` after the call no longer reaches this buffer.
  void adoptStateOf(const TransformBuffer& other);

  // Counts the whole-state replacements, clear() and adoptStateOf(); a write never
  // moves it. A reader that extends a cached result when revision() moves (a trail
  // appending samples after its last stamp) rebuilds it when the epoch moves instead.
  [[nodiscard]] uint64_t epoch() const noexcept;

 private:
  // Strict-weak-order comparator for frame names as a std::set/std::map key.
  // Orders case-insensitively (matching frameNameLess, the user-facing display
  // order) but tie-breaks case-sensitively, so two names differing only by ASCII
  // case ("Lidar" vs "lidar") are distinct keys and neither is silently dropped
  // from the hierarchy — frame identity stays case-sensitive everywhere.
  struct FrameNameLess {
    bool operator()(const std::string& a, const std::string& b) const noexcept;
  };

  // parent -> case-insensitively-sorted children, plus the forest roots and the
  // full frame set. Built once per hierarchy call under the caller's shared_lock
  // so getFrameHierarchy and getFrameHierarchyJson share one root-detection +
  // sibling-ordering definition (they previously duplicated it verbatim).
  using ChildrenMap = std::unordered_map<std::string, std::set<std::string, FrameNameLess>>;
  using RootSet = std::set<std::string, FrameNameLess>;
  struct ForestIndex {
    ChildrenMap children;
    RootSet roots;
    std::unordered_set<std::string> all_frames;  // lets callers emit cycle members no root reaches
  };

  using EdgeHistory = TransformSnapshot::EdgeHistory;
  using ChainHop = TransformSnapshot::ChainHop;
  using MeetPoint = TransformSnapshot::MeetPoint;

  // The current state; every read goes through it under parents_mutex_. Written in
  // place only while no snapshot shares it (see writableLocked).
  std::shared_ptr<TransformSnapshot> current_;
  mutable std::shared_mutex parents_mutex_;
  Duration cache_window_;
  uint64_t epoch_ = 0;  // see epoch(); guarded by parents_mutex_

  // The current state, ready for a write: copied first when a snapshot shares it.
  // Caller holds the unique_lock.
  TransformSnapshot& writableLocked();
  // Makes samples[index] known by `entry_time`: lowers its entry key, and every
  // earlier key above it, to `entry_time` (keeping the suffix-minimum invariant).
  // Returns whether any key moved, i.e. whether entry-clock lookups changed.
  static bool knownBy(std::deque<EdgeHistory::Sample>& samples, std::size_t index, TimePoint entry_time);
  // Build the children/roots/all_frames index from the current state. Caller holds
  // the shared_lock. Roots are "frames never seen as a child"; a parent cycle yields
  // a frame with no reachable root, so callers DFS the roots first, then any
  // not-yet-visited frame at depth 0 to surface cycle members too.
  ForestIndex buildForestIndexLocked() const;
  // Trim the edge's samples to cache_window_ in place, relative to the newest
  // received stamp and entry time: per clock, keeps the newest sample at or before
  // that clock's cutoff (it holds the value valid there, however old it is) and
  // everything after, and always at least the last sample. No-op when
  // cache_window_ == kKeepAll. Returns whether it dropped anything. Caller holds
  // the unique_lock.
  bool evictEdgeLocked(EdgeHistory& history) const;
};

}  // namespace pj::tf
