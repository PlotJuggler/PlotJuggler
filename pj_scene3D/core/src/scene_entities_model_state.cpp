// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/scene_entities_model_state.h"

#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace pj::scene3d {
namespace {

std::optional<std::uint64_t> addBytes(std::uint64_t left, std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::nullopt;
  }
  return left + right;
}

std::optional<std::uint64_t> multiplyBytes(std::uint64_t count, std::uint64_t stride) {
  if (count != 0U && stride > std::numeric_limits<std::uint64_t>::max() / count) {
    return std::nullopt;
  }
  return count * stride;
}

std::optional<std::uint64_t> modelEntityBytes(const PJ::sdk::SceneEntity& entity) {
  std::uint64_t bytes = sizeof(PJ::sdk::SceneEntity);
  for (const std::uint64_t dynamic :
       {static_cast<std::uint64_t>(entity.id.size()), static_cast<std::uint64_t>(entity.frame_id.size())}) {
    const auto next = addBytes(bytes, dynamic);
    if (!next.has_value()) {
      return std::nullopt;
    }
    bytes = *next;
  }
  const auto model_storage = multiplyBytes(entity.models.size(), sizeof(PJ::sdk::ModelPrimitive));
  if (!model_storage.has_value()) {
    return std::nullopt;
  }
  const auto with_models = addBytes(bytes, *model_storage);
  if (!with_models.has_value()) {
    return std::nullopt;
  }
  bytes = *with_models;
  for (const PJ::sdk::ModelPrimitive& model : entity.models) {
    for (const std::uint64_t dynamic :
         {static_cast<std::uint64_t>(model.url.size()), static_cast<std::uint64_t>(model.media_type.size()),
          static_cast<std::uint64_t>(model.data.size())}) {
      const auto next = addBytes(bytes, dynamic);
      if (!next.has_value()) {
        return std::nullopt;
      }
      bytes = *next;
    }
  }
  return bytes;
}

PJ::sdk::SceneEntity modelOnlyEntity(const PJ::sdk::SceneEntity& source) {
  PJ::sdk::SceneEntity out;
  out.timestamp = source.timestamp;
  out.frame_id = source.frame_id;
  out.id = source.id;
  out.lifetime_ns = source.lifetime_ns;
  out.frame_locked = source.frame_locked;
  out.models = source.models;
  return out;
}

bool deletedBy(const PJ::sdk::SceneEntity& entity, const PJ::sdk::SceneEntityDeletion& deletion) {
  if (entity.timestamp > deletion.timestamp) {
    return false;
  }
  return deletion.type == PJ::sdk::SceneEntityDeletion::Type::kAll || deletion.id == entity.id;
}

}  // namespace

bool sceneEntityExpiredAt(const PJ::sdk::SceneEntity& entity, std::int64_t anchor_ns, std::int64_t time_ns) {
  if (entity.lifetime_ns == 0) {
    return false;
  }
  if (entity.lifetime_ns > 0 && anchor_ns > std::numeric_limits<std::int64_t>::max() - entity.lifetime_ns) {
    return false;
  }
  if (entity.lifetime_ns < 0 && anchor_ns < std::numeric_limits<std::int64_t>::min() - entity.lifetime_ns) {
    return true;
  }
  return anchor_ns + entity.lifetime_ns < time_ns;
}

void SceneEntitiesModelState::clear() {
  entities_.clear();
  expiry_anchor_ns_.clear();
  finite_lifetimes_ = 0;
  retained_bytes_ = 0;
}

std::optional<std::uint64_t> SceneEntitiesModelState::projectedRetainedBytes(
    const PJ::sdk::SceneEntities& snapshot) const {
  try {
    std::unordered_map<std::string_view, std::size_t> final_upserts;
    final_upserts.reserve(snapshot.entities.size());
    for (std::size_t index = 0; index < snapshot.entities.size(); ++index) {
      final_upserts[snapshot.entities[index].id] = index;
    }

    std::uint64_t projected = retained_bytes_;
    for (const auto& [id, entity] : entities_) {
      bool replaced_or_deleted = final_upserts.contains(id);
      if (!replaced_or_deleted) {
        for (const PJ::sdk::SceneEntityDeletion& deletion : snapshot.deletions) {
          if (deletedBy(entity, deletion)) {
            replaced_or_deleted = true;
            break;
          }
        }
      }
      if (replaced_or_deleted) {
        const auto bytes = modelEntityBytes(entity);
        if (!bytes.has_value() || *bytes > projected) {
          return std::nullopt;
        }
        projected -= *bytes;
      }
    }
    for (const auto& [_, index] : final_upserts) {
      const auto bytes = modelEntityBytes(snapshot.entities[index]);
      if (!bytes.has_value()) {
        return std::nullopt;
      }
      const auto next = addBytes(projected, *bytes);
      if (!next.has_value()) {
        return std::nullopt;
      }
      projected = *next;
    }
    return projected;
  } catch (const std::bad_alloc&) {
    return std::nullopt;
  } catch (const std::length_error&) {
    return std::nullopt;
  }
}

bool SceneEntitiesModelState::applySnapshot(const PJ::sdk::SceneEntities& snapshot, std::int64_t ingest_ns) {
  bool changed = applySceneEntityDeletions(entities_, snapshot, [this](Iterator iterator) { return erase(iterator); });
  for (const PJ::sdk::SceneEntity& entity : snapshot.entities) {
    // projectedRetainedBytes() already rejected any snapshot whose entity sizes
    // overflow, so modelEntityBytes() cannot be nullopt here; the guards keep
    // that pre-gate a bookkeeping error instead of undefined behavior.
    if (const auto current = entities_.find(entity.id); current != entities_.end()) {
      retained_bytes_ -= modelEntityBytes(current->second).value_or(0);
      finite_lifetimes_ -= current->second.lifetime_ns != 0 ? 1U : 0U;
    }
    finite_lifetimes_ += entity.lifetime_ns != 0 ? 1U : 0U;
    PJ::sdk::SceneEntity slim = modelOnlyEntity(entity);
    retained_bytes_ += modelEntityBytes(slim).value_or(0);
    entities_[entity.id] = std::move(slim);
    // Expiry follows the ObjectStore/tracker clock, not the embedded sensor
    // timestamp. Streams commonly use different epochs for those two clocks.
    expiry_anchor_ns_[entity.id] = ingest_ns;
    changed = true;
  }
  return changed;
}

bool SceneEntitiesModelState::dropExpired(std::int64_t time_ns) {
  bool changed = false;
  for (auto iterator = entities_.begin(); finite_lifetimes_ != 0 && iterator != entities_.end();) {
    if (iterator->second.lifetime_ns != 0 &&
        sceneEntityExpiredAt(iterator->second, expiry_anchor_ns_.at(iterator->first), time_ns)) {
      iterator = erase(iterator);
      changed = true;
    } else {
      ++iterator;
    }
  }
  return changed;
}

SceneEntitiesModelState::Iterator SceneEntitiesModelState::erase(Iterator iterator) {
  retained_bytes_ -= modelEntityBytes(iterator->second).value_or(0);
  finite_lifetimes_ -= iterator->second.lifetime_ns != 0 ? 1U : 0U;
  expiry_anchor_ns_.erase(iterator->first);
  return entities_.erase(iterator);
}

}  // namespace pj::scene3d
