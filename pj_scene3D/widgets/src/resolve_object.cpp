// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/resolve_object.h"

#include <string>
#include <utility>

#include "pj_base/builtin/compressed_point_cloud_codec.hpp"
#include "pj_base/builtin/frame_transforms_codec.hpp"
#include "pj_base/builtin/grid_map_codec.hpp"
#include "pj_base/builtin/occupancy_grid_codec.hpp"
#include "pj_base/builtin/occupancy_grid_update_codec.hpp"
#include "pj_base/builtin/point_cloud_codec.hpp"
#include "pj_base/builtin/poses_in_frame_codec.hpp"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_base/builtin/voxel_grid_codec.hpp"
#include "pj_scene3d_widgets/parse_locked.h"  // parseLocked

namespace pj::scene3d {
namespace {

using BT = PJ::sdk::BuiltinObjectType;

// Wrap a canonical-codec result as an ObjectRecord. ts is left nullopt: the
// canonical producer pushed the blob under the store entry's timestamp, which
// the host uses for nullopt (mirrors the canonical image path); per-element
// timestamps (FrameTransforms) are read straight from the decoded struct.
template <typename T>
PJ::Expected<PJ::sdk::ObjectRecord> wrapCanonical(PJ::Expected<T> decoded) {
  if (!decoded) {
    return PJ::unexpected(std::move(decoded).error());
  }
  return PJ::sdk::ObjectRecord{.ts = std::nullopt, .object = PJ::sdk::BuiltinObject{std::move(*decoded)}};
}

}  // namespace

bool hasCanonical3DCodec(BT type) noexcept {
  switch (type) {
    case BT::kPointCloud:
    case BT::kCompressedPointCloud:
    case BT::kFrameTransforms:
    case BT::kPosesInFrame:
    case BT::kOccupancyGrid:
    case BT::kOccupancyGridUpdate:
    case BT::kVoxelGrid:
    case BT::kGridMap:
    case BT::kSceneEntities:
      return true;
    default:
      return false;
  }
}

PJ::Expected<PJ::sdk::ObjectRecord> resolveObject(
    const PJ::SessionManager::ParserBinding& binding, BT type, PJ::Timestamp ts, const PJ::sdk::PayloadView& payload) {
  // Parser bound -> the topic's objects are parser-decoded messages.
  if (binding) {
    return parseLocked(binding, ts, payload);
  }
  // No parser -> the bytes are a serialized canonical object; decode by type.
  const auto* data = payload.bytes.data();
  const auto size = payload.bytes.size();
  switch (type) {
    case BT::kPointCloud:
      return wrapCanonical(PJ::deserializePointCloud(data, size));
    case BT::kCompressedPointCloud:
      return wrapCanonical(PJ::deserializeCompressedPointCloud(data, size));
    case BT::kFrameTransforms:
      return wrapCanonical(PJ::deserializeFrameTransforms(data, size));
    case BT::kPosesInFrame:
      return wrapCanonical(PJ::deserializePosesInFrame(data, size));
    case BT::kOccupancyGrid:
      return wrapCanonical(PJ::deserializeOccupancyGrid(data, size));
    case BT::kOccupancyGridUpdate:
      return wrapCanonical(PJ::deserializeOccupancyGridUpdate(data, size));
    case BT::kVoxelGrid:
      return wrapCanonical(PJ::deserializeVoxelGrid(data, size));
    case BT::kGridMap:
      return wrapCanonical(PJ::deserializeGridMap(data, size));
    case BT::kSceneEntities:
      return wrapCanonical(PJ::deserializeSceneEntities(data, size));
    default:
      return PJ::unexpected(
          std::string("resolveObject: object topic has no MessageParser and type ") + std::string(PJ::sdk::name(type)) +
          " has no canonical 3D codec");
  }
}

}  // namespace pj::scene3d
