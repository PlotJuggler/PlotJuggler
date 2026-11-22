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

// Why setTransform rejected a single edge. Each is a recoverable data error a real bag can carry: callers count
// and continue instead of aborting the ingest.
enum class SetTransformError {
  // Child is already parented to a different frame. Unlike tf2 (the latest parent wins), every later edge for that
  // child is dropped for good: the frame freezes at its last pre-reparent pose instead of starting a new history.
  kReparentConflict,
  kSelfLoop,              // child == parent (a frame relative to itself)
  kInvalidFrameName,      // parent_frame or child_frame is empty
  kInvalidRotation,       // quaternion has non-finite components or |q|^2 < 1e-12
  kNonFiniteTranslation,  // translation has a non-finite (NaN/Inf) component
};

// Why a TF lookup failed. An enum, not a string, keeps render-loop misses allocation-free.
enum class LookupError {
  kUnknownSource,   // source frame not present in the buffer
  kUnknownTarget,   // target frame not present in the buffer
  kDisconnected,    // both frames known but no common ancestor (incl. broken cycles)
  kNoSampleAtTime,  // an edge on the connecting path has no sample at the requested time
};

// The time base a lookup is expressed in. Every sample carries two stamps: its header stamp and the entry time of
// the message that carried it (the dataset's store timestamp, e.g. MCAP log time). They differ by the transport
// latency, or entirely under simulated-time headers; when header == entry for every sample the clocks agree.
// Which consumer uses which: docs/derived_objects/ARCHITECTURE.md "The two clocks".
enum class Clock {
  // The cursor's clock: per edge, the newest-stamped sample that had arrived by `t` (what the recording knew at t).
  // For anything addressed by a query time.
  kEntry,
  // The transforms' own stamps: per edge, the newest sample stamped at or before `t`. For placing an object at its
  // own header stamp.
  kHeader,
};

// How a lookup reads an edge at a time between two of its samples. Before the first sample there is none either way;
// at or past the last one, the last.
enum class Sampling {
  // The earlier sample (zero-order hold): what the recording held. The default, and what scripts read.
  kHold,
  // Between the two: translation linearly, rotation by slerp. A value repeated over a span is held over that span
  // and interpolation starts at its last repeat.
  kInterpolate,
};

// Case-insensitive (ASCII) less for frame names: display order only, frame identity stays case-sensitive. The one
// definition of user-facing frame ordering; getFrameHierarchy()'s sibling sort and any dock-level merge sort call it.
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
      const std::string& target, const std::string& source, TimePoint stamp, Clock clock,
      Sampling sampling = Sampling::kHold) const;

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

  // Defined here, not forward-declared: libstdc++ 11 (GCC 11) needs a complete mapped_type for std::unordered_map
  // members.
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

  // One hop in a frame's chain toward its root, with the edge chainToRoot already resolved (valid only while
  // parents_ is unmodified). `link` is null for the topmost frame, which has no edge.
  struct ChainHop {
    std::string frame;
    const ParentLink* link;
  };

  // Index into a source/target chain where the two first meet.
  struct MeetPoint {
    std::size_t src_k = 0;
    std::size_t tgt_k = 0;
    bool found = false;
  };

  std::unordered_map<std::string, ParentLink> parents_;
  uint64_t revision_;

  static std::optional<Transform> sampleAt(const EdgeHistory& history, TimePoint time, Clock clock, Sampling sampling);
  // Walk `frame` to its root into `out` (cleared first, so hot lookups reuse scratch storage). A repeated frame
  // stops the walk, so a cyclic tree fails the lookup instead of spinning.
  void chainToRoot(const std::string& frame, std::vector<ChainHop>& out) const;
  // Lowest common ancestor of two chains.
  static MeetPoint findCommonAncestor(const std::vector<ChainHop>& src, const std::vector<ChainHop>& tgt);
  // True if `frame` appears anywhere in the buffer (as a child or a parent). O(n): only for classifying a failed
  // lookup as Unknown* vs Disconnected.
  bool isKnownFrame(const std::string& frame) const;
  void framesInto(std::vector<std::string>& out) const;
};

class TransformBuffer {
 public:
  // Cache window that disables eviction. For bulk-ingested bounded sources (a loaded file): the whole recording is
  // fed up front, so a rolling window would trim every dynamic edge to its tail and break lookups earlier in the
  // timeline. The finite default suits live streaming, where it bounds memory.
  static constexpr Duration kKeepAll = Duration::max();

  explicit TransformBuffer(Duration cache_window = std::chrono::seconds(10));
  ~TransformBuffer();

  // Insert or replace the edge for `tf.child_frame`; the rotation is normalized on write. A newer sample exactly
  // equal (component-wise) to the edge's last stored one is not stored (zero-order hold makes it redundant; the
  // last one of a run is stored once the value changes, where an interpolating lookup starts moving), but
  // the edge still records its stamp and entry time (see getLatestSample), which advance the eviction cutoffs, and
  // an earlier entry time counts for the stored sample in the entry clock. Such a repeat bumps revision() only if
  // that evicts something or moves the entry clock.
  //
  // A rejected edge returns its reason, never throws. Validation order: InvalidFrameName, InvalidRotation,
  // NonFiniteTranslation, SelfLoop, ReparentConflict; the first three run before glm::normalize, so a zero-length
  // or NaN quaternion never poisons a lookup. There is no static/dynamic flag: a transform published once
  // (/tf_static, however namespaced) is a single-sample history that resolves at every later time.
  //
  // `entry_time` is when the sample entered the dataset (Clock::kEntry); the overload without it uses tf.stamp.
  [[nodiscard]] PJ::Expected<void, SetTransformError> setTransform(const StampedTransform& tf, TimePoint entry_time);
  [[nodiscard]] PJ::Expected<void, SetTransformError> setTransform(const StampedTransform& tf);

  // The SE(3) target<-source transform at `stamp` in `clock`; throws std::runtime_error when unavailable. Wrapper
  // over tryLookupTransform.
  [[nodiscard]] Transform lookupTransform(
      const std::string& target, const std::string& source, TimePoint stamp, Clock clock,
      Sampling sampling = Sampling::kHold) const;

  // The transform at `stamp`, read in `clock`, or the LookupError reason.
  [[nodiscard]] PJ::Expected<Transform, LookupError> tryLookupTransform(
      const std::string& target, const std::string& source, TimePoint stamp, Clock clock,
      Sampling sampling = Sampling::kHold) const;

  // True iff `target` and `source` share a common ancestor, i.e. a transform between them could be composed at some
  // time. Connectivity only, cheaper than latestCommonTime.
  [[nodiscard]] bool areConnected(const std::string& target, const std::string& source) const;

  // The nearest frame both `a` and `b` descend from (either of them when one is the other's ancestor, `a` when they
  // are the same frame), or nullopt when they are not connected.
  [[nodiscard]] std::optional<std::string> commonAncestor(const std::string& a, const std::string& b) const;

  // Newest time, in `clock`, at which every edge between the two frames has a sample (the newest stamp or entry
  // time each edge received; single-stamp edges hold for all time and impose no bound). nullopt if disconnected.
  // A path made up entirely of single-sample edges (all static) returns an engaged TimePoint{} (the Unix epoch),
  // not nullopt: "connected, valid from the newest single-sample stamp on". Feeding that straight into
  // tryLookupTransform can fail with kNoSampleAtTime (real stamps are > epoch); use areConnected() for
  // reachability.
  [[nodiscard]] std::optional<TimePoint> latestCommonTime(
      const std::string& target, const std::string& source, Clock clock) const;

  // Union of every edge's stored sample times in `clock` along the connecting path (both chains up to their common
  // ancestor), clipped to [lo, hi] inclusive, sorted and deduplicated into `out` (cleared first): the instants at
  // which a lookup in `clock` can change, i.e. the sampling grid for a motion trail (a static edge's frame still
  // moves when an edge higher up the chain does). Empty when either frame is unknown, the two are disconnected, or
  // target == source.
  void chainSampleTimes(
      const std::string& target, const std::string& source, TimePoint lo, TimePoint hi, Clock clock,
      std::vector<TimePoint>& out) const;

  [[nodiscard]] std::vector<std::string> getAllFrames() const;
  // As getAllFrames(), into the caller's vector (cleared first) so per-frame render passes reuse its capacity.
  void getAllFrames(std::vector<std::string>& out) const;
  [[nodiscard]] std::optional<std::string> getParent(const std::string& child) const;
  // Newest header stamp the edge received (Clock::kHeader), including repeats
  // setTransform skipped.
  [[nodiscard]] std::optional<TimePoint> getLatestSample(const std::string& child) const;

  // Depth-annotated DFS pre-order of the TF forest; roots and siblings sorted by frameNameLess. Frames inside a
  // parent cycle (no reachable root) are emitted at depth 0 so a broken tree does not vanish from the fixed-frame
  // combo.
  [[nodiscard]] std::vector<FrameRow> getFrameHierarchy() const;

  // Same forest as getFrameHierarchy, as nested JSON:
  //   [ { "name": "odom", "children": [ { "name": "base_link", ... } ] }, ... ]
  [[nodiscard]] nlohmann::json getFrameHierarchyJson() const;

  // Replaces the rolling cache window and trims every edge to it with setTransform's eviction rule (per edge and
  // clock, cutoff = newest stamp or entry time - `window`; a sample goes only when neither clock still needs it).
  // The last sample of an edge is always kept, so a stopped or once-published frame stays resolvable. kKeepAll
  // drops nothing.
  void setCacheWindow(Duration window);
  [[nodiscard]] Duration cacheWindow() const;

  // Change-detection token, bumped under the write lock on every change to the stored samples or to what lookups
  // resolve: an insert, a replace with a different value, an earlier entry time, an eviction (during setTransform
  // or setCacheWindow) and clear(). A skipped repeat that changes none of these does not bump, although
  // getLatestSample / latestCommonTime can advance without one. A bump does not imply connectivity changed. Values
  // come from one process-wide counter, so no two buffers report the same revision and it stays a valid cache key
  // when a buffer is replaced.
  [[nodiscard]] uint64_t revision() const noexcept;

  // The current state, frozen, in O(1): it is shared until the next write, which copies the whole state first
  // (once per snapshot; the cost is in docs/derived_objects/ARCHITECTURE.md "Reference numbers"). A buffer that is
  // not written again (a loaded file) is never copied.
  [[nodiscard]] std::shared_ptr<const TransformSnapshot> snapshot() const;

  // Replaces the state with an empty one: holders of the buffer see the reset, snapshots keep what they hold.
  void clear();

  // Replaces this buffer's state with `other`'s current one in O(1) (the first write to either buffer copies the
  // then-shared state): holders of this buffer see the replacement, snapshots keep theirs. Keeps this buffer's
  // cache window; later writes to `other` do not reach this buffer.
  void adoptStateOf(const TransformBuffer& other);

  // Counts whole-state replacements (clear(), adoptStateOf()); a write never moves it. A reader that extends a
  // cached result when revision() moves (a trail appending samples) rebuilds it when the epoch moves.
  [[nodiscard]] uint64_t epoch() const noexcept;

 private:
  // Set/map key order: frameNameLess with a case-sensitive tie-break, so "Lidar" and "lidar" stay distinct keys
  // and neither drops out of the hierarchy.
  struct FrameNameLess {
    bool operator()(const std::string& a, const std::string& b) const noexcept;
  };

  // parent -> sorted children, the forest roots and the full frame set, shared by getFrameHierarchy and
  // getFrameHierarchyJson.
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

  // The current state; read under parents_mutex_, written in place only while no snapshot shares it.
  std::shared_ptr<TransformSnapshot> current_;
  mutable std::shared_mutex parents_mutex_;
  Duration cache_window_;
  uint64_t epoch_ = 0;  // guarded by parents_mutex_

  // The current state, copied first when a snapshot shares it. Caller holds the unique_lock.
  TransformSnapshot& writableLocked();
  // Lowers samples[index]'s entry key, and every earlier key above it, to `entry_time` (the suffix-minimum
  // invariant). Returns whether any key moved, i.e. whether entry-clock lookups changed.
  static bool knownBy(std::deque<EdgeHistory::Sample>& samples, std::size_t index, TimePoint entry_time);
  // Caller holds the shared_lock. Roots are frames never seen as a child, so a parent cycle has no reachable root:
  // callers DFS the roots, then every unvisited frame at depth 0 to surface cycle members.
  ForestIndex buildForestIndexLocked() const;
  // Trims the edge to cache_window_ relative to the newest received stamp and entry time: per clock, keeps the
  // newest sample at or before the cutoff (it holds the value valid there, however old) and everything after, and
  // always the last sample. No-op for kKeepAll. Returns whether it dropped anything. Caller holds the unique_lock.
  bool evictEdgeLocked(EdgeHistory& history) const;
};

}  // namespace pj::tf
