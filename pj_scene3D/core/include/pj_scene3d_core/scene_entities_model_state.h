// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "pj_base/builtin/scene_entities.hpp"

namespace pj::scene3d {

// Backend-neutral reducer for the stateful ModelPrimitive half of a
// SceneEntities topic. The native layer currently carries the same semantics
// inline; the browser uses this isolated reducer so its QRhi/Assimp path cannot
// accidentally leak into procedural marker snapshot behavior.
// Replays a SceneEntities batch's DELETION contract onto an entity map:
// deletions act on PRIOR state (Foxglove's reference order — the canonical
// DELETEALL + re-add republish puts the kAll deletion and the replacement
// entities in one batch at one timestamp, so upserting first would let the
// deletion's `timestamp <= deletion.timestamp` gate erase the just-added
// entities), and each deletion removes only entries whose sensor timestamp is
// <= the deletion's. `erase` receives a map iterator, performs the owner's
// side effects, and returns the next iterator. Returns true when anything was
// erased; the caller performs its own upserts afterwards. Shared by the
// native SceneEntitiesLayer and the browser model reducer so the subtle
// ordering contract exists exactly once.
template <typename EntityMap, typename Erase>
bool applySceneEntityDeletions(EntityMap& entities, const PJ::sdk::SceneEntities& snapshot, Erase&& erase) {
  bool changed = false;
  for (const PJ::sdk::SceneEntityDeletion& deletion : snapshot.deletions) {
    if (deletion.type == PJ::sdk::SceneEntityDeletion::Type::kAll) {
      for (auto iterator = entities.begin(); iterator != entities.end();) {
        if (iterator->second.timestamp <= deletion.timestamp) {
          iterator = erase(iterator);
          changed = true;
        } else {
          ++iterator;
        }
      }
      continue;
    }
    const auto iterator = entities.find(deletion.id);
    if (iterator != entities.end() && iterator->second.timestamp <= deletion.timestamp) {
      erase(iterator);
      changed = true;
    }
  }
  return changed;
}

// Overflow-safe lifetime expiry shared by the native SceneEntitiesLayer and the
// browser model reducer (lifetime_ns == 0 means "never expires", per the
// SceneEntity contract). anchor_ns is the entity's lifetime-expiry origin: the
// ObjectStore entry timestamp it was folded from (the tracker's clock), NOT
// entity.timestamp — under streaming the entry is host-stamped while
// entity.timestamp keeps the original sensor epoch, so comparing the sensor
// epoch against the tracker would expire every finite-lifetime entity instantly.
[[nodiscard]] bool sceneEntityExpiredAt(
    const PJ::sdk::SceneEntity& entity, std::int64_t anchor_ns, std::int64_t time_ns);

class SceneEntitiesModelState {
 public:
  void clear();
  [[nodiscard]] bool applySnapshot(const PJ::sdk::SceneEntities& snapshot, std::int64_t ingest_ns);
  [[nodiscard]] bool dropExpired(std::int64_t time_ns);
  // Byte projection for the model-only entity copy made by applySnapshot().
  // nullopt means widened arithmetic or the temporary ID index overflowed.
  [[nodiscard]] std::optional<std::uint64_t> projectedRetainedBytes(const PJ::sdk::SceneEntities& snapshot) const;

  [[nodiscard]] const std::map<std::string, PJ::sdk::SceneEntity>& entities() const {
    return entities_;
  }
  [[nodiscard]] std::uint64_t retainedBytes() const {
    return retained_bytes_;
  }

 private:
  using Iterator = std::map<std::string, PJ::sdk::SceneEntity>::iterator;
  Iterator erase(Iterator iterator);

  std::map<std::string, PJ::sdk::SceneEntity> entities_;
  std::map<std::string, std::int64_t> expiry_anchor_ns_;
  /// Entities with a finite lifetime: dropExpired() runs every playhead tick,
  /// and with none it has nothing to scan.
  std::size_t finite_lifetimes_ = 0;
  std::uint64_t retained_bytes_ = 0;
};

}  // namespace pj::scene3d
