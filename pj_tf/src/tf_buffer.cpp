// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_tf/tf_buffer.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace pj::tf {

// EdgeHistory and ParentLink are defined in tf_buffer.h (required complete by
// std::unordered_map on GCC 11's libstdc++).

namespace {

// One counter for every buffer in the process, so a revision names one state.
uint64_t nextRevision() noexcept {
  static std::atomic<uint64_t> counter{0};
  return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool isFinite(const glm::dvec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool isFinite(const glm::dquat& q) {
  return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
}

}  // namespace

TransformBuffer::TransformBuffer(Duration cache_window)
    : current_(new TransformSnapshot(nextRevision())), cache_window_(cache_window) {}

TransformBuffer::~TransformBuffer() = default;

TransformSnapshot& TransformBuffer::writableLocked() {
  // Owners are added by snapshot() and adoptStateOf() under the shared lock, so they
  // cannot appear while we hold the unique lock, and by TransformService's weak
  // cache (weak_ptr::lock) with no lock at all: that one reads the state only after
  // taking this buffer's shared lock, i.e. after an in-place write that missed its
  // new owner has finished, and every later write sees the owner and copies. A count
  // that falls meanwhile only costs a needless copy.
  if (current_.use_count() > 1) {
    current_.reset(new TransformSnapshot(*current_));
  } else {
    // use_count() is a relaxed load: order the last other owner's release (its
    // reads of the state) before our in-place writes.
    std::atomic_thread_fence(std::memory_order_acquire);
  }
  return *current_;
}

PJ::Expected<void, SetTransformError> TransformBuffer::setTransform(const StampedTransform& tf) {
  return setTransform(tf, tf.stamp);
}

PJ::Expected<void, SetTransformError> TransformBuffer::setTransform(const StampedTransform& tf, TimePoint entry_time) {
  std::unique_lock lock(parents_mutex_);

  // Validate the wire data BEFORE the structural checks (and before
  // glm::normalize): an empty name, a zero-length / NaN quaternion, or a
  // non-finite translation is a recoverable data error a real bag can carry, and
  // a zero-length quaternion normalized to all-NaN would otherwise be stored and
  // silently poison every lookup composing through that edge.
  if (tf.parent_frame.empty() || tf.child_frame.empty()) {
    return PJ::unexpected(SetTransformError::kInvalidFrameName);
  }
  // glm::dot of a quaternion is w*w + x*x + y*y + z*z (the squared norm). Reject
  // a degenerate quaternion before normalize divides by (near-)zero.
  if (!isFinite(tf.transform.q) || glm::dot(tf.transform.q, tf.transform.q) < 1e-12) {
    return PJ::unexpected(SetTransformError::kInvalidRotation);
  }
  if (!isFinite(tf.transform.t)) {
    return PJ::unexpected(SetTransformError::kNonFiniteTranslation);
  }

  if (tf.child_frame == tf.parent_frame) {
    // A frame relative to itself is the identity; storing it would create a
    // self-loop in the parent map and hang chainToRoot. Drop it (recoverable).
    return PJ::unexpected(SetTransformError::kSelfLoop);
  }

  TransformSnapshot& state = writableLocked();
  auto& link = state.parents_[tf.child_frame];
  // The empty-name reject above makes this sentinel sound: link.parent is empty
  // ONLY for a freshly default-constructed link (never for a real edge), so an
  // empty parent unambiguously means "no link yet", not "child<-empty parent".
  if (!link.parent.empty() && link.parent != tf.parent_frame) {
    // A second publisher claims this child under a different parent. Drop this
    // one edge rather than aborting the whole bulk ingest.
    return PJ::unexpected(SetTransformError::kReparentConflict);
  }

  link.parent = tf.parent_frame;

  // No static/dynamic distinction: every edge is a time-ordered history resolved
  // by nearest-previous (see sampleAt). A transform published once — /tf_static,
  // however namespaced — is simply a single-sample history that holds for all
  // later query times, so static-ness never has to be inferred from a topic name.
  const Transform normalized{tf.transform.t, glm::normalize(tf.transform.q)};
  auto& history = link.history;
  auto& samples = history.samples;
  const bool first_sample = samples.empty();
  // A fresh sample enters with no entry key; knownBy() then sets it and restores the
  // suffix minimum over the earlier samples.
  constexpr TimePoint kNotYetKnown = TimePoint::max();
  bool changed = true;
  if (first_sample || tf.stamp > samples.back().stamp) {
    // Exact under zero-order hold for in-order arrival, in both clocks: a skipped
    // repeat's entry time is folded into the stored sample it repeats. Accepted
    // trade-off: an out-of-order sample landing inside a run of skipped repeats
    // keeps its value until the next stored sample, where the full history would
    // have returned the repeated value again (rare; exact handling would need the
    // skipped stamps).
    if (!first_sample && samples.back().value == normalized) {
      changed = knownBy(samples, samples.size() - 1, entry_time);
    } else {
      samples.push_back({tf.stamp, kNotYetKnown, normalized});
      (void)knownBy(samples, samples.size() - 1, entry_time);
    }
  } else {
    // Not past the back, so `it` is a stored sample (same stamp or the next newer).
    auto it = std::lower_bound(samples.begin(), samples.end(), tf.stamp, EdgeHistory::KeyLess{Clock::kHeader});
    const auto index = static_cast<std::size_t>(it - samples.begin());
    if (it->stamp == tf.stamp) {
      // Same stamp: the new value applies from the sample's earliest entry key on
      // (the entry clock cannot hold two values for one stamp).
      changed = it->value != normalized;
      it->value = normalized;
      changed = knownBy(samples, index, entry_time) || changed;
    } else {
      // `it` is the next-newer sample, which bounds the new key (suffix minimum).
      const TimePoint key = std::min(entry_time, it->entry);
      samples.insert(it, {tf.stamp, kNotYetKnown, normalized});
      (void)knownBy(samples, index, key);
    }
  }

  history.newest_stamp = first_sample ? tf.stamp : std::max(history.newest_stamp, tf.stamp);
  history.newest_entry = first_sample ? entry_time : std::max(history.newest_entry, entry_time);
  // A skipped repeat still advances the newest stamp and entry time and can evict,
  // which changes what lookups resolve, so eviction alone also counts as a change.
  const bool evicted = evictEdgeLocked(history);
  if (changed || evicted) {
    state.revision_ = nextRevision();
  }
  return {};
}

bool TransformBuffer::knownBy(std::deque<EdgeHistory::Sample>& samples, std::size_t index, TimePoint entry_time) {
  bool lowered = false;
  // Earlier keys are non-decreasing, so the walk stops at the first one already known.
  for (std::size_t position = index + 1; position-- > 0 && samples[position].entry > entry_time;) {
    samples[position].entry = entry_time;
    lowered = true;
  }
  return lowered;
}

bool TransformBuffer::evictEdgeLocked(EdgeHistory& history) const {
  auto& samples = history.samples;
  // kKeepAll disables eviction: bulk-ingested bounded sources (a loaded file)
  // feed the whole recording up front, so a rolling window would trim every
  // edge to its tail and break lookups earlier in the timeline. Under a finite
  // window (live streaming) `samples.size() > 1` pins the most recent sample —
  // an edge is NEVER evicted to empty, so a slow / stopped / once-published frame
  // keeps resolving forever instead of orphaning. That "always keep the last
  // sample" guarantee is what lets us drop the static flag: it gives static-like
  // persistence to every edge uniformly. (The current cutoff is per-edge-relative
  // so the guard is presently belt-and-suspenders, but it pins the invariant
  // should eviction ever switch to a global clock.)
  //
  // The front goes only once its successor is at or before the cutoff in BOTH
  // clocks: the newest sample at or before a clock's cutoff holds the value valid
  // there, and with repeats skipped it may be far older than the cutoff. The entry
  // cutoff keeps the cursor's live window (the store retains by entry time); the
  // header cutoff keeps recent objects' own stamps resolvable.
  if (samples.empty() || cache_window_ >= kKeepAll) {
    return false;
  }
  const auto header_cutoff = history.newest_stamp - cache_window_;
  const auto entry_cutoff = history.newest_entry - cache_window_;
  bool evicted = false;
  while (samples.size() > 1 && samples[1].stamp <= header_cutoff && samples[1].entry <= entry_cutoff) {
    samples.pop_front();
    evicted = true;
  }
  return evicted;
}

std::optional<Transform> TransformSnapshot::sampleAt(const EdgeHistory& history, TimePoint time, Clock clock) {
  if (history.samples.empty()) {
    return std::nullopt;
  }
  // Nearest-previous (zero-order hold) in `clock`: the last sample whose key is at
  // or before `time` (both keys are non-decreasing along the deque). A single-sample
  // edge therefore resolves at every time >= its key. A query strictly before the
  // first key has no value yet (the frame had not been announced, or had not
  // arrived, at that time) -> nullopt.
  auto hi = std::upper_bound(history.samples.begin(), history.samples.end(), time, EdgeHistory::KeyLess{clock});
  if (hi == history.samples.begin()) {
    return std::nullopt;
  }
  return std::prev(hi)->value;
}

void TransformSnapshot::chainToRoot(const std::string& frame, std::vector<ChainHop>& out) const {
  out.clear();
  out.push_back(ChainHop{frame, nullptr});
  for (std::string cur = frame;;) {
    auto it = parents_.find(cur);
    if (it == parents_.end()) {
      break;  // `cur` is a root (or unknown): no outgoing edge.
    }
    // Record the resolved edge on the hop we just added, then advance to the
    // parent — so lookup composition reuses it instead of re-hashing `cur`.
    out.back().link = &it->second;
    cur = it->second.parent;
    // Cycle guard: a repeated frame means a malformed tree (e.g. A→B→A, which
    // the reparent guard does not catch). Stop so the lookup fails cleanly
    // instead of spinning. Linear scan — chains are short, so this beats a
    // per-lookup hash set.
    const bool seen = std::any_of(out.begin(), out.end(), [&cur](const ChainHop& hop) { return hop.frame == cur; });
    if (seen) {
      break;
    }
    out.push_back(ChainHop{cur, nullptr});
  }
}

TransformSnapshot::MeetPoint TransformSnapshot::findCommonAncestor(
    const std::vector<ChainHop>& src, const std::vector<ChainHop>& tgt) {
  // Lowest common ancestor: the first src frame (walking up from source) that
  // also appears in tgt. Linear scan — chains are short, so this is cheaper than
  // building a hash index on every lookup.
  for (std::size_t i = 0; i < src.size(); ++i) {
    for (std::size_t j = 0; j < tgt.size(); ++j) {
      if (src[i].frame == tgt[j].frame) {
        return MeetPoint{i, j, true};
      }
    }
  }
  return MeetPoint{};
}

bool TransformSnapshot::isKnownFrame(const std::string& frame) const {
  if (parents_.find(frame) != parents_.end()) {
    return true;  // appears as a child
  }
  for (const auto& [child, link] : parents_) {
    if (link.parent == frame) {
      return true;  // appears as a parent (a root frame)
    }
  }
  return false;
}

PJ::Expected<Transform, LookupError> TransformSnapshot::tryLookupTransform(
    const std::string& target, const std::string& source, TimePoint stamp, Clock clock) const {
  if (target == source) {
    return Transform::identity();
  }

  // Reuse per-thread scratch instead of heap-allocating two vectors per lookup
  // (this is the per-frame, per-link render hot path). Safe: chainToRoot results
  // never escape this call, and lookups never recurse, so the two buffers can't
  // be aliased mid-walk.
  static thread_local std::vector<ChainHop> tgt_chain;
  static thread_local std::vector<ChainHop> src_chain;
  chainToRoot(target, tgt_chain);
  chainToRoot(source, src_chain);

  const MeetPoint meet = findCommonAncestor(src_chain, tgt_chain);
  if (!meet.found) {
    // Distinguish "frame not in the buffer" from "known but disconnected". The
    // O(n) scans run only on this (rare) failure path, never in the steady loop.
    if (!isKnownFrame(source)) {
      return PJ::unexpected(LookupError::kUnknownSource);
    }
    if (!isKnownFrame(target)) {
      return PJ::unexpected(LookupError::kUnknownTarget);
    }
    return PJ::unexpected(LookupError::kDisconnected);
  }

  // Compose source->common, then common->target inverse. Every hop below the
  // common ancestor carries a non-null edge (only the root hop is null).
  Transform t_common_from_source = Transform::identity();
  for (std::size_t i = 0; i < meet.src_k; ++i) {
    const auto sample = sampleAt(src_chain[i].link->history, stamp, clock);
    if (!sample) {
      return PJ::unexpected(LookupError::kNoSampleAtTime);
    }
    t_common_from_source = (*sample) * t_common_from_source;
  }

  Transform t_common_from_target = Transform::identity();
  for (std::size_t i = 0; i < meet.tgt_k; ++i) {
    const auto sample = sampleAt(tgt_chain[i].link->history, stamp, clock);
    if (!sample) {
      return PJ::unexpected(LookupError::kNoSampleAtTime);
    }
    t_common_from_target = (*sample) * t_common_from_target;
  }

  return t_common_from_target.inverse() * t_common_from_source;
}

Transform TransformBuffer::lookupTransform(
    const std::string& target, const std::string& source, TimePoint stamp, Clock clock) const {
  // Thin wrapper: delegate to the non-throwing accessor (which takes the lock)
  // and surface the LookupError as a message for tf2-style callers.
  auto result = tryLookupTransform(target, source, stamp, clock);
  if (!result) {
    throw std::runtime_error("pj_scene3d: no transform from '" + source + "' to '" + target + "' at requested time");
  }
  return *result;
}

PJ::Expected<Transform, LookupError> TransformBuffer::tryLookupTransform(
    const std::string& target, const std::string& source, TimePoint stamp, Clock clock) const {
  std::shared_lock lock(parents_mutex_);
  return current_->tryLookupTransform(target, source, stamp, clock);
}

bool TransformBuffer::areConnected(const std::string& target, const std::string& source) const {
  if (target == source) {
    return true;
  }
  std::shared_lock lock(parents_mutex_);
  static thread_local std::vector<ChainHop> tgt_chain;
  static thread_local std::vector<ChainHop> src_chain;
  current_->chainToRoot(target, tgt_chain);
  current_->chainToRoot(source, src_chain);
  return TransformSnapshot::findCommonAncestor(src_chain, tgt_chain).found;
}

std::string TransformBuffer::rootOf(const std::string& frame) const {
  std::shared_lock lock(parents_mutex_);
  static thread_local std::vector<ChainHop> chain;
  current_->chainToRoot(frame, chain);
  return chain.back().frame;
}

std::optional<TimePoint> TransformBuffer::latestCommonTime(
    const std::string& target, const std::string& source, Clock clock) const {
  std::shared_lock lock(parents_mutex_);

  static thread_local std::vector<ChainHop> tgt_chain;
  static thread_local std::vector<ChainHop> src_chain;
  current_->chainToRoot(target, tgt_chain);
  current_->chainToRoot(source, src_chain);

  const MeetPoint meet = TransformSnapshot::findCommonAncestor(src_chain, tgt_chain);
  if (!meet.found) {
    return std::nullopt;
  }

  // Newest time bounded by every edge that received more than one stamp. Edges
  // published at a single stamp (the static case) hold for all time, so they
  // impose no upper bound.
  auto walk = [clock](const std::vector<ChainHop>& chain, std::size_t k, std::optional<TimePoint>& acc) {
    for (std::size_t i = 0; i < k; ++i) {
      const auto& edge = chain[i].link->history;
      if (!edge.receivedMultipleStamps()) {
        continue;
      }
      const TimePoint newest = clock == Clock::kHeader ? edge.newest_stamp : edge.newest_entry;
      acc = acc ? std::min(*acc, newest) : newest;
    }
  };

  std::optional<TimePoint> result;
  walk(src_chain, meet.src_k, result);
  walk(tgt_chain, meet.tgt_k, result);
  return result.value_or(TimePoint{});
}

void TransformBuffer::chainSampleTimes(
    const std::string& target, const std::string& source, TimePoint lo, TimePoint hi, Clock clock,
    std::vector<TimePoint>& out) const {
  out.clear();
  if (lo > hi) {
    return;
  }
  std::shared_lock lock(parents_mutex_);
  static thread_local std::vector<ChainHop> tgt_chain;
  static thread_local std::vector<ChainHop> src_chain;
  current_->chainToRoot(target, tgt_chain);
  current_->chainToRoot(source, src_chain);
  const MeetPoint meet = TransformSnapshot::findCommonAncestor(src_chain, tgt_chain);
  if (!meet.found) {
    return;
  }
  const auto append_edge_stamps = [&out, lo, hi, clock](const std::vector<ChainHop>& chain, std::size_t hops) {
    for (std::size_t hop = 0; hop < hops; ++hop) {
      const auto& samples = chain[hop].link->history.samples;
      auto first = std::lower_bound(samples.begin(), samples.end(), lo, EdgeHistory::KeyLess{clock});
      const auto last = std::upper_bound(samples.begin(), samples.end(), hi, EdgeHistory::KeyLess{clock});
      for (; first != last; ++first) {
        out.push_back(EdgeHistory::key(*first, clock));
      }
    }
  };
  append_edge_stamps(src_chain, meet.src_k);
  append_edge_stamps(tgt_chain, meet.tgt_k);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
}

std::vector<std::string> TransformBuffer::getAllFrames() const {
  std::vector<std::string> out;
  getAllFrames(out);
  return out;
}

void TransformBuffer::getAllFrames(std::vector<std::string>& out) const {
  std::shared_lock lock(parents_mutex_);
  current_->framesInto(out);
}

std::vector<std::string> TransformSnapshot::getAllFrames() const {
  std::vector<std::string> out;
  framesInto(out);
  return out;
}

void TransformSnapshot::framesInto(std::vector<std::string>& out) const {
  std::unordered_set<std::string> frames;
  frames.reserve(parents_.size() * 2U);
  for (const auto& [child, link] : parents_) {
    frames.insert(child);
    frames.insert(link.parent);
  }

  out.clear();
  out.insert(out.end(), frames.begin(), frames.end());
}

std::optional<std::string> TransformBuffer::getParent(const std::string& child) const {
  std::shared_lock lock(parents_mutex_);

  const auto it = current_->parents_.find(child);
  if (it == current_->parents_.end()) {
    return std::nullopt;
  }
  return it->second.parent;
}

std::optional<TimePoint> TransformBuffer::getLatestSample(const std::string& child) const {
  std::shared_lock lock(parents_mutex_);

  const auto it = current_->parents_.find(child);
  if (it == current_->parents_.end()) {
    return std::nullopt;
  }

  const auto& history = it->second.history;
  if (history.samples.empty()) {
    return std::nullopt;
  }
  return history.newest_stamp;
}

void TransformBuffer::clear() {
  std::unique_lock lock(parents_mutex_);
  current_.reset(new TransformSnapshot(nextRevision()));
  ++epoch_;
}

void TransformBuffer::setCacheWindow(Duration window) {
  std::unique_lock lock(parents_mutex_);
  cache_window_ = window;
  if (window >= kKeepAll) {
    return;  // nothing to evict, and no state to copy for it
  }
  // Apply the new window retroactively so a buffer switched from kKeepAll (file)
  // to a finite window (live) is trimmed immediately, not only on the next write
  // per edge. evictEdgeLocked keeps the last sample per edge, so static frames
  // stay resolvable.
  TransformSnapshot& state = writableLocked();
  bool evicted = false;
  for (auto& [child, link] : state.parents_) {
    evicted = evictEdgeLocked(link.history) || evicted;
  }
  if (evicted) {
    state.revision_ = nextRevision();
  }
}

Duration TransformBuffer::cacheWindow() const {
  std::shared_lock lock(parents_mutex_);
  return cache_window_;
}

uint64_t TransformBuffer::revision() const noexcept {
  std::shared_lock lock(parents_mutex_);
  return current_->revision_;
}

void TransformBuffer::adoptStateOf(const TransformBuffer& other) {
  if (&other == this) {
    return;
  }
  std::unique_lock mine(parents_mutex_, std::defer_lock);
  std::shared_lock theirs(other.parents_mutex_, std::defer_lock);
  std::lock(mine, theirs);
  current_ = other.current_;
  ++epoch_;
}

uint64_t TransformBuffer::epoch() const noexcept {
  std::shared_lock lock(parents_mutex_);
  return epoch_;
}

std::shared_ptr<const TransformSnapshot> TransformBuffer::snapshot() const {
  std::shared_lock lock(parents_mutex_);
  return current_;
}

bool TransformBuffer::FrameNameLess::operator()(const std::string& a, const std::string& b) const noexcept {
  // Case-insensitive display order with a case-sensitive tie-break: still a
  // strict weak order, but two names equal under frameNameLess ("Lidar" vs
  // "lidar") stay DISTINCT keys, so neither collapses out of a std::set the way
  // a pure case-insensitive comparator would (it treats equivalent keys as
  // duplicates). Frame identity is case-sensitive everywhere else; this keeps
  // the hierarchy consistent with getAllFrames/lookups.
  if (frameNameLess(a, b)) {
    return true;
  }
  if (frameNameLess(b, a)) {
    return false;
  }
  return a < b;
}

TransformBuffer::ForestIndex TransformBuffer::buildForestIndexLocked() const {
  ForestIndex index;
  std::unordered_set<std::string> non_root;
  for (const auto& [child, link] : current_->parents_) {
    index.children[link.parent].insert(child);
    index.all_frames.insert(child);
    index.all_frames.insert(link.parent);
    non_root.insert(child);
  }
  for (const auto& frame : index.all_frames) {
    if (non_root.count(frame) == 0) {
      index.roots.insert(frame);
    }
  }
  return index;
}

std::vector<FrameRow> TransformBuffer::getFrameHierarchy() const {
  std::shared_lock lock(parents_mutex_);
  const ForestIndex index = buildForestIndexLocked();

  std::vector<FrameRow> rows;
  rows.reserve(index.all_frames.size());
  std::unordered_set<std::string> visited;

  auto dfs = [&](auto& self, const std::string& name, int depth) -> void {
    if (!visited.insert(name).second) {
      return;
    }
    rows.push_back(FrameRow{name, depth});
    auto it = index.children.find(name);
    if (it == index.children.end()) {
      return;
    }
    for (const auto& child : it->second) {
      self(self, child, depth + 1);
    }
  };

  for (const auto& root : index.roots) {
    dfs(dfs, root, 0);
  }
  // A parent cycle (e.g. map<->odom) leaves its members with no reachable root,
  // so they would otherwise vanish from the hierarchy entirely. Emit any frame
  // still unvisited as its own depth-0 entry (sorted for stable output) so a
  // broken tree degrades to a flat list rather than disappearing.
  RootSet leftover;
  for (const auto& frame : index.all_frames) {
    if (visited.count(frame) == 0) {
      leftover.insert(frame);
    }
  }
  for (const auto& frame : leftover) {
    dfs(dfs, frame, 0);
  }
  return rows;
}

nlohmann::json TransformBuffer::getFrameHierarchyJson() const {
  std::shared_lock lock(parents_mutex_);
  const ForestIndex index = buildForestIndexLocked();

  std::unordered_set<std::string> visited;
  auto build = [&](auto& self, const std::string& name) -> nlohmann::json {
    nlohmann::json node;
    node["name"] = name;
    node["children"] = nlohmann::json::array();
    if (!visited.insert(name).second) {
      return node;  // cycle guard — emit node with no children
    }
    auto it = index.children.find(name);
    if (it != index.children.end()) {
      for (const auto& child : it->second) {
        node["children"].push_back(self(self, child));
      }
    }
    return node;
  };

  nlohmann::json out = nlohmann::json::array();
  for (const auto& root : index.roots) {
    out.push_back(build(build, root));
  }
  // Same cycle handling as getFrameHierarchy: surface any frame no root reaches
  // (cycle members) as its own top-level entry so the two views stay identical.
  RootSet leftover;
  for (const auto& frame : index.all_frames) {
    if (visited.count(frame) == 0) {
      leftover.insert(frame);
    }
  }
  for (const auto& frame : leftover) {
    out.push_back(build(build, frame));
  }
  return out;
}

bool frameNameLess(std::string_view a, std::string_view b) noexcept {
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) {
    return std::tolower(x) < std::tolower(y);
  });
}

}  // namespace pj::tf
