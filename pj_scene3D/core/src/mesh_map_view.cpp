// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_core/mesh_map_view.h"

#include <array>
#include <cmath>
#include <cstring>
#include <glm/glm.hpp>
#include <limits>
#include <string_view>

#include "pj_scene3d_core/pointcloud_convert.h"     // readScalarAt
#include "pj_scene3d_core/scene_entities_decode.h"  // poseToMat4

namespace pj::scene3d {

namespace {

using PJ::sdk::PointField;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

bool isChannelField(const PointField& field) {
  return field.count == 1 && PJ::sdk::bytesPerElement(field.datatype) > 0;
}

bool isReservedVertexField(std::string_view name) {
  return name == "x" || name == "y" || name == "z";
}

// Every channel field of `fields` read from `count` records into float arrays.
void readChannels(
    const std::vector<PointField>& fields, const uint8_t* records, uint32_t count, uint32_t stride,
    std::string_view skip, std::map<std::string, std::vector<float>>& out) {
  for (const PointField& field : fields) {
    if (!isChannelField(field) || field.name == skip || isReservedVertexField(field.name) ||
        field.name == PJ::sdk::kMeshMapVertexIndicesField) {
      continue;
    }
    std::vector<float>& values = out[field.name];
    values.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      values[i] = readScalarAt(records + (static_cast<size_t>(i) * stride) + field.offset, field.datatype);
    }
  }
}

uint8_t colorByte(float value) {
  return std::isfinite(value) ? static_cast<uint8_t>(std::clamp(value, 0.0F, 255.0F)) : uint8_t{0};
}

uint32_t packRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8U) | (static_cast<uint32_t>(b) << 16U) |
         (static_cast<uint32_t>(a) << 24U);
}

// Link colours by kind: generic white, then ramp, stairs, elevator, door (the
// NavMap convention, the most common producer); unknown kinds cycle.
uint32_t linkColor(uint32_t kind) {
  static constexpr std::array<uint32_t, 5> kPalette = {
      0xFFFFFFFFU,  // white
      0xFF0099FFU,  // orange (ABGR packing: R in the low byte)
      0xFFE633E6U,  // magenta
      0xFFFFCC00U,  // cyan
      0xFF00E6FFU,  // yellow
  };
  return kPalette[kind % kPalette.size()];
}

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

}  // namespace

MeshMapState decodeMeshMap(const PJ::sdk::MeshMap& mesh) {
  MeshMapState state;
  state.frame_id = mesh.frame_id;
  state.origin = mesh.origin;
  state.geometry_id = mesh.geometry_id;
  state.links = mesh.links;

  const uint8_t* vertices = mesh.data.data();
  if (mesh.vertex_count > 0) {
    const PointField* x = PJ::sdk::findField(mesh.vertex_fields, "x");
    const PointField* y = PJ::sdk::findField(mesh.vertex_fields, "y");
    const PointField* z = PJ::sdk::findField(mesh.vertex_fields, "z");
    state.positions.resize(static_cast<size_t>(mesh.vertex_count) * 3);
    for (uint32_t v = 0; v < mesh.vertex_count; ++v) {
      const uint8_t* record = vertices + (static_cast<size_t>(v) * mesh.vertex_stride);
      state.positions[(3 * v) + 0] = readScalarAt(record + x->offset, x->datatype);
      state.positions[(3 * v) + 1] = readScalarAt(record + y->offset, y->datatype);
      state.positions[(3 * v) + 2] = readScalarAt(record + z->offset, z->datatype);
    }
    readChannels(mesh.vertex_fields, vertices, mesh.vertex_count, mesh.vertex_stride, {}, state.vertex_channels);
  }

  const auto red = state.vertex_channels.find("red");
  const auto green = state.vertex_channels.find("green");
  const auto blue = state.vertex_channels.find("blue");
  if (red != state.vertex_channels.end() && green != state.vertex_channels.end() &&
      blue != state.vertex_channels.end()) {
    const auto alpha = state.vertex_channels.find("alpha");
    state.vertex_colors.resize(mesh.vertex_count);
    for (uint32_t v = 0; v < mesh.vertex_count; ++v) {
      const uint8_t a = alpha != state.vertex_channels.end() ? colorByte(alpha->second[v]) : uint8_t{255};
      state.vertex_colors[v] =
          packRgba(colorByte(red->second[v]), colorByte(green->second[v]), colorByte(blue->second[v]), a);
    }
  }

  if (mesh.face_count > 0) {
    const uint8_t* faces = mesh.data.data() + mesh.face_data_offset;
    const PointField* indices = PJ::sdk::findField(mesh.face_fields, PJ::sdk::kMeshMapVertexIndicesField);
    state.indices.resize(static_cast<size_t>(mesh.face_count) * 3);
    for (uint32_t f = 0; f < mesh.face_count; ++f) {
      std::memcpy(
          &state.indices[3 * static_cast<size_t>(f)],
          faces + (static_cast<size_t>(f) * mesh.face_stride) + indices->offset, 3 * sizeof(uint32_t));
    }
    readChannels(mesh.face_fields, faces, mesh.face_count, mesh.face_stride, {}, state.face_channels);
  }
  return state;
}

MeshMapUpdateResult applyMeshMapUpdate(MeshMapState& state, const PJ::sdk::MeshMapUpdate& update) {
  MeshMapUpdateResult result;
  if (!update.geometry_id.empty() && !state.geometry_id.empty() && update.geometry_id != state.geometry_id) {
    return result;
  }
  result.applied = true;
  const bool faces = update.target == PJ::sdk::MeshMapUpdate::Target::kFaces;
  const uint32_t element_count = faces ? state.faceCount() : state.vertexCount();
  auto& channels = faces ? state.face_channels : state.vertex_channels;
  const PointField* index_field = PJ::sdk::findField(update.fields, PJ::sdk::kMeshMapUpdateIndexField);
  if (index_field == nullptr || update.count == 0) {
    return result;
  }

  std::vector<uint32_t> indices(update.count);
  for (uint32_t k = 0; k < update.count; ++k) {
    std::memcpy(
        &indices[k], update.data.data() + (static_cast<size_t>(k) * update.stride) + index_field->offset,
        sizeof(uint32_t));
    if (indices[k] >= element_count) {
      ++result.skipped_records;
    }
  }
  for (const PointField& field : update.fields) {
    if (!isChannelField(field) || field.name == PJ::sdk::kMeshMapUpdateIndexField) {
      continue;
    }
    std::vector<float>& values = channels[field.name];
    if (values.size() != element_count) {
      values.assign(element_count, kNaN);  // a new channel: "no data" where the update is silent
    }
    for (uint32_t k = 0; k < update.count; ++k) {
      if (indices[k] < element_count) {
        values[indices[k]] =
            readScalarAt(update.data.data() + (static_cast<size_t>(k) * update.stride) + field.offset, field.datatype);
      }
    }
  }
  return result;
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
    upload.link_colors.push_back(linkColor(link.kind));
    upload.link_colors.push_back(linkColor(link.kind));
  }
}

void fillMeshMapColors(
    const MeshMapState& state, MeshMapColorMode mode, const std::string& channel, MeshMapUpload& upload) {
  upload.values.clear();
  upload.colors.clear();
  upload.color_range = {};
  const size_t corners = state.indices.size();

  if (mode == MeshMapColorMode::kChannel) {
    if (const auto face = state.face_channels.find(channel); face != state.face_channels.end()) {
      upload.color_mode = MeshMapColorMode::kChannel;
      upload.values.resize(corners);
      for (size_t i = 0; i < corners; ++i) {
        const float value = face->second[i / 3];
        upload.values[i] = value;
        accumulate(upload.color_range, value);
      }
      return;
    }
    if (const auto vertex = state.vertex_channels.find(channel); vertex != state.vertex_channels.end()) {
      upload.color_mode = MeshMapColorMode::kChannel;
      upload.values.resize(corners);
      for (size_t i = 0; i < corners; ++i) {
        const float value = vertex->second[state.indices[i]];
        upload.values[i] = value;
        accumulate(upload.color_range, value);
      }
      return;
    }
  }
  if (mode != MeshMapColorMode::kSolid && !state.vertex_colors.empty()) {
    upload.color_mode = MeshMapColorMode::kVertexColor;
    upload.colors.resize(corners);
    for (size_t i = 0; i < corners; ++i) {
      upload.colors[i] = state.vertex_colors[state.indices[i]];
    }
    return;
  }
  upload.color_mode = MeshMapColorMode::kSolid;
}

std::string defaultMeshMapChannel(const MeshMapState& state) {
  for (const auto& [name, values] : state.face_channels) {
    (void)values;
    if (name != "surface") {
      return name;
    }
  }
  for (const auto& [name, values] : state.vertex_channels) {
    (void)values;
    if (name != "red" && name != "green" && name != "blue" && name != "alpha") {
      return name;
    }
  }
  return {};
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
