// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "pj_base/builtin/mesh_map_update.hpp"
#include "pj_base/types.hpp"
#include "pj_scene3d_core/mesh_map_view.h"

namespace pj::scene3d {

/// What the updates of one MeshMapReconstructor::advance() window did.
struct MeshMapReplay {
  int applied = 0;
  /// Updates of a sibling stream that could not apply: not a valid update, or
  /// naming another geometry.
  int refused = 0;
  int lost = 0;                  ///< entries of a paired stream the source could not re-read
  uint32_t skipped_indices = 0;  ///< summed MeshMapUpdateResult::skipped_indices
  uint32_t skipped_patches = 0;  ///< summed MeshMapUpdateResult::skipped_patches
  std::string decode_error;      ///< why the last invalid sibling update was refused
};

/// Rebuilds the mesh shown at a time `t` from a map stream and any number of
/// MeshMapUpdate streams, the mesh-map counterpart of OccupancyGridReconstructor:
///
///   shown(t) = (latest map with ts <= t) + every update of a paired stream in
///              [that map's ts, t], applied in ascending ts order.
///
/// Pure: no Qt, GPU or ObjectStore. The caller owns the timeline and hands over
/// decoded objects: the map through rebase() when needsBase() asks for it, the
/// updates through the UpdatesProvider of advance(). Going forward only the new
/// updates are applied; a new map, a time before what was applied, or
/// invalidate() starts over from the map.
///
/// Pairing: a `sibling` stream (the map's own update topic) applies every update,
/// those without a geometry_id included. Any other stream applies an update only
/// when it names the map's geometry_id (both set), and counts as paired() from
/// its first one. An entry of such a stream found to name another geometry is
/// remembered and never asked for again while the geometry stays the same, so a
/// replay does not re-read another mesh's updates.
class MeshMapReconstructor {
 public:
  /// A candidate update stream. `key` is the caller's stable name for it (the
  /// topic id value in production).
  struct Stream {
    uint64_t key = 0;
    bool sibling = false;
  };

  /// Where an UpdatesProvider reports the entries of its window. `identity` is
  /// the never-reused token of the timeline entry (the ObjectStore SequentialUID
  /// value in production).
  class Sink {
   public:
    /// False for an entry already known to belong to another mesh: skip it
    /// without fetching or decoding.
    [[nodiscard]] bool wants(uint64_t stream, uint64_t identity) const;
    /// A decoded update that passed PJ::validateMeshMapUpdate().
    void apply(uint64_t stream, uint64_t identity, const PJ::sdk::MeshMapUpdate& update);
    /// An entry that did not decode into a valid update.
    void invalid(uint64_t stream, uint64_t identity, std::string reason);
    /// An entry the source permanently failed to re-read.
    void lost(uint64_t stream);

   private:
    friend class MeshMapReconstructor;
    Sink(MeshMapReconstructor* owner, MeshMapReplay* replay) : owner_(owner), replay_(replay) {}
    MeshMapReconstructor* owner_;
    MeshMapReplay* replay_;
  };

  /// Reports every update entry with `lo < ts <= hi`, over all streams, in
  /// ascending ts order, to `sink`.
  using UpdatesProvider = std::function<void(PJ::Timestamp lo, PJ::Timestamp hi, Sink& sink)>;

  /// Replaces the candidate streams, keeping what is known about those that
  /// stay. A changed set invalidate()s: a new stream may hold updates inside the
  /// window already applied.
  void setStreams(std::span<const Stream> streams);
  /// The stream's updates belong to this map: a sibling, or one whose update
  /// applied. Sticky across maps; cleared only by reset().
  [[nodiscard]] bool paired(uint64_t stream) const;

  /// The map entry `identity` must be decoded and handed to rebase() before
  /// advance(t): there is no state, it is another map, `t` is before what was
  /// applied, or invalidate() was called.
  [[nodiscard]] bool needsBase(uint64_t identity, PJ::Timestamp t) const;
  /// Starts over from `state`, decoded from the map entry `identity` stamped
  /// `timestamp`. Updates stamped with the map itself are still to apply.
  void rebase(uint64_t identity, PJ::Timestamp timestamp, MeshMapState state);
  /// Applies the updates in (appliedUntil(), t] and moves appliedUntil() to `t`;
  /// a no-op without a state or when `t` is not ahead. Each channel's `dirty`
  /// span then covers exactly what this call wrote.
  MeshMapReplay advance(PJ::Timestamp t, const UpdatesProvider& updates_in);

  /// Drops the mesh (its map was refused): state() is null until the next
  /// rebase(). Pairings stay.
  void clear();
  /// The timeline changed behind appliedUntil() (a late update, a dropped map):
  /// the next needsBase() is true. Pairings and the foreign-entry memo stay.
  void invalidate();
  /// Forgets everything, pairings included: a new attach, a replaced dataset.
  void reset();

  /// The reconstructed mesh, or nullptr before the first rebase().
  [[nodiscard]] const MeshMapState* state() const {
    return state_.has_value() ? &*state_ : nullptr;
  }
  /// Updates stamped at or before this are in state().
  [[nodiscard]] PJ::Timestamp appliedUntil() const {
    return applied_until_;
  }
  /// Updates stamped at or before this never apply to the current map: they
  /// predate it.
  [[nodiscard]] PJ::Timestamp appliedFrom() const {
    return applied_from_;
  }
  /// Sink::wants() outside a replay, for a caller examining entries itself.
  [[nodiscard]] bool wants(uint64_t stream, uint64_t identity) const;
  /// True when `update` would apply to the current map. An entry of a
  /// non-sibling stream that would not is remembered, so wants() is false for it
  /// from then on.
  [[nodiscard]] bool claims(uint64_t stream, uint64_t identity, const PJ::sdk::MeshMapUpdate& update);

  // --- Introspection (tests / diagnostics) ---
  [[nodiscard]] std::size_t foreignEntryCount() const;

 private:
  struct StreamState {
    uint64_t key = 0;
    bool sibling = false;
    bool matched = false;
    // ponytail: grows by 8 bytes per foreign update for as long as the geometry
    // stays the same; prune entries older than the map if a long live session
    // with several meshes ever makes it matter.
    std::unordered_set<uint64_t> foreign;
  };

  [[nodiscard]] const StreamState* find(uint64_t stream) const;
  [[nodiscard]] StreamState* find(uint64_t stream);

  std::vector<StreamState> streams_;
  std::optional<MeshMapState> state_;
  uint64_t base_identity_ = 0;
  PJ::Timestamp applied_from_ = 0;
  PJ::Timestamp applied_until_ = 0;
  bool invalidated_ = false;
};

}  // namespace pj::scene3d
