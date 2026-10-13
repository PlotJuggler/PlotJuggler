#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
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
  // zero-order hold makes it redundant. The edge still records its stamp (see
  // getLatestSample), which advances the eviction cutoff, so the repeat bumps
  // revision() only if that evicts something.
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
  [[nodiscard]] PJ::Expected<void, SetTransformError> setTransform(const StampedTransform& tf);

  // Throwing lookup (tf2 ergonomics): the SE(3) target<-source transform at
  // `stamp`, or throws std::runtime_error if unavailable. Thin wrapper over
  // tryLookupTransform.
  [[nodiscard]] Transform lookupTransform(const std::string& target, const std::string& source, TimePoint stamp) const;

  // Non-throwing lookup — the primary accessor. Returns the transform or a
  // LookupError reason; lookupTransform is sugar over it.
  [[nodiscard]] PJ::Expected<Transform, LookupError> tryLookupTransform(
      const std::string& target, const std::string& source, TimePoint stamp) const;

  // True iff `target` and `source` share a common ancestor in the forest, i.e. a
  // transform between them could be composed at *some* time. A pure connectivity
  // predicate (no time argument, no per-edge sample bound) — strictly cheaper
  // than latestCommonTime for callers that only need reachability.
  [[nodiscard]] bool areConnected(const std::string& target, const std::string& source) const;

  // Newest time at which every edge between the two frames has a sample (see
  // getLatestSample; edges published at a single stamp hold for all time, so they
  // impose no bound). nullopt if the frames are disconnected. CONTRACT SURPRISE: when the connecting path is
  // made up *entirely* of single-sample edges (the all-static case), the result
  // is an ENGAGED TimePoint{} (the Unix epoch sentinel), meaning "connected and
  // valid at any time at or after the newest single-sample stamp on the path" —
  // NOT nullopt. A caller that feeds the returned time straight into
  // tryLookupTransform may therefore get NoSampleAtTime on a pure-static path
  // (real stamps are all > epoch); callers wanting only reachability should use
  // areConnected() instead.
  [[nodiscard]] std::optional<TimePoint> latestCommonTime(const std::string& target, const std::string& source) const;

  // Union of every edge's stored sample stamps (a repeat of the previous value is not stored) along the connecting path
  // between `target` and `source` (both chains up to their common ancestor), clipped to [lo, hi] inclusive, sorted
  // ascending and deduplicated into `out` (cleared first). Empty when either frame is unknown, the two are disconnected
  // (same reachability rule as tryLookupTransform), or target == source.
  // This is the natural sampling grid for a motion trail: a frame whose own
  // edge is static still moves whenever an edge higher up the chain does.
  void chainSampleTimes(
      const std::string& target, const std::string& source, TimePoint lo, TimePoint hi,
      std::vector<TimePoint>& out) const;

  [[nodiscard]] std::vector<std::string> getAllFrames() const;
  // Same result as getAllFrames(), but refills the caller's vector (cleared
  // first) so it can reuse capacity across calls — used by per-frame render
  // passes to avoid a heap allocation every paint. Same locking as the
  // by-value overload, which now delegates here.
  void getAllFrames(std::vector<std::string>& out) const;
  [[nodiscard]] std::optional<std::string> getParent(const std::string& child) const;
  // Newest stamp the edge received, including repeats setTransform skipped.
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
  // the SAME rule as setTransform's eviction (per-edge cutoff = newest stamp -
  // `window`), always keeping at least the last sample per edge — that invariant
  // is what keeps a stopped / once-published (static) frame resolvable forever.
  // Pass kKeepAll to disable eviction (and drop nothing). Used to switch a buffer
  // from file (kKeepAll) to live-streaming (finite) retention without rebuilding.
  void setCacheWindow(Duration window);
  [[nodiscard]] Duration cacheWindow() const;

  // Monotonic change-detection token: bumps under the write lock on every change
  // to the stored samples or to what lookups resolve — an insert, a replace with a
  // different value, an eviction during setTransform, and clear(). A skipped repeat
  // that evicts nothing does not bump, although getLatestSample / latestCommonTime
  // can advance without one; setCacheWindow does not bump either. Lets a caller
  // gate expensive recomputation (orphan state, hierarchy) on "did the buffer
  // change since I last looked?" without diffing. A bump does NOT imply
  // connectivity changed (an in-place sample replace bumps it too).
  [[nodiscard]] uint64_t revision() const noexcept;

  void clear();

 private:
  // Defined in-header (not forward-declared) because std::unordered_map requires
  // a complete mapped_type at member-declaration time on libstdc++ (GCC 11);
  // only newer libstdc++ tolerates an incomplete one. Keeping these inline keeps
  // the buffer portable across the supported toolchains.
  struct EdgeHistory {
    using Sample = std::pair<TimePoint, Transform>;

    std::deque<Sample> samples;
    // Newest stamp received, including repeats setTransform skipped; may exceed samples.back().first.
    TimePoint newest_stamp{};

    // True once the edge has received a stamp later than its oldest stored one.
    bool receivedMultipleStamps() const {
      return !samples.empty() && newest_stamp > samples.front().first;
    }

    static bool lessStamp(const Sample& sample, TimePoint stamp) {
      return sample.first < stamp;
    }
    static bool stampLess(TimePoint stamp, const Sample& sample) {
      return stamp < sample.first;
    }
  };

  struct ParentLink {
    std::string parent;
    EdgeHistory history;
  };

  // One hop in a frame's chain toward its root. Carries the edge already resolved
  // by chainToRoot so lookup composition never re-hashes the same name (the
  // pointer is valid only while the caller holds parents_mutex_ and parents_ is
  // unmodified). `link` is null for the topmost (root) frame, which has no edge.
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

  std::unordered_map<std::string, ParentLink> parents_;
  mutable std::shared_mutex parents_mutex_;
  Duration cache_window_;
  // Bumped on every change to stored samples or to what lookups resolve (see
  // revision()); guarded by parents_mutex_ (write side under unique_lock, read
  // side under shared_lock).
  uint64_t revision_ = 0;

  static std::optional<Transform> sampleAt(const EdgeHistory& h, TimePoint t);
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
  // Build the children/roots/all_frames index from parents_. Caller holds the
  // shared_lock. Roots are "frames never seen as a child"; a parent cycle yields
  // a frame with no reachable root, so callers DFS the roots first, then any
  // not-yet-visited frame at depth 0 to surface cycle members too.
  ForestIndex buildForestIndexLocked() const;
  // Trim the edge's samples to cache_window_ in place, relative to the newest
  // received stamp: keeps the newest sample at or before the cutoff (it holds the
  // value valid there, however old it is) and everything after, and always at least
  // the last sample. No-op when cache_window_ == kKeepAll. Returns whether it
  // dropped anything. Caller holds the unique_lock.
  bool evictEdgeLocked(EdgeHistory& history) const;
  PJ::Expected<Transform, LookupError> lookupTransformImpl(
      const std::string& target, const std::string& source, TimePoint stamp) const;
};

}  // namespace pj::tf
