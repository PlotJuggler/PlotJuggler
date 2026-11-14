// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/mesh_map_view.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <glm/glm.hpp>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>

#include "pj_scene3d_core/pointcloud_convert.h"  // readScalarAt, readFloat64At
#include "pj_scene3d_core/pose_math.h"           // poseMatrix

namespace pj::scene3d {

namespace {

using PJ::sdk::ChannelKind;
using PJ::sdk::MeshChannel;
using Target = PJ::sdk::MeshChannel::Target;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

constexpr std::array<std::string_view, 4> kColorComponents = {"red", "green", "blue", "alpha"};

// A uint8 vertex channel named after a colour component: the producer's vertex
// colour, not data. A float `red` in [0, 1] would draw black as a colour.
bool isColorComponent(const MeshMapChannelState& channel) {
  return channel.target == Target::kVertex && channel.datatype == PJ::sdk::PointField::Datatype::kUint8 &&
         std::find(kColorComponents.begin(), kColorComponents.end(), channel.name) != kColorComponents.end();
}

// The colour component `name`, or nullptr when absent or not a colour component.
const MeshMapChannelState* colorComponent(const MeshMapState& state, std::string_view name) {
  const MeshMapChannelState* channel = state.find(Target::kVertex, name);
  return channel != nullptr && isColorComponent(*channel) ? channel : nullptr;
}

// One little-endian wire element, exact (readScalarAt widens to float). A plain
// load: every PJ target is little-endian, as the wire is.
static_assert(std::endian::native == std::endian::little);
template <typename T>
double readAs(const uint8_t* data) {
  T value;
  std::memcpy(&value, data, sizeof(T));
  return static_cast<double>(value);
}

// Element `i` of a single-component channel, exact, "no data" -> NaN. The
// sentinel is compared before any narrowing, and a float32 element against the
// sentinel narrowed to float, so 1e30 matches the float the producer wrote.
double readValue(
    const uint8_t* data, PJ::sdk::PointField::Datatype datatype, const PJ::sdk::ChannelInfo& info, size_t i) {
  using Datatype = PJ::sdk::PointField::Datatype;
  const uint8_t* element = data + (i * PJ::sdk::bytesPerElement(datatype));
  double value = kNaN;
  switch (datatype) {
    case Datatype::kInt8:
      value = readAs<int8_t>(element);
      break;
    case Datatype::kUint8:
      value = readAs<uint8_t>(element);
      break;
    case Datatype::kInt16:
      value = readAs<int16_t>(element);
      break;
    case Datatype::kUint16:
      value = readAs<uint16_t>(element);
      break;
    case Datatype::kInt32:
      value = readAs<int32_t>(element);
      break;
    case Datatype::kUint32:
      value = readAs<uint32_t>(element);
      break;
    case Datatype::kFloat32:
      value = readAs<float>(element);
      if (info.no_data.has_value() && value == static_cast<double>(static_cast<float>(*info.no_data))) {
        return kNaN;
      }
      return value;
    case Datatype::kFloat64:
      value = readAs<double>(element);
      break;
    case Datatype::kUnknown:
      return kNaN;
  }
  if (info.no_data.has_value() && value == *info.no_data) {
    return kNaN;
  }
  return value;
}

// The channel's values in the state's representation.
std::vector<double> readValues(const MeshChannel& channel, size_t elements) {
  std::vector<double> values(elements);
  for (size_t i = 0; i < elements; ++i) {
    values[i] = readValue(channel.data.data(), channel.datatype, channel.info, i);
  }
  return values;
}

constexpr uint32_t packRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8U) | (static_cast<uint32_t>(b) << 16U) |
         (static_cast<uint32_t>(a) << 24U);
}

uint8_t colorByte(double value) {
  return std::isfinite(value) ? static_cast<uint8_t>(std::clamp(value, 0.0, 255.0)) : uint8_t{0};
}

constexpr uint8_t alphaOf(uint32_t color) {
  return static_cast<uint8_t>(color >> 24U);
}

constexpr uint32_t kNoDataColor = 0xFF808080U;  // mid grey, opaque

// "face:<name>" / "vertex:<name>" back to its channel, or nullptr.
const MeshMapChannelState* channelForKey(const MeshMapState& state, std::string_view key) {
  for (const MeshMapChannelState& channel : state.channels) {
    if (meshMapChannelKey(channel.target, channel.name) == key) {
      return &channel;
    }
  }
  return nullptr;
}

bool hasVertexColor(const MeshMapState& state) {
  return colorComponent(state, "red") != nullptr && colorComponent(state, "green") != nullptr &&
         colorComponent(state, "blue") != nullptr;
}

// Writes one element per corner of faces [begin, end) to `out`: `map(value)`,
// or `no_data` on all three corners of a face that has "no data" at any.
template <typename T, typename Map>
void writeFaces(
    const MeshMapState& state, const MeshMapChannelState& channel, size_t begin, size_t end, T* out, T no_data,
    const Map& map) {
  if (channel.target == Target::kFace) {
    for (size_t face = begin; face < end; ++face) {
      const double value = channel.values[face];
      std::fill_n(out + (3 * face), 3, std::isfinite(value) ? map(value) : no_data);
    }
    return;
  }
  for (size_t face = begin; face < end; ++face) {
    std::array<double, 3> values{};
    bool valid = true;
    for (size_t corner = 0; corner < 3; ++corner) {
      values[corner] = channel.values[state.indices[(3 * face) + corner]];
      valid = valid && std::isfinite(values[corner]);
    }
    for (size_t corner = 0; corner < 3; ++corner) {
      out[(3 * face) + corner] = valid ? map(values[corner]) : no_data;
    }
  }
}

// Colours faces [begin, end) of `upload` by `channel`, in the mode its kind
// asks for (`values` or `colors`, already sized): a continuous channel through
// the colormap, with `color_range` brought up to date; a categorical one with
// the producer's category colour (the first category of a value that carries
// one), else the palette, turning `translucent` on for an alpha below 255.
void colorFaces(
    const MeshMapState& state, const MeshMapChannelState& channel, size_t begin, size_t end, MeshMapUpload& upload) {
  if (channel.info.kind == ChannelKind::kContinuous) {
    writeFaces(state, channel, begin, end, upload.values.data(), kMeshMapNoDataValue, [](double value) {
      return static_cast<float>(std::clamp<double>(value, -kMeshMapValueLimit, kMeshMapValueLimit));
    });
    // The finite range of every value, "no data" corners aside. The three
    // corners of a face channel's face are equal: one in three is enough.
    upload.color_range = {};
    const size_t stride = channel.target == Target::kFace ? 3 : 1;
    for (size_t i = 0; i < upload.values.size(); i += stride) {
      if (std::abs(upload.values[i]) <= kMeshMapValueLimit) {
        accumulate(upload.color_range, upload.values[i]);
      }
    }
    return;
  }
  std::unordered_map<int64_t, uint32_t> category_colors;
  for (const PJ::sdk::ChannelCategory& category : channel.info.categories) {
    if (category.color.has_value()) {
      category_colors.try_emplace(
          category.value, packRgba(category.color->r, category.color->g, category.color->b, category.color->a));
    }
  }
  writeFaces(state, channel, begin, end, upload.colors.data(), kNoDataColor, [&category_colors](double value) {
    const auto id = static_cast<int64_t>(std::llround(value));
    const auto found = category_colors.find(id);
    return found != category_colors.end() ? found->second : categoricalColor(id);
  });
  for (size_t i = 3 * begin; i < 3 * end && !upload.translucent; ++i) {
    upload.translucent = alphaOf(upload.colors[i]) < 255;
  }
}

}  // namespace

const MeshMapChannelState* MeshMapState::find(Target target, std::string_view name) const {
  for (const MeshMapChannelState& channel : channels) {
    if (channel.target == target && channel.name == name) {
      return &channel;
    }
  }
  return nullptr;
}

MeshMapChannelState* MeshMapState::find(Target target, std::string_view name) {
  return const_cast<MeshMapChannelState*>(static_cast<const MeshMapState&>(*this).find(target, name));
}

MeshMapState decodeMeshMap(const PJ::sdk::MeshMap& mesh) {
  MeshMapState state;
  state.frame_id = mesh.frame_id;
  state.origin = mesh.origin;
  state.geometry_id = mesh.geometry_id;
  state.links = mesh.links;

  const size_t coordinates = static_cast<size_t>(mesh.vertex_count) * 3;
  state.positions.resize(coordinates);
  if (mesh.position_type == PJ::sdk::PointField::Datatype::kFloat64) {
    // Far from the origin (UTM-scale coordinates) float32 has no centimetres:
    // narrow relative to the first finite vertex, and place that vertex through
    // the origin instead. The view's camera-relative model matrix is double.
    const auto at = [&mesh](size_t i) { return readFloat64At(mesh.positions.data() + (i * sizeof(double))); };
    glm::dvec3 offset(0.0);
    const std::optional<glm::dmat4> origin = poseMatrix(mesh.origin);
    for (size_t v = 0; origin.has_value() && v + 2 < coordinates; v += 3) {
      const glm::dvec3 p(at(v), at(v + 1), at(v + 2));
      if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) {
        offset = p;
        const glm::dvec3 placed(*origin * glm::dvec4(offset, 1.0));
        state.origin.position = {.x = placed.x, .y = placed.y, .z = placed.z};
        break;
      }
    }
    for (size_t i = 0; i < coordinates; ++i) {
      state.positions[i] = static_cast<float>(at(i) - offset[static_cast<glm::length_t>(i % 3)]);
    }
  } else {
    for (size_t i = 0; i < coordinates; ++i) {
      state.positions[i] =
          readScalarAt(mesh.positions.data() + (i * PJ::sdk::bytesPerElement(mesh.position_type)), mesh.position_type);
    }
  }
  state.indices.resize(static_cast<size_t>(mesh.face_count) * 3);
  if (!state.indices.empty()) {
    std::memcpy(state.indices.data(), mesh.indices.data(), state.indices.size() * sizeof(uint32_t));
  }

  for (const MeshChannel& channel : mesh.channels) {
    if (channel.count != 1) {
      continue;  // vectors (normal, uv) are not coloured by
    }
    const size_t elements = channel.target == Target::kVertex ? mesh.vertex_count : mesh.face_count;
    state.channels.push_back(
        {.name = channel.name,
         .target = channel.target,
         .datatype = channel.datatype,
         .info = channel.info,
         .values = readValues(channel, elements),
         .dirty = {}});
  }
  return state;
}

MeshMapUpdateResult applyMeshMapUpdate(MeshMapState& state, const PJ::sdk::MeshMapUpdate& update) {
  MeshMapUpdateResult result;
  if (!update.geometry_id.empty() && !state.geometry_id.empty() && update.geometry_id != state.geometry_id) {
    return result;
  }
  result.applied = true;
  for (const PJ::sdk::MeshChannelPatch& patch : update.patches) {
    const MeshChannel& channel = patch.channel;
    const size_t elements = channel.target == Target::kFace ? state.faceCount() : state.vertexCount();
    const size_t element_bytes = PJ::sdk::bytesPerElement(channel.datatype);
    if (channel.count != 1 || element_bytes == 0) {
      ++result.skipped_patches;
      continue;
    }
    const size_t values = channel.data.size() / element_bytes;
    const bool dense = patch.indices.empty();
    if (dense && values != elements) {
      ++result.skipped_patches;  // a dense patch for another mesh size
      continue;
    }

    MeshMapChannelState* target = state.find(channel.target, channel.name);
    if (target == nullptr) {
      MeshMapChannelState created;
      created.name = channel.name;
      created.target = channel.target;
      created.datatype = channel.datatype;  // a patch never retypes an existing channel
      state.channels.push_back(std::move(created));
      target = &state.channels.back();
    }
    // New semantics re-colour every element, as a dense patch or a new channel does.
    bool all_dirty = dense || target->info != channel.info;
    target->info = channel.info;
    if (target->values.size() != elements) {
      target->values.assign(elements, kNaN);  // a new channel: "no data" where the patch is silent
      all_dirty = true;
    }
    for (size_t k = 0; k < values; ++k) {
      uint32_t index = static_cast<uint32_t>(k);
      if (!dense) {
        std::memcpy(&index, patch.indices.data() + (k * sizeof(uint32_t)), sizeof(index));
      }
      if (index >= elements) {
        ++result.skipped_indices;
        continue;
      }
      target->values[index] = readValue(channel.data.data(), channel.datatype, channel.info, k);
      if (!all_dirty) {
        target->dirty.add(index);
      }
    }
    if (all_dirty) {
      target->dirty = {.begin = 0, .end = static_cast<uint32_t>(elements)};
    }
  }
  return result;
}

void clearMeshMapDirty(MeshMapState& state) {
  for (MeshMapChannelState& channel : state.channels) {
    channel.dirty = {};
  }
}

std::string meshMapChannelKey(Target target, std::string_view name) {
  return std::string(target == Target::kFace ? "face:" : "vertex:") + std::string(name);
}

std::vector<std::string> meshMapColorSources(const MeshMapState& state) {
  std::vector<std::string> sources;
  for (const Target target : {Target::kFace, Target::kVertex}) {
    for (const MeshMapChannelState& channel : state.channels) {
      if (channel.target == target) {
        sources.push_back(meshMapChannelKey(channel.target, channel.name));
      }
    }
  }
  if (hasVertexColor(state)) {
    sources.emplace_back(kMeshMapSourceVertexColor);
  }
  sources.emplace_back(kMeshMapSourceSolid);
  return sources;
}

std::string defaultMeshMapColorSource(const MeshMapState& state) {
  for (const Target target : {Target::kFace, Target::kVertex}) {
    for (const MeshMapChannelState& channel : state.channels) {
      if (channel.target == target && channel.info.kind == ChannelKind::kContinuous && !isColorComponent(channel)) {
        return meshMapChannelKey(channel.target, channel.name);
      }
    }
  }
  if (hasVertexColor(state)) {
    return std::string(kMeshMapSourceVertexColor);
  }
  for (const MeshMapChannelState& channel : state.channels) {
    if (!isColorComponent(channel)) {
      return meshMapChannelKey(channel.target, channel.name);
    }
  }
  return std::string(kMeshMapSourceSolid);
}

uint32_t categoricalColor(int64_t value) {
  // A ten-colour qualitative palette (Tableau 10), cycled.
  static constexpr std::array<uint32_t, 10> kPalette = {
      packRgba(0x4E, 0x79, 0xA7, 0xFF), packRgba(0xF2, 0x8E, 0x2B, 0xFF), packRgba(0xE1, 0x57, 0x59, 0xFF),
      packRgba(0x76, 0xB7, 0xB2, 0xFF), packRgba(0x59, 0xA1, 0x4F, 0xFF), packRgba(0xED, 0xC9, 0x48, 0xFF),
      packRgba(0xB0, 0x7A, 0xA1, 0xFF), packRgba(0xFF, 0x9D, 0xA7, 0xFF), packRgba(0x9C, 0x75, 0x5F, 0xFF),
      packRgba(0xBA, 0xB0, 0xAC, 0xFF)};
  const auto index = static_cast<size_t>(value < 0 ? -(value + 1) : value) % kPalette.size();
  return kPalette[index];
}

void fillMeshMapGeometry(const MeshMapState& state, MeshMapUpload& upload) {
  upload.frame_id = state.frame_id;
  upload.origin = state.origin;
  upload.positions.resize(state.indices.size() * 3);
  for (size_t i = 0; i < state.indices.size(); ++i) {
    std::memcpy(
        &upload.positions[3 * i], &state.positions[3 * static_cast<size_t>(state.indices[i])], 3 * sizeof(float));
  }
  upload.link_positions.clear();
  upload.link_colors.clear();
  const auto centroid = [&state](uint32_t face, float* out) {
    for (size_t axis = 0; axis < 3; ++axis) {
      float sum = 0.0F;
      for (size_t corner = 0; corner < 3; ++corner) {
        const size_t vertex = state.indices[(3 * static_cast<size_t>(face)) + corner];
        sum += state.positions[(3 * vertex) + axis];
      }
      out[axis] = sum / 3.0F;
    }
  };
  for (const PJ::sdk::MeshFaceLink& link : state.links) {
    if (link.from_face >= state.faceCount() || link.to_face >= state.faceCount()) {
      continue;
    }
    const size_t offset = upload.link_positions.size();
    upload.link_positions.resize(offset + 6);
    centroid(link.from_face, &upload.link_positions[offset]);
    centroid(link.to_face, &upload.link_positions[offset + 3]);
    upload.link_colors.push_back(categoricalColor(link.kind));
    upload.link_colors.push_back(categoricalColor(link.kind));
  }
}

void fillMeshMapColors(const MeshMapState& state, std::string_view source, MeshMapUpload& upload) {
  upload.values.clear();
  upload.colors.clear();
  upload.color_range = {};
  upload.color_mode = MeshMapColorMode::kSolid;
  upload.translucent = false;
  const size_t corners = state.indices.size();

  if (const MeshMapChannelState* channel = channelForKey(state, source); channel != nullptr) {
    if (channel->info.kind == ChannelKind::kContinuous) {
      upload.color_mode = MeshMapColorMode::kChannel;
      upload.values.resize(corners);
    } else {
      upload.color_mode = MeshMapColorMode::kRgba;
      upload.colors.resize(corners);
    }
    colorFaces(state, *channel, 0, corners / 3, upload);
    return;
  }

  if (source == kMeshMapSourceVertexColor && hasVertexColor(state)) {
    const MeshMapChannelState* red = colorComponent(state, "red");
    const MeshMapChannelState* green = colorComponent(state, "green");
    const MeshMapChannelState* blue = colorComponent(state, "blue");
    const MeshMapChannelState* alpha = colorComponent(state, "alpha");
    upload.color_mode = MeshMapColorMode::kRgba;
    upload.colors.resize(corners);
    for (size_t i = 0; i < corners; ++i) {
      const uint32_t v = state.indices[i];
      const uint8_t a = alpha != nullptr ? colorByte(alpha->values[v]) : uint8_t{255};
      upload.colors[i] =
          packRgba(colorByte(red->values[v]), colorByte(green->values[v]), colorByte(blue->values[v]), a);
      upload.translucent = upload.translucent || a < 255;
    }
  }
}

std::optional<ElementRange> refreshMeshMapColors(
    const MeshMapState& state, std::string_view source, MeshMapUpload& upload) {
  const size_t corners = state.indices.size();
  if (const MeshMapChannelState* channel = channelForKey(state, source); channel != nullptr) {
    const bool continuous = channel->info.kind == ChannelKind::kContinuous;
    const bool same_mode = continuous
                               ? upload.color_mode == MeshMapColorMode::kChannel && upload.values.size() == corners
                               : upload.color_mode == MeshMapColorMode::kRgba && upload.colors.size() == corners;
    if (!same_mode) {
      return std::nullopt;
    }
    if (channel->dirty.empty()) {
      return ElementRange{};
    }
    // ponytail: a dirty vertex channel re-colours whole; a vertex -> faces index
    // would bound it, if vertex-channel updates on large meshes turn out to matter.
    if (channel->target != Target::kFace) {
      return std::nullopt;
    }
    const uint32_t begin = channel->dirty.begin;
    const uint32_t end = std::min(channel->dirty.end, state.faceCount());
    colorFaces(state, *channel, begin, end, upload);
    return ElementRange{.begin = 3 * begin, .end = 3 * end};
  }
  if (source == kMeshMapSourceVertexColor && hasVertexColor(state)) {
    const bool untouched = std::all_of(kColorComponents.begin(), kColorComponents.end(), [&state](auto name) {
      const MeshMapChannelState* component = state.find(Target::kVertex, name);
      return component == nullptr || component->dirty.empty();
    });
    return untouched && upload.color_mode == MeshMapColorMode::kRgba ? std::optional(ElementRange{}) : std::nullopt;
  }
  return upload.color_mode == MeshMapColorMode::kSolid ? std::optional(ElementRange{}) : std::nullopt;
}

AABB meshMapBounds(const MeshMapState& state) {
  AABB local;
  for (size_t v = 0; v + 2 < state.positions.size(); v += 3) {
    const glm::vec3 p(state.positions[v], state.positions[v + 1], state.positions[v + 2]);
    if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) {
      expandAABB(local, p);
    }
  }
  const std::optional<glm::dmat4> origin = poseMatrix(state.origin);
  if (!local.valid || !origin.has_value()) {
    return {};
  }
  // In double: a float64 map's origin carries its UTM-scale offset.
  AABB out;
  for (int corner = 0; corner < 8; ++corner) {
    const glm::dvec3 p(
        (corner & 1) != 0 ? local.max.x : local.min.x, (corner & 2) != 0 ? local.max.y : local.min.y,
        (corner & 4) != 0 ? local.max.z : local.min.z);
    expandAABB(out, glm::vec3(*origin * glm::dvec4(p, 1.0)));
  }
  return out;
}

}  // namespace pj::scene3d
