// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Qt-free coverage of the MeshMap CPU path: decoding positions, indices and the
// single-component channels (integer "no data" folded into NaN); applying
// MeshMapUpdates (geometry id, dense and sparse patches, new channels, records
// past the end, vertex-colour updates); the colour sources and the default pick
// (categorical channels and colour components aside); the de-indexed upload in
// each colour mode, with a face that has "no data" at one corner drawn as no
// data; the categorical palette; link segments and the bounds.

#include "pj_scene3d_core/mesh_map_view.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/builtin/mesh_map_codec.hpp"
#include "pj_base/builtin/mesh_map_update_codec.hpp"

namespace {

using namespace pj::scene3d;
using PJ::sdk::ChannelKind;
using PJ::sdk::MeshChannel;
using PJ::sdk::MeshChannelPatch;
using PJ::sdk::MeshMap;
using PJ::sdk::MeshMapUpdate;
using Datatype = PJ::sdk::PointField::Datatype;
using Target = PJ::sdk::MeshChannel::Target;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// Byte storage outliving the spans the fixture hands out.
struct Bytes {
  std::vector<std::shared_ptr<std::vector<uint8_t>>> blobs;

  template <typename T>
  PJ::Span<const uint8_t> add(const std::vector<T>& values) {
    auto blob = std::make_shared<std::vector<uint8_t>>(values.size() * sizeof(T));
    if (!values.empty()) {
      std::memcpy(blob->data(), values.data(), blob->size());
    }
    blobs.push_back(blob);
    return {blob->data(), blob->size()};
  }
};

MeshChannel makeChannel(
    Bytes& bytes, std::string name, Target target, Datatype datatype, PJ::Span<const uint8_t> data,
    PJ::sdk::ChannelInfo info = {}) {
  MeshChannel channel;
  channel.name = std::move(name);
  channel.target = target;
  channel.datatype = datatype;
  channel.data = data;
  channel.info = std::move(info);
  (void)bytes;
  return channel;
}

// A unit square: vertices (0,0,0) (1,0,0) (1,1,2) (0,1,0); faces (0,1,2) and
// (0,2,3). Channels: face `cost` (float32), face `surface` (categorical uint32),
// vertex `height` (int16, -1 = no data), vertex red/green/blue (uint8).
struct Fixture {
  Bytes bytes;
  MeshMap mesh;

  Fixture() {
    mesh.frame_id = "map";
    mesh.geometry_id = "7";
    mesh.vertex_count = 4;
    mesh.positions = bytes.add(std::vector<float>{0, 0, 0, 1, 0, 0, 1, 1, 2, 0, 1, 0});
    mesh.face_count = 2;
    mesh.indices = bytes.add(std::vector<uint32_t>{0, 1, 2, 0, 2, 3});
    PJ::sdk::ChannelInfo categorical;
    categorical.kind = ChannelKind::kCategorical;
    PJ::sdk::ChannelCategory red_category;
    red_category.value = 1;
    red_category.label = "upstairs";
    red_category.color = PJ::sdk::ColorRGBA{200, 10, 20, 255};
    categorical.categories.push_back(red_category);
    PJ::sdk::ChannelInfo height_info;
    height_info.no_data = -1.0;
    mesh.channels = {
        makeChannel(
            bytes, "surface", Target::kFace, Datatype::kUint32, bytes.add(std::vector<uint32_t>{0, 1}), categorical),
        makeChannel(bytes, "cost", Target::kFace, Datatype::kFloat32, bytes.add(std::vector<float>{1.5F, 4.0F})),
        makeChannel(
            bytes, "height", Target::kVertex, Datatype::kInt16, bytes.add(std::vector<int16_t>{10, 20, -1, 40}),
            height_info),
        makeChannel(bytes, "red", Target::kVertex, Datatype::kUint8, bytes.add(std::vector<uint8_t>{10, 20, 30, 40})),
        makeChannel(bytes, "green", Target::kVertex, Datatype::kUint8, bytes.add(std::vector<uint8_t>{0, 0, 0, 0})),
        makeChannel(
            bytes, "blue", Target::kVertex, Datatype::kUint8, bytes.add(std::vector<uint8_t>{200, 200, 200, 200})),
    };
    PJ::sdk::MeshFaceLink link;
    link.from_face = 0;
    link.to_face = 1;
    link.kind = 2;
    mesh.links = {link};
  }
};

MeshMapUpdate makeUpdate(
    Bytes& bytes, Target target, const std::string& name, const std::vector<float>& values,
    const std::vector<uint32_t>& indices = {}) {
  MeshChannelPatch patch;
  patch.channel = makeChannel(bytes, name, target, Datatype::kFloat32, bytes.add(values));
  if (!indices.empty()) {
    patch.indices = bytes.add(indices);
  }
  MeshMapUpdate update;
  update.geometry_id = "7";
  update.patches.push_back(patch);
  return update;
}

TEST(MeshMapViewTest, FixtureIsAValidMeshMap) {
  Fixture fixture;
  EXPECT_TRUE(PJ::validateMeshMap(fixture.mesh));
}

TEST(MeshMapViewTest, DecodesGeometryAndChannels) {
  Fixture fixture;
  const MeshMapState state = decodeMeshMap(fixture.mesh);
  EXPECT_EQ(state.frame_id, "map");
  ASSERT_EQ(state.vertexCount(), 4u);
  ASSERT_EQ(state.faceCount(), 2u);
  EXPECT_FLOAT_EQ(state.positions[(2 * 3) + 2], 2.0F);
  EXPECT_EQ(state.indices, (std::vector<uint32_t>{0, 1, 2, 0, 2, 3}));
  const MeshMapChannelState* height = state.find(Target::kVertex, "height");
  ASSERT_NE(height, nullptr);
  EXPECT_FLOAT_EQ(height->values[1], 20.0F);
  EXPECT_TRUE(std::isnan(height->values[2]));  // the integer no-data value became NaN
  EXPECT_EQ(state.find(Target::kFace, "height"), nullptr);
}

TEST(MeshMapViewTest, SourcesAndDefaultPick) {
  Fixture fixture;
  const MeshMapState state = decodeMeshMap(fixture.mesh);
  const auto sources = meshMapColorSources(state);
  ASSERT_EQ(sources.front(), "face:surface");
  EXPECT_EQ(sources[1], "face:cost");
  EXPECT_EQ(sources[sources.size() - 2], kMeshMapSourceVertexColor);
  EXPECT_EQ(sources.back(), kMeshMapSourceSolid);
  // The first continuous face channel, not the categorical one listed before it.
  EXPECT_EQ(defaultMeshMapColorSource(state), "face:cost");

  Fixture colors_only;
  colors_only.mesh.channels.erase(colors_only.mesh.channels.begin(), colors_only.mesh.channels.begin() + 3);
  EXPECT_EQ(defaultMeshMapColorSource(decodeMeshMap(colors_only.mesh)), kMeshMapSourceVertexColor);
}

TEST(MeshMapViewTest, UploadDeIndexesEveryColorMode) {
  Fixture fixture;
  const MeshMapState state = decodeMeshMap(fixture.mesh);
  MeshMapUpload upload;
  fillMeshMapGeometry(state, upload);
  ASSERT_EQ(upload.positions.size(), 18u);
  EXPECT_FLOAT_EQ(upload.positions[(5 * 3) + 1], 1.0F);  // face 1 corner 2 = vertex 3 (0,1,0)

  fillMeshMapColors(state, "face:cost", upload);
  EXPECT_EQ(upload.color_mode, MeshMapColorMode::kChannel);
  EXPECT_EQ(upload.values, (std::vector<float>{1.5F, 1.5F, 1.5F, 4.0F, 4.0F, 4.0F}));  // flat per face
  EXPECT_FLOAT_EQ(upload.color_range.lo, 1.5F);
  EXPECT_FLOAT_EQ(upload.color_range.hi, 4.0F);

  // A vertex channel interpolates, but a face touching a no-data vertex is no
  // data at all three corners (vertex 2 is in both faces here).
  fillMeshMapColors(state, "vertex:height", upload);
  ASSERT_EQ(upload.values.size(), 6u);
  for (const float value : upload.values) {
    EXPECT_TRUE(std::isnan(value));
  }
  EXPECT_FALSE(upload.color_range.valid);

  fillMeshMapColors(state, "face:surface", upload);  // categorical: palette / category colour
  EXPECT_EQ(upload.color_mode, MeshMapColorMode::kRgba);
  EXPECT_EQ(upload.colors[0], categoricalColor(0));
  EXPECT_EQ(upload.colors[3] & 0xFFU, 200U);  // the producer's colour for category 1

  fillMeshMapColors(state, kMeshMapSourceVertexColor, upload);
  EXPECT_EQ(upload.color_mode, MeshMapColorMode::kRgba);
  EXPECT_EQ(upload.colors[5] & 0xFFU, 40U);  // vertex 3's red
  EXPECT_EQ(upload.colors[5] >> 24U, 255U);  // no alpha channel: opaque
  EXPECT_TRUE(upload.values.empty());

  fillMeshMapColors(state, "face:missing", upload);
  EXPECT_EQ(upload.color_mode, MeshMapColorMode::kSolid);
  EXPECT_TRUE(upload.colors.empty());
}

TEST(MeshMapViewTest, AppliesPatches) {
  Fixture fixture;
  MeshMapState state = decodeMeshMap(fixture.mesh);
  Bytes bytes;

  // Sparse, with one record past the 2 faces.
  auto result = applyMeshMapUpdate(state, makeUpdate(bytes, Target::kFace, "cost", {9.0F, 1.0F}, {1, 5}));
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.skipped_records, 1u);
  EXPECT_FLOAT_EQ(state.find(Target::kFace, "cost")->values[0], 1.5F);
  EXPECT_FLOAT_EQ(state.find(Target::kFace, "cost")->values[1], 9.0F);

  // Dense: replaces the channel; a dense patch of the wrong size is skipped.
  result = applyMeshMapUpdate(state, makeUpdate(bytes, Target::kFace, "cost", {5.0F, 6.0F}));
  EXPECT_FLOAT_EQ(state.find(Target::kFace, "cost")->values[0], 5.0F);
  result = applyMeshMapUpdate(state, makeUpdate(bytes, Target::kFace, "cost", {7.0F}));
  EXPECT_EQ(result.skipped_patches, 1u);
  EXPECT_FLOAT_EQ(state.find(Target::kFace, "cost")->values[0], 5.0F);

  // A new channel: "no data" where the patch is silent.
  result = applyMeshMapUpdate(state, makeUpdate(bytes, Target::kFace, "occupancy", {0.25F}, {0}));
  ASSERT_NE(state.find(Target::kFace, "occupancy"), nullptr);
  EXPECT_TRUE(std::isnan(state.find(Target::kFace, "occupancy")->values[1]));

  // A vertex-colour update shows in the vertex-colour source.
  result = applyMeshMapUpdate(state, makeUpdate(bytes, Target::kVertex, "red", {99.0F}, {3}));
  MeshMapUpload upload;
  fillMeshMapGeometry(state, upload);
  fillMeshMapColors(state, kMeshMapSourceVertexColor, upload);
  EXPECT_EQ(upload.colors[5] & 0xFFU, 99U);

  // Another geometry: refused whole; an unset id applies to any.
  MeshMapUpdate other = makeUpdate(bytes, Target::kFace, "cost", {100.0F, 100.0F});
  other.geometry_id = "8";
  EXPECT_FALSE(applyMeshMapUpdate(state, other).applied);
  EXPECT_FLOAT_EQ(state.find(Target::kFace, "cost")->values[0], 5.0F);
  other.geometry_id.clear();
  EXPECT_TRUE(applyMeshMapUpdate(state, other).applied);
  EXPECT_FLOAT_EQ(state.find(Target::kFace, "cost")->values[0], 100.0F);
}

TEST(MeshMapViewTest, LinksBecomeCentroidSegmentsColouredByKind) {
  Fixture fixture;
  MeshMapUpload upload;
  fillMeshMapGeometry(decodeMeshMap(fixture.mesh), upload);
  ASSERT_EQ(upload.link_positions.size(), 6u);
  EXPECT_FLOAT_EQ(upload.link_positions[0], 2.0F / 3.0F);  // centroid of face 0, x
  EXPECT_FLOAT_EQ(upload.link_positions[2], 2.0F / 3.0F);  // z: (0 + 0 + 2) / 3
  EXPECT_FLOAT_EQ(upload.link_positions[3], 1.0F / 3.0F);  // centroid of face 1, x
  ASSERT_EQ(upload.link_colors.size(), 2u);
  EXPECT_EQ(upload.link_colors[0], categoricalColor(2));
}

TEST(MeshMapViewTest, PaletteIsDistinctAndHandlesNegatives) {
  EXPECT_NE(categoricalColor(0), categoricalColor(1));
  EXPECT_EQ(categoricalColor(0), categoricalColor(10));
  EXPECT_EQ(categoricalColor(-1) >> 24U, 255U);
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
