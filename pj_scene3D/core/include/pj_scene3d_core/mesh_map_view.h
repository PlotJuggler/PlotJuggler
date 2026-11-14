// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/builtin/mesh_map.hpp"
#include "pj_base/builtin/mesh_map_update.hpp"
#include "pj_scene3d_core/camera/camera.h"  // AABB
#include "pj_scene3d_core/finite_range.h"

namespace pj::scene3d {

// Upper bound on the faces a MeshMap layer expands for the GPU (three vertices
// per face, 16 bytes each across the two vertex buffers). A larger, otherwise
// valid mesh is refused with a status warning instead of uploaded.
inline constexpr uint64_t kMaxRenderableMeshMapFaces = 4'000'000;

// The GPU's "no data" value for a channel vertex, and the magnitude data values
// are clamped to: the headroom between them keeps the sentinel (and its
// perspective-correct interpolation) finite and far from any data value. The
// fragment shader (meshmap.frag) tests abs(v) > 1e29.
inline constexpr float kMeshMapNoDataValue = 1.0e30F;
inline constexpr float kMeshMapValueLimit = 1.0e28F;

// Colour-source tokens beside channel keys: angle brackets keep them distinct
// from any channel key ("face:<name>" / "vertex:<name>").
inline constexpr std::string_view kMeshMapSourceVertexColor = "<vertex color>";
inline constexpr std::string_view kMeshMapSourceSolid = "<solid>";

// A half-open span of element indices [begin, end); empty when begin >= end.
struct ElementRange {
  uint32_t begin = 0;
  uint32_t end = 0;

  [[nodiscard]] bool empty() const {
    return begin >= end;
  }
  // Grows the span to cover `index`.
  void add(uint32_t index) {
    if (empty()) {
      begin = index;
      end = index + 1;
    } else {
      begin = begin < index ? begin : index;
      end = end > index ? end : index + 1;
    }
  }
  bool operator==(const ElementRange&) const = default;
};

// One scalar channel of a decoded mesh. Values are exact: a double holds every
// value of the supported datatypes (no 64-bit integers), so category ids above
// 2^24 stay distinct. "No data" is NaN: the channel's info.no_data value is
// folded into NaN at decode, compared at the channel's own precision.
struct MeshMapChannelState {
  std::string name;
  PJ::sdk::MeshChannel::Target target = PJ::sdk::MeshChannel::Target::kFace;
  /// The source datatype: red/green/blue/alpha are the vertex colour only as uint8.
  PJ::sdk::PointField::Datatype datatype = PJ::sdk::PointField::Datatype::kFloat32;
  PJ::sdk::ChannelInfo info;
  std::vector<double> values;  ///< one per vertex or face
  /// The span holding every element applyMeshMapUpdate() wrote since
  /// clearMeshMapDirty(), or all of them when a patch changed `info`: what
  /// refreshMeshMapColors() re-colours.
  // ponytail: one span, lowest to highest index written. A patch local in space
  // need not be local in index order; keep a few runs instead if the elements
  // between them start to cost.
  ElementRange dirty;
};

// A MeshMap decoded for display and for applying MeshMapUpdates. Built once per
// map sample; updates mutate the channels in place. Owns its arrays (no anchor
// into the producer's buffer is kept).
struct MeshMapState {
  std::string frame_id;
  /// The map's origin, composed with the offset removed from float64 positions
  /// (origin * translate(offset)), so `positions` stay float-precise far from 0.
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
// channels (normals, uvs) are not kept: nothing colours by them yet. float64
// positions lose the first finite vertex as a double-precision offset before they
// narrow to float; the offset moves into `origin` (when it is finite).
[[nodiscard]] MeshMapState decodeMeshMap(const PJ::sdk::MeshMap& mesh);

// What applyMeshMapUpdate() did.
struct MeshMapUpdateResult {
  bool applied = false;          ///< false: geometry mismatch, nothing changed
  uint32_t skipped_indices = 0;  ///< sparse indices past the element count
  uint32_t skipped_patches = 0;  ///< dense patches of the wrong size, multi-component patches
};

// Writes `update`'s patches into `state`. An update whose geometry_id is set and
// differs from the state's is refused whole. A dense patch must hold one value
// per element; a channel the state lacks is created ("no data" elsewhere);
// `channel.info` replaces the state channel's; its datatype stays the one the
// channel was created with (a uint8 colour component patched with float values
// stays the vertex colour). Every element written joins its channel's `dirty`
// span. `update` must have passed PJ::validateMeshMapUpdate().
[[nodiscard]] MeshMapUpdateResult applyMeshMapUpdate(MeshMapState& state, const PJ::sdk::MeshMapUpdate& update);

// Empties every channel's `dirty` span: call once the colours built from the
// state are up to date.
void clearMeshMapDirty(MeshMapState& state);

// The selector key of a channel: "face:<name>" or "vertex:<name>".
[[nodiscard]] std::string meshMapChannelKey(PJ::sdk::MeshChannel::Target target, std::string_view name);

// The colour sources the mesh offers: every channel key, then the vertex colour
// when `red`/`green`/`blue` uint8 vertex channels exist, then solid.
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
  /// kChannel: 3 per face, clamped to +-kMeshMapValueLimit, ready for the GPU. A
  /// face with "no data" at any corner has kMeshMapNoDataValue at all three, so it
  /// draws as no data instead of interpolating towards it.
  std::vector<float> values;
  std::vector<uint32_t> colors;  ///< kRgba: 3 packed RGBA8 (R in the low byte) per face
  /// Some colour has alpha below 255: the surface blends even at full layer
  /// opacity, so it draws in the translucent phase without writing depth.
  bool translucent = false;
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
// continuous channel through the colormap (kChannel), a categorical
// channel through its categories' colours or the palette (kRgba), the vertex
// colour (kRgba), else solid. An unknown source colours solid. Coloured by a
// channel, a face with "no data" at any corner draws whole as no data.
void fillMeshMapColors(const MeshMapState& state, std::string_view source, MeshMapUpload& upload);

// Brings an `upload` that fillMeshMapColors() coloured from the same `source`
// up to date with the channels' `dirty` spans, rewriting only the faces a FACE
// channel's span covers. Returns the span of corners (three per face) it rewrote,
// empty when the source's channels are untouched. nullopt when the upload must
// be coloured whole instead: the dirty source is a vertex channel or the vertex
// colour (a vertex touches faces anywhere in the buffer), or its kind no longer
// matches the upload's colour mode. `translucent` only ever turns on here.
[[nodiscard]] std::optional<ElementRange> refreshMeshMapColors(
    const MeshMapState& state, std::string_view source, MeshMapUpload& upload);

// A distinct colour per small integer (qualitative palette), packed RGBA8.
[[nodiscard]] uint32_t categoricalColor(int64_t value);

// SOURCE-FRAME bounds of the vertices, transformed by the origin pose. Invalid
// for an empty mesh or one with no finite vertex.
[[nodiscard]] AABB meshMapBounds(const MeshMapState& state);

}  // namespace pj::scene3d
