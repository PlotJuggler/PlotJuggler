// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Qt-free coverage of the MeshMap CPU path: decoding positions, indices, face
// and vertex channels and the vertex colour; applying MeshMapUpdates (geometry
// id check, new channels, out-of-range records); the de-indexed upload in each
// colour mode; link segments; the default channel and the bounds.

#include "pj_scene3d_core/mesh_map_view.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "pj_base/builtin/mesh_map_codec.hpp"

namespace {

using namespace pj::scene3d;
using PJ::sdk::MeshMap;
using PJ::sdk::MeshMapUpdate;
using PJ::sdk::PointField;
using Datatype = PJ::sdk::PointField::Datatype;

template <typename T>
void append(std::vector<uint8_t>& out, T value) {
  const auto offset = out.size();
  out.resize(offset + sizeof(T));
  std::memcpy(out.data() + offset, &value, sizeof(T));
}

// A unit square: vertices (0,0,0) (1,0,0) (1,1,2) (0,1,0); faces (0,1,2) and
// (0,2,3). Vertex record: x, y, z float32 + red, green, blue uint8 + pad.
// Face record: vertex_indices + surface uint32 + cost float32.
struct Fixture {
  std::vector<uint8_t> bytes;
  MeshMap mesh;

  Fixture() {
    const float xyz[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 2}, {0, 1, 0}};
    for (int v = 0; v < 4; ++v) {
      for (float c : xyz[v]) {
        append(bytes, c);
      }
      append(bytes, static_cast<uint8_t>(10 * (v + 1)));  // red
      append(bytes, uint8_t{0});                          // green
      append(bytes, uint8_t{200});                        // blue
      append(bytes, uint8_t{0});                          // pad
    }
    const uint32_t faces[2][3] = {{0, 1, 2}, {0, 2, 3}};
    const float cost[2] = {1.5F, 4.0F};
    for (int f = 0; f < 2; ++f) {
      for (uint32_t i : faces[f]) {
        append(bytes, i);
      }
      append(bytes, uint32_t{0});  // surface
      append(bytes, cost[f]);
    }
    mesh.frame_id = "map";
    mesh.geometry_id = "7";
    mesh.vertex_count = 4;
    mesh.vertex_stride = 16;
    mesh.vertex_fields = {
        {.name = "x", .offset = 0, .datatype = Datatype::kFloat32, .count = 1},
        {.name = "y", .offset = 4, .datatype = Datatype::kFloat32, .count = 1},
        {.name = "z", .offset = 8, .datatype = Datatype::kFloat32, .count = 1},
        {.name = "red", .offset = 12, .datatype = Datatype::kUint8, .count = 1},
        {.name = "green", .offset = 13, .datatype = Datatype::kUint8, .count = 1},
        {.name = "blue", .offset = 14, .datatype = Datatype::kUint8, .count = 1},
    };
    mesh.face_count = 2;
    mesh.face_stride = 20;
    mesh.face_data_offset = 64;
    mesh.face_fields = {
        {.name = "vertex_indices", .offset = 0, .datatype = Datatype::kUint32, .count = 3},
        {.name = "surface", .offset = 12, .datatype = Datatype::kUint32, .count = 1},
        {.name = "cost", .offset = 16, .datatype = Datatype::kFloat32, .count = 1},
    };
    mesh.links = {{.from_face = 0, .to_face = 1, .kind = 2, .bidirectional = true}};
    mesh.data = PJ::Span<const uint8_t>(bytes.data(), bytes.size());
  }
};

MeshMapUpdate makeUpdate(
    std::vector<uint8_t>& bytes, const std::string& channel, const std::vector<std::pair<uint32_t, float>>& records) {
  MeshMapUpdate update;
  update.geometry_id = "7";
  update.target = MeshMapUpdate::Target::kFaces;
  update.count = static_cast<uint32_t>(records.size());
  update.stride = 8;
  update.fields = {
      {.name = "index", .offset = 0, .datatype = Datatype::kUint32, .count = 1},
      {.name = channel, .offset = 4, .datatype = Datatype::kFloat32, .count = 1},
  };
  bytes.clear();
  for (const auto& [index, value] : records) {
    append(bytes, index);
    append(bytes, value);
  }
  update.data = PJ::Span<const uint8_t>(bytes.data(), bytes.size());
  return update;
}

TEST(MeshMapViewTest, FixtureIsAValidMeshMap) {
  Fixture fixture;
  EXPECT_TRUE(PJ::validateMeshMap(fixture.mesh));
}

TEST(MeshMapViewTest, DecodesGeometryChannelsAndColors) {
  Fixture fixture;
  const MeshMapState state = decodeMeshMap(fixture.mesh);
  EXPECT_EQ(state.frame_id, "map");
  ASSERT_EQ(state.vertexCount(), 4u);
  ASSERT_EQ(state.faceCount(), 2u);
  EXPECT_FLOAT_EQ(state.positions[(2 * 3) + 2], 2.0F);
  EXPECT_EQ(state.indices, (std::vector<uint32_t>{0, 1, 2, 0, 2, 3}));
  ASSERT_TRUE(state.face_channels.contains("cost"));
  ASSERT_TRUE(state.face_channels.contains("surface"));
  EXPECT_FALSE(state.face_channels.contains("vertex_indices"));
  EXPECT_FLOAT_EQ(state.face_channels.at("cost")[1], 4.0F);
  EXPECT_FALSE(state.vertex_channels.contains("x"));
  ASSERT_TRUE(state.vertex_channels.contains("red"));
  ASSERT_EQ(state.vertex_colors.size(), 4u);
  EXPECT_EQ(state.vertex_colors[2], 30U | (0U << 8U) | (200U << 16U) | (255U << 24U));  // no alpha: opaque
  EXPECT_EQ(defaultMeshMapChannel(state), "cost");                                      // not "surface"
}

TEST(MeshMapViewTest, UploadDeIndexesEveryColorMode) {
  Fixture fixture;
  const MeshMapState state = decodeMeshMap(fixture.mesh);
  MeshMapUpload upload;
  fillMeshMapGeometry(state, upload);
  ASSERT_EQ(upload.positions.size(), 18u);
  EXPECT_FLOAT_EQ(upload.positions[(5 * 3) + 1], 1.0F);  // face 1 corner 2 = vertex 3 (0,1,0)

  fillMeshMapColors(state, MeshMapColorMode::kChannel, "cost", upload);
  EXPECT_EQ(upload.color_mode, MeshMapColorMode::kChannel);
  EXPECT_EQ(upload.values, (std::vector<float>{1.5F, 1.5F, 1.5F, 4.0F, 4.0F, 4.0F}));  // flat per face
  EXPECT_TRUE(upload.color_range.valid);
  EXPECT_FLOAT_EQ(upload.color_range.lo, 1.5F);
  EXPECT_FLOAT_EQ(upload.color_range.hi, 4.0F);

  fillMeshMapColors(state, MeshMapColorMode::kChannel, "red", upload);  // a vertex channel interpolates
  EXPECT_EQ(upload.values, (std::vector<float>{10, 20, 30, 10, 30, 40}));

  fillMeshMapColors(state, MeshMapColorMode::kVertexColor, "", upload);
  EXPECT_EQ(upload.color_mode, MeshMapColorMode::kVertexColor);
  ASSERT_EQ(upload.colors.size(), 6u);
  EXPECT_EQ(upload.colors[5] & 0xFFU, 40U);
  EXPECT_TRUE(upload.values.empty());

  fillMeshMapColors(state, MeshMapColorMode::kChannel, "missing", upload);  // falls back to the vertex colour
  EXPECT_EQ(upload.color_mode, MeshMapColorMode::kVertexColor);

  fillMeshMapColors(state, MeshMapColorMode::kSolid, "", upload);
  EXPECT_EQ(upload.color_mode, MeshMapColorMode::kSolid);
  EXPECT_TRUE(upload.colors.empty());
}

TEST(MeshMapViewTest, LinksBecomeCentroidSegments) {
  Fixture fixture;
  MeshMapUpload upload;
  fillMeshMapGeometry(decodeMeshMap(fixture.mesh), upload);
  ASSERT_EQ(upload.link_positions.size(), 6u);
  EXPECT_FLOAT_EQ(upload.link_positions[0], 2.0F / 3.0F);  // centroid of face 0, x
  EXPECT_FLOAT_EQ(upload.link_positions[2], 2.0F / 3.0F);  // z: (0 + 0 + 2) / 3
  EXPECT_FLOAT_EQ(upload.link_positions[3], 1.0F / 3.0F);  // centroid of face 1, x
  ASSERT_EQ(upload.link_colors.size(), 2u);
  EXPECT_EQ(upload.link_colors[0], upload.link_colors[1]);
}

TEST(MeshMapViewTest, AppliesUpdatesAndRefusesAnotherGeometry) {
  Fixture fixture;
  MeshMapState state = decodeMeshMap(fixture.mesh);
  std::vector<uint8_t> bytes;

  auto result = applyMeshMapUpdate(state, makeUpdate(bytes, "cost", {{1, 9.0F}, {5, 1.0F}}));
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.skipped_records, 1u);  // index 5 is past the 2 faces
  EXPECT_FLOAT_EQ(state.face_channels.at("cost")[0], 1.5F);
  EXPECT_FLOAT_EQ(state.face_channels.at("cost")[1], 9.0F);

  // A channel the mesh lacks is created, "no data" where the update is silent.
  result = applyMeshMapUpdate(state, makeUpdate(bytes, "occupancy", {{0, 0.25F}}));
  EXPECT_TRUE(result.applied);
  ASSERT_TRUE(state.face_channels.contains("occupancy"));
  EXPECT_FLOAT_EQ(state.face_channels.at("occupancy")[0], 0.25F);
  EXPECT_TRUE(std::isnan(state.face_channels.at("occupancy")[1]));

  MeshMapUpdate other = makeUpdate(bytes, "cost", {{0, 100.0F}});
  other.geometry_id = "8";
  EXPECT_FALSE(applyMeshMapUpdate(state, other).applied);
  EXPECT_FLOAT_EQ(state.face_channels.at("cost")[0], 1.5F);

  other.geometry_id.clear();  // unspecified applies to any geometry
  EXPECT_TRUE(applyMeshMapUpdate(state, other).applied);
  EXPECT_FLOAT_EQ(state.face_channels.at("cost")[0], 100.0F);

  MeshMapUpdate vertices = makeUpdate(bytes, "height", {{3, 2.0F}});
  vertices.target = MeshMapUpdate::Target::kVertices;
  EXPECT_TRUE(applyMeshMapUpdate(state, vertices).applied);
  EXPECT_FLOAT_EQ(state.vertex_channels.at("height")[3], 2.0F);
  EXPECT_FALSE(state.face_channels.contains("height"));
}

TEST(MeshMapViewTest, BoundsFollowTheOriginPose) {
  Fixture fixture;
  fixture.mesh.origin.position = {.x = 10.0, .y = 0.0, .z = 0.0};
  const AABB box = meshMapBounds(decodeMeshMap(fixture.mesh));
  ASSERT_TRUE(box.valid);
  EXPECT_FLOAT_EQ(box.min.x, 10.0F);
  EXPECT_FLOAT_EQ(box.max.x, 11.0F);
  EXPECT_FLOAT_EQ(box.max.z, 2.0F);
  EXPECT_FALSE(meshMapBounds(MeshMapState{}).valid);
}

}  // namespace
