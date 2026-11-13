// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pj_base/builtin/mesh_map.hpp"
#include "pj_base/builtin/mesh_map_update.hpp"
#include "pj_scene3d_core/camera/camera.h"  // AABB
#include "pj_scene3d_core/grid_map_view.h"  // FiniteRange

namespace pj::scene3d {

// Upper bound on the faces a MeshMap layer expands for the GPU (three vertices
// per face, 48 bytes each across the two vertex buffers). A larger, otherwise
// valid mesh is refused with a status warning instead of uploaded.
inline constexpr uint64_t kMaxRenderableMeshMapFaces = 4'000'000;

// A MeshMap decoded for display and for applying MeshMapUpdates: positions and
// indices unpacked, every scalar channel widened to float (NaN = "no data"), the
// vertex colour packed RGBA8. Built once per base sample; updates mutate the
// channels in place. Independent of the source buffer (no anchor kept).
struct MeshMapState {
  std::string frame_id;
  PJ::sdk::Pose origin;
  std::string geometry_id;
  std::vector<float> positions;   ///< 3 per vertex (x, y, z)
  std::vector<uint32_t> indices;  ///< 3 per face
  /// Scalar channels by name. A face channel holds one value per face, a
  /// vertex channel one per vertex. Ordered maps: the UI lists them sorted.
  std::map<std::string, std::vector<float>> face_channels;
  std::map<std::string, std::vector<float>> vertex_channels;
  /// Packed R | G << 8 | B << 16 | A << 24 per vertex; empty when the mesh has no
  /// `red`/`green`/`blue` vertex channels.
  std::vector<uint32_t> vertex_colors;
  std::vector<PJ::sdk::MeshFaceLink> links;

  [[nodiscard]] uint32_t vertexCount() const {
    return static_cast<uint32_t>(positions.size() / 3);
  }
  [[nodiscard]] uint32_t faceCount() const {
    return static_cast<uint32_t>(indices.size() / 3);
  }
};

// Decodes `mesh`, which must have passed PJ::validateMeshMap(). Channels are the
// single-element fields with a sized datatype other than x/y/z and
// vertex_indices; `red`/`green`/`blue`/`alpha` also become the vertex colour.
[[nodiscard]] MeshMapState decodeMeshMap(const PJ::sdk::MeshMap& mesh);

// What applyMeshMapUpdate() did.
struct MeshMapUpdateResult {
  bool applied = false;          ///< false: geometry mismatch, nothing changed
  uint32_t skipped_records = 0;  ///< records whose index is past the element count
};

// Writes `update`'s channel values into `state`. An update whose geometry_id is
// set and differs from the state's is refused whole. A channel the state lacks
// is created ("no data" elsewhere). Multi-element and sizeless fields, and the
// `index` field itself, are ignored. `update` must have passed
// PJ::validateMeshMapUpdate().
[[nodiscard]] MeshMapUpdateResult applyMeshMapUpdate(MeshMapState& state, const PJ::sdk::MeshMapUpdate& update);

// How the mesh is coloured.
enum class MeshMapColorMode : uint8_t {
  kChannel,      ///< a scalar channel through the colormap
  kVertexColor,  ///< the RGBA vertex colour
  kSolid,        ///< one neutral colour
};

// One MeshMap ready for the GPU, de-indexed: three vertices per face.
struct MeshMapUpload {
  std::string frame_id;
  PJ::sdk::Pose origin;
  std::vector<float> positions;  ///< 9 per face
  MeshMapColorMode color_mode = MeshMapColorMode::kSolid;
  std::vector<float> values;          ///< kChannel: 3 per face (a face channel repeats its value)
  std::vector<uint32_t> colors;       ///< kVertexColor: 3 packed RGBA8 per face
  FiniteRange color_range;            ///< finite min/max of `values` (the auto colormap range)
  std::vector<float> link_positions;  ///< 6 per link: from-face centroid, to-face centroid
  std::vector<uint32_t> link_colors;  ///< 2 packed RGBA8 per link, by link kind
  /// Bumped by the producer when `positions` / `link_positions` change; the GPU
  /// pass re-uploads geometry only then.
  uint64_t geometry_revision = 0;
};

// The three vertex positions of every face, then the link segments.
void fillMeshMapGeometry(const MeshMapState& state, MeshMapUpload& upload);

// The colouring: `channel` names a face or vertex channel (kChannel), else the
// vertex colour when `mode` asks for it and the mesh has one, else solid.
void fillMeshMapColors(
    const MeshMapState& state, MeshMapColorMode mode, const std::string& channel, MeshMapUpload& upload);

// The channel shown when the user has not picked one: the first face channel
// other than `surface`, else the first vertex channel, else "" (none).
[[nodiscard]] std::string defaultMeshMapChannel(const MeshMapState& state);

// SOURCE-FRAME bounds of the vertices, transformed by the origin pose. Invalid
// for an empty mesh or one with no finite vertex.
[[nodiscard]] AABB meshMapBounds(const MeshMapState& state);

}  // namespace pj::scene3d
