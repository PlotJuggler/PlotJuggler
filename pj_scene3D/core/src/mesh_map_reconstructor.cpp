// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/mesh_map_reconstructor.h"

#include <algorithm>
#include <utility>

namespace pj::scene3d {

const MeshMapReconstructor::StreamState* MeshMapReconstructor::find(uint64_t stream) const {
  const auto found =
      std::find_if(streams_.begin(), streams_.end(), [stream](const StreamState& s) { return s.key == stream; });
  return found != streams_.end() ? &*found : nullptr;
}

MeshMapReconstructor::StreamState* MeshMapReconstructor::find(uint64_t stream) {
  return const_cast<StreamState*>(std::as_const(*this).find(stream));
}

void MeshMapReconstructor::setStreams(std::span<const Stream> streams) {
  const bool same = std::equal(
      streams.begin(), streams.end(), streams_.begin(), streams_.end(),
      [](const Stream& a, const StreamState& b) { return a.key == b.key && a.sibling == b.sibling; });
  if (same) {
    return;
  }
  std::vector<StreamState> next;
  next.reserve(streams.size());
  for (const Stream& stream : streams) {
    StreamState* known = find(stream.key);
    StreamState state;
    if (known != nullptr) {
      state = std::move(*known);
    }
    state.key = stream.key;
    state.sibling = stream.sibling;
    next.push_back(std::move(state));
  }
  streams_ = std::move(next);
  invalidate();
}

bool MeshMapReconstructor::paired(uint64_t stream) const {
  const StreamState* found = find(stream);
  return found != nullptr && (found->sibling || found->matched);
}

bool MeshMapReconstructor::needsBase(uint64_t identity, PJ::Timestamp t) const {
  return !state_.has_value() || invalidated_ || identity != base_identity_ || t < applied_until_;
}

void MeshMapReconstructor::rebase(uint64_t identity, PJ::Timestamp timestamp, MeshMapState state) {
  // The memo records "not this geometry": it holds only while the geometry does.
  if (!state_.has_value() || state_->geometry_id != state.geometry_id) {
    for (StreamState& stream : streams_) {
      stream.foreign.clear();
    }
  }
  state_ = std::move(state);
  base_identity_ = identity;
  // Exclusive lower bound of the first advance(): updates stamped with the map apply.
  applied_from_ = timestamp - 1;
  applied_until_ = applied_from_;
  invalidated_ = false;
}

MeshMapReplay MeshMapReconstructor::advance(PJ::Timestamp t, const UpdatesProvider& updates_in) {
  MeshMapReplay replay;
  if (!state_.has_value() || t <= applied_until_) {
    return replay;
  }
  clearMeshMapDirty(*state_);
  if (!streams_.empty()) {
    Sink sink(this, &replay);
    updates_in(applied_until_, t, sink);
  }
  applied_until_ = t;
  return replay;
}

void MeshMapReconstructor::clear() {
  state_.reset();
}

void MeshMapReconstructor::invalidate() {
  invalidated_ = true;
}

void MeshMapReconstructor::reset() {
  streams_.clear();
  state_.reset();
  base_identity_ = 0;
  applied_from_ = 0;
  applied_until_ = 0;
  invalidated_ = false;
}

bool MeshMapReconstructor::wants(uint64_t stream, uint64_t identity) const {
  const StreamState* found = find(stream);
  return found != nullptr && !found->foreign.contains(identity);
}

bool MeshMapReconstructor::claims(uint64_t stream, uint64_t identity, const PJ::sdk::MeshMapUpdate& update) {
  StreamState* found = find(stream);
  if (found == nullptr || !state_.has_value()) {
    return false;
  }
  if (found->sibling) {
    return true;
  }
  if (!update.geometry_id.empty() && update.geometry_id == state_->geometry_id) {
    return true;
  }
  found->foreign.insert(identity);
  return false;
}

std::size_t MeshMapReconstructor::foreignEntryCount() const {
  std::size_t total = 0;
  for (const StreamState& stream : streams_) {
    total += stream.foreign.size();
  }
  return total;
}

bool MeshMapReconstructor::Sink::wants(uint64_t stream, uint64_t identity) const {
  return owner_->wants(stream, identity);
}

void MeshMapReconstructor::Sink::apply(uint64_t stream, uint64_t identity, const PJ::sdk::MeshMapUpdate& update) {
  if (!owner_->claims(stream, identity, update)) {
    return;
  }
  const MeshMapUpdateResult result = applyMeshMapUpdate(*owner_->state_, update);
  if (!result.applied) {
    ++replay_->refused;  // a sibling's update naming another geometry
    return;
  }
  owner_->find(stream)->matched = true;
  replay_->skipped_indices += result.skipped_indices;
  replay_->skipped_patches += result.skipped_patches;
  ++replay_->applied;
}

void MeshMapReconstructor::Sink::invalid(uint64_t stream, uint64_t identity, std::string reason) {
  StreamState* found = owner_->find(stream);
  if (found == nullptr) {
    return;
  }
  if (found->sibling) {
    replay_->decode_error = std::move(reason);
    ++replay_->refused;
  } else {
    found->foreign.insert(identity);  // another topic's decode trouble is not this map's
  }
}

void MeshMapReconstructor::Sink::lost(uint64_t stream) {
  replay_->lost += owner_->paired(stream) ? 1 : 0;
}

}  // namespace pj::scene3d
