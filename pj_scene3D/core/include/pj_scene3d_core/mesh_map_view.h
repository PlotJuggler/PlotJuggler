// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/builtin/mesh_map.hpp"
#include "pj_base/builtin/mesh_map_update.hpp"
#include "pj_scene3d_core/camera/camera.h"  // AABB
#include "pj_scene3d_core/grid_map_view.h"  // FiniteRange

namespace pj::scene3d {

// Upper bound on the faces a MeshMap layer expands for the GPU (three vertices
// per face, 16 bytes each across the two vertex buffers). A larger, otherwise
// valid mesh is refused with a status warning instead of uploaded.
inline constexpr uint64_t kMaxRenderableMeshMapFaces = 4'000'000;

// Colour-source tokens beside channel keys: angle brackets keep them distinct
// from any channel key ("face:<name>" / "vertex:<name>").
inline constexpr std::string_view kMeshMapSourceVertexColor = "<vertex color>";
inline constexpr std::string_view kMeshMapSourceSolid = "<solid>";

// One scalar channel of a decoded mesh, widened to float. "No data" is NaN:
// the channel's info.no_data value is folded into NaN at decode.
struct MeshMapChannelState {
  std::string name;
  PJ::sdk::MeshChannel::Target target = PJ::sdk::MeshChannel::Target::kFace;
  PJ::sdk::ChannelInfo info;
  std::vector<float> values;  ///< one per vertex or face
};

// A MeshMap decoded for display and for applying MeshMapUpdates. Built once per
// map sample; updates mutate the channels in place. Owns its arrays (no anchor
// into the producer's buffer is kept).
struct MeshMapState {
  std::string frame_id;
  PJ::sdk::Pose origin;
  std::string geometry_id;
  std::vector<float> positions;               ///< 3 per vertex
  std::vector<uint32_t> indices;              ///< 3 per face
  std::vector<MeshMapChannelState> channels;  ///< single-component channels; (target, name) unique
  std::vector<PJ::sdk::MeshFaceLink> links;

  [[nodiscard]] uint32_t vertexCount() const {
    return static_cast<uint32_t>(positions.size() / 3);
  }
  [[nodiscard]] uint32_t faceCount() const {
    return static_cast<uint32_t>(indices.size() / 3);
  }
  [[nodiscard]] const MeshMapChannelState* find(PJ::sdk::MeshChannel::Target target, std::string_view name) const;
  [[nodiscard]] MeshMapChannelState* find(PJ::sdk::MeshChannel::Target target, std::string_view name);
};

// Decodes `mesh`, which must have passed PJ::validateMeshMap(). Multi-component
// channels (normals, uvs) are not kept: nothing colours by them yet.
[[nodiscard]] MeshMapState decodeMeshMap(const PJ::sdk::MeshMap& mesh);

// What applyMeshMapUpdate() did.
struct MeshMapUpdateResult {
  bool applied = false;          ///< false: geometry mismatch, nothing changed
  uint32_t skipped_records = 0;  ///< sparse indices past the element count
  uint32_t skipped_patches = 0;  ///< dense patches of the wrong size, multi-component patches
};

// Writes `update`'s patches into `state`. An update whose geometry_id is set and
// differs from the state's is refused whole. A dense patch must hold one value
// per element; a channel the state lacks is created ("no data" elsewhere);
// `channel.info` replaces the state channel's. `update` must have passed
// PJ::validateMeshMapUpdate().
[[nodiscard]] MeshMapUpdateResult applyMeshMapUpdate(MeshMapState& state, const PJ::sdk::MeshMapUpdate& update);

// The selector key of a channel: "face:<name>" or "vertex:<name>".
[[nodiscard]] std::string meshMapChannelKey(PJ::sdk::MeshChannel::Target target, std::string_view name);

// The colour sources the mesh offers: every channel key, then the vertex colour
// when `red`/`green`/`blue` vertex channels exist, then solid.
[[nodiscard]] std::vector<std::string> meshMapColorSources(const MeshMapState& state);

// The source shown when the user has not picked one: the first continuous face
// channel, else the first continuous vertex channel (colour components aside),
// else the vertex colour, else the first categorical channel, else solid.
[[nodiscard]] std::string defaultMeshMapColorSource(const MeshMapState& state);

// How the upload is coloured.
enum class MeshMapColorMode : uint8_t {
  kChannel,  ///< `values` through the colormap
  kRgba,     ///< `colors`: vertex colour, or a categorical channel's palette
  kSolid,    ///< one neutral colour
};

// One MeshMap ready for the GPU, de-indexed: three vertices per face.
struct MeshMapUpload {
  std::string frame_id;
  PJ::sdk::Pose origin;
  std::vector<float> positions;  ///< 9 per face
  MeshMapColorMode color_mode = MeshMapColorMode::kSolid;
  /// kChannel: 3 per face. A face with "no data" at any corner has NaN at all
  /// three, so it draws as no data instead of interpolating towards it.
  std::vector<float> values;
  std::vector<uint32_t> colors;       ///< kRgba: 3 packed RGBA8 (R in the low byte) per face
  FiniteRange color_range;            ///< finite min/max of `values` (the auto colormap range)
  std::vector<float> link_positions;  ///< 6 per link: from-face centroid, to-face centroid
  std::vector<uint32_t> link_colors;  ///< 2 per link, a palette colour by link kind
  /// Bumped by the producer when `positions` / `link_positions` change; the GPU
  /// pass re-uploads geometry only then.
  uint64_t geometry_revision = 0;
};

// The three vertex positions of every face, then the link segments.
void fillMeshMapGeometry(const MeshMapState& state, MeshMapUpload& upload);

// Colours `upload` from `source` (a channel key or a kMeshMapSource* token): a
// continuous channel through the colormap (kChannel), a categorical or bitmask
// channel through its categories' colours or the palette (kRgba), the vertex
// colour (kRgba), else solid. An unknown source colours solid.
void fillMeshMapColors(const MeshMapState& state, std::string_view source, MeshMapUpload& upload);

// A distinct colour per small integer (qualitative palette), packed RGBA8.
[[nodiscard]] uint32_t categoricalColor(int64_t value);

// SOURCE-FRAME bounds of the vertices, transformed by the origin pose. Invalid
// for an empty mesh or one with no finite vertex.
[[nodiscard]] AABB meshMapBounds(const MeshMapState& state);

}  // namespace pj::scene3d
