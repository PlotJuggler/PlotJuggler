// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/mesh_map_view.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <glm/glm.hpp>
#include <limits>
#include <optional>
#include <utility>

#include "pj_scene3d_core/pointcloud_convert.h"     // readScalarAt
#include "pj_scene3d_core/scene_entities_decode.h"  // poseToMat4

namespace pj::scene3d {

namespace {

using PJ::sdk::ChannelKind;
using PJ::sdk::MeshChannel;
using Target = PJ::sdk::MeshChannel::Target;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

constexpr std::array<std::string_view, 4> kColorComponents = {"red", "green", "blue", "alpha"};

bool isColorComponent(const MeshMapChannelState& channel) {
  return channel.target == Target::kVertex &&
         std::find(kColorComponents.begin(), kColorComponents.end(), channel.name) != kColorComponents.end();
}

// Element `i` of a single-component channel, widened to float, "no data" -> NaN.
float readValue(
    const uint8_t* data, PJ::sdk::PointField::Datatype datatype, const PJ::sdk::ChannelInfo& info, size_t i) {
  const float value = readScalarAt(data + (i * PJ::sdk::bytesPerElement(datatype)), datatype);
  if (info.no_data.has_value() && static_cast<double>(value) == *info.no_data) {
    return kNaN;
  }
  return value;
}

// The channel's values in the state's representation.
std::vector<float> readValues(const MeshChannel& channel, size_t elements) {
  std::vector<float> values(elements);
  for (size_t i = 0; i < elements; ++i) {
    values[i] = readValue(channel.data.data(), channel.datatype, channel.info, i);
  }
  return values;
}

constexpr uint32_t packRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8U) | (static_cast<uint32_t>(b) << 16U) |
         (static_cast<uint32_t>(a) << 24U);
}

uint8_t colorByte(float value) {
  return std::isfinite(value) ? static_cast<uint8_t>(std::clamp(value, 0.0F, 255.0F)) : uint8_t{0};
}

constexpr uint32_t kNoDataColor = 0xFF808080U;  // mid grey, opaque

void accumulate(FiniteRange& range, float value) {
  if (!std::isfinite(value)) {
    return;
  }
  if (!range.valid) {
    range = {.lo = value, .hi = value, .valid = true};
  } else {
    range.lo = std::min(range.lo, value);
    range.hi = std::max(range.hi, value);
  }
}

// The value of `channel` at corner `i` of the de-indexed mesh.
float cornerValue(const MeshMapState& state, const MeshMapChannelState& channel, size_t i) {
  return channel.target == Target::kFace ? channel.values[i / 3] : channel.values[state.indices[i]];
}

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
  return state.find(Target::kVertex, "red") != nullptr && state.find(Target::kVertex, "green") != nullptr &&
         state.find(Target::kVertex, "blue") != nullptr;
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
  for (size_t i = 0; i < coordinates; ++i) {
    state.positions[i] =
        readScalarAt(mesh.positions.data() + (i * PJ::sdk::bytesPerElement(mesh.position_type)), mesh.position_type);
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
         .info = channel.info,
         .values = readValues(channel, elements)});
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
      state.channels.push_back(std::move(created));
      target = &state.channels.back();
    }
    target->info = channel.info;
    if (target->values.size() != elements) {
      target->values.assign(elements, kNaN);  // a new channel: "no data" where the patch is silent
    }
    for (size_t k = 0; k < values; ++k) {
      uint32_t index = static_cast<uint32_t>(k);
      if (!dense) {
        std::memcpy(&index, patch.indices.data() + (k * sizeof(uint32_t)), sizeof(index));
      }
      if (index >= elements) {
        ++result.skipped_records;
        continue;
      }
      target->values[index] = readValue(channel.data.data(), channel.datatype, channel.info, k);
    }
  }
  return result;
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
  const size_t corners = state.indices.size();

  if (const MeshMapChannelState* channel = channelForKey(state, source); channel != nullptr) {
    if (channel->info.kind == ChannelKind::kContinuous) {
      upload.color_mode = MeshMapColorMode::kChannel;
      upload.values.resize(corners);
      for (size_t face = 0; face * 3 < corners; ++face) {
        std::array<float, 3> values{};
        bool valid = true;
        for (size_t corner = 0; corner < 3; ++corner) {
          values[corner] = cornerValue(state, *channel, (3 * face) + corner);
          valid = valid && std::isfinite(values[corner]);
        }
        for (size_t corner = 0; corner < 3; ++corner) {
          upload.values[(3 * face) + corner] = valid ? values[corner] : kNaN;
          if (valid) {
            accumulate(upload.color_range, values[corner]);
          }
        }
      }
      return;
    }
    // Categorical / bitmask: the producer's category colour, else the palette.
    upload.color_mode = MeshMapColorMode::kRgba;
    upload.colors.resize(corners);
    for (size_t i = 0; i < corners; ++i) {
      const float value = cornerValue(state, *channel, i);
      if (!std::isfinite(value)) {
        upload.colors[i] = kNoDataColor;
        continue;
      }
      const auto id = static_cast<int64_t>(std::llround(value));
      uint32_t color = categoricalColor(id);
      for (const PJ::sdk::ChannelCategory& category : channel->info.categories) {
        if (category.value == id && category.color.has_value()) {
          color = packRgba(category.color->r, category.color->g, category.color->b, category.color->a);
          break;
        }
      }
      upload.colors[i] = color;
    }
    return;
  }

  if (source == kMeshMapSourceVertexColor && hasVertexColor(state)) {
    const MeshMapChannelState* red = state.find(Target::kVertex, "red");
    const MeshMapChannelState* green = state.find(Target::kVertex, "green");
    const MeshMapChannelState* blue = state.find(Target::kVertex, "blue");
    const MeshMapChannelState* alpha = state.find(Target::kVertex, "alpha");
    upload.color_mode = MeshMapColorMode::kRgba;
    upload.colors.resize(corners);
    for (size_t i = 0; i < corners; ++i) {
      const uint32_t v = state.indices[i];
      upload.colors[i] = packRgba(
          colorByte(red->values[v]), colorByte(green->values[v]), colorByte(blue->values[v]),
          alpha != nullptr ? colorByte(alpha->values[v]) : uint8_t{255});
    }
  }
}

AABB meshMapBounds(const MeshMapState& state) {
  AABB local;
  for (size_t v = 0; v + 2 < state.positions.size(); v += 3) {
    const glm::vec3 p(state.positions[v], state.positions[v + 1], state.positions[v + 2]);
    if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) {
      expandAABB(local, p);
    }
  }
  if (!local.valid) {
    return {};
  }
  return transformedAABB(poseToMat4(state.origin), local);
}

}  // namespace pj::scene3d
