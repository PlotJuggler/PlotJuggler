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

#include <algorithm>
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
  EXPECT_DOUBLE_EQ(height->values[1], 20.0);
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
    EXPECT_EQ(value, kMeshMapNoDataValue);
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
  EXPECT_EQ(result.skipped_indices, 1u);
  EXPECT_DOUBLE_EQ(state.find(Target::kFace, "cost")->values[0], 1.5);
  EXPECT_DOUBLE_EQ(state.find(Target::kFace, "cost")->values[1], 9.0);

  // Dense: replaces the channel; a dense patch of the wrong size is skipped.
  result = applyMeshMapUpdate(state, makeUpdate(bytes, Target::kFace, "cost", {5.0F, 6.0F}));
  EXPECT_DOUBLE_EQ(state.find(Target::kFace, "cost")->values[0], 5.0);
  result = applyMeshMapUpdate(state, makeUpdate(bytes, Target::kFace, "cost", {7.0F}));
  EXPECT_EQ(result.skipped_patches, 1u);
  EXPECT_DOUBLE_EQ(state.find(Target::kFace, "cost")->values[0], 5.0);

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
  EXPECT_DOUBLE_EQ(state.find(Target::kFace, "cost")->values[0], 5.0);
  other.geometry_id.clear();
  EXPECT_TRUE(applyMeshMapUpdate(state, other).applied);
  EXPECT_DOUBLE_EQ(state.find(Target::kFace, "cost")->values[0], 100.0);
}

TEST(MeshMapViewTest, PatchesMarkWhatTheyWrote) {
  Fixture fixture;
  MeshMapState state = decodeMeshMap(fixture.mesh);
  Bytes bytes;
  const auto dirty = [&state](Target target, const char* name) { return state.find(target, name)->dirty; };
  EXPECT_TRUE(dirty(Target::kFace, "cost").empty());

  // Sparse: the span of the indices written; one past the end is not.
  (void)applyMeshMapUpdate(state, makeUpdate(bytes, Target::kFace, "cost", {9.0F, 1.0F}, {1, 5}));
  EXPECT_EQ(dirty(Target::kFace, "cost"), (ElementRange{.begin = 1, .end = 2}));
  EXPECT_TRUE(dirty(Target::kFace, "surface").empty());
  clearMeshMapDirty(state);
  EXPECT_TRUE(dirty(Target::kFace, "cost").empty());

  // Dense, a new channel, and new semantics: every element.
  (void)applyMeshMapUpdate(state, makeUpdate(bytes, Target::kFace, "cost", {5.0F, 6.0F}));
  EXPECT_EQ(dirty(Target::kFace, "cost"), (ElementRange{.begin = 0, .end = 2}));
  (void)applyMeshMapUpdate(state, makeUpdate(bytes, Target::kVertex, "slope", {0.5F}, {3}));
  EXPECT_EQ(dirty(Target::kVertex, "slope"), (ElementRange{.begin = 0, .end = 4}));
  clearMeshMapDirty(state);
  MeshMapUpdate relabel = makeUpdate(bytes, Target::kFace, "cost", {1.0F}, {0});
  relabel.patches[0].channel.info.unit = "s";
  (void)applyMeshMapUpdate(state, relabel);
  EXPECT_EQ(dirty(Target::kFace, "cost"), (ElementRange{.begin = 0, .end = 2}));
}

// A refresh must leave the upload as a whole fill from the same state would.
TEST(MeshMapViewTest, RefreshRecoloursOnlyTheFacesAFaceChannelPatchWrote) {
  Fixture fixture;
  MeshMapState state = decodeMeshMap(fixture.mesh);
  Bytes bytes;
  for (const char* source : {"face:cost", "face:surface"}) {
    SCOPED_TRACE(source);
    clearMeshMapDirty(state);
    MeshMapUpload upload;
    fillMeshMapGeometry(state, upload);
    fillMeshMapColors(state, source, upload);
    EXPECT_EQ(refreshMeshMapColors(state, source, upload), ElementRange{}) << "nothing written: nothing to do";

    MeshMapUpdate update = makeUpdate(bytes, Target::kFace, "cost", {1.0F}, {1});
    update.patches.push_back(makeUpdate(bytes, Target::kFace, "surface", {0.0F}, {1}).patches[0]);
    update.patches[1].channel.info = state.find(Target::kFace, "surface")->info;  // still categorical
    (void)applyMeshMapUpdate(state, update);

    const auto corners = refreshMeshMapColors(state, source, upload);
    ASSERT_TRUE(corners.has_value());
    EXPECT_EQ(*corners, (ElementRange{.begin = 3, .end = 6}));
    MeshMapUpload whole;
    fillMeshMapGeometry(state, whole);
    fillMeshMapColors(state, source, whole);
    EXPECT_EQ(upload.values, whole.values);
    EXPECT_EQ(upload.colors, whole.colors);
    EXPECT_EQ(upload.color_range.valid, whole.color_range.valid);
    EXPECT_FLOAT_EQ(upload.color_range.lo, whole.color_range.lo);
    EXPECT_FLOAT_EQ(upload.color_range.hi, whole.color_range.hi) << "the range shrinks with the values";
  }
}

TEST(MeshMapViewTest, RefreshAsksForAWholeFillWhenASpanIsNotEnough) {
  Fixture fixture;
  MeshMapState state = decodeMeshMap(fixture.mesh);
  Bytes bytes;
  MeshMapUpload upload;
  fillMeshMapGeometry(state, upload);

  // A vertex touches faces anywhere in the buffer.
  fillMeshMapColors(state, "vertex:height", upload);
  (void)applyMeshMapUpdate(state, makeUpdate(bytes, Target::kVertex, "height", {5.0F}, {0}));
  EXPECT_FALSE(refreshMeshMapColors(state, "vertex:height", upload).has_value());
  // Another channel's patch leaves this source alone.
  fillMeshMapColors(state, "face:cost", upload);
  EXPECT_EQ(refreshMeshMapColors(state, "face:cost", upload), ElementRange{});

  // The vertex colour, when a component was written.
  clearMeshMapDirty(state);
  fillMeshMapColors(state, kMeshMapSourceVertexColor, upload);
  EXPECT_EQ(refreshMeshMapColors(state, kMeshMapSourceVertexColor, upload), ElementRange{});
  (void)applyMeshMapUpdate(state, makeUpdate(bytes, Target::kVertex, "red", {99.0F}, {3}));
  EXPECT_FALSE(refreshMeshMapColors(state, kMeshMapSourceVertexColor, upload).has_value());

  // A patch that turns a categorical channel continuous changes the colour mode.
  clearMeshMapDirty(state);
  fillMeshMapColors(state, "face:surface", upload);
  ASSERT_EQ(upload.color_mode, MeshMapColorMode::kRgba);
  (void)applyMeshMapUpdate(state, makeUpdate(bytes, Target::kFace, "surface", {1.0F}, {0}));
  EXPECT_FALSE(refreshMeshMapColors(state, "face:surface", upload).has_value());

  // Solid has nothing to refresh; a source that appeared needs the whole fill.
  clearMeshMapDirty(state);
  fillMeshMapColors(state, kMeshMapSourceSolid, upload);
  EXPECT_EQ(refreshMeshMapColors(state, kMeshMapSourceSolid, upload), ElementRange{});
  EXPECT_FALSE(refreshMeshMapColors(state, "face:cost", upload).has_value());
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

// "No data" is compared at the channel's own precision: the sentinels at the top
// of uint32, int32 and a float32 1e30 all fold into NaN, and their neighbours
// stay data.
TEST(MeshMapViewTest, NoDataIsComparedBeforeNarrowing) {
  Fixture fixture;
  Bytes& bytes = fixture.bytes;
  PJ::sdk::ChannelInfo u32;
  u32.no_data = 4294967295.0;
  PJ::sdk::ChannelInfo i32;
  i32.no_data = 2147483647.0;
  PJ::sdk::ChannelInfo f32;
  f32.no_data = 1e30;
  fixture.mesh.channels = {
      makeChannel(
          bytes, "u32", Target::kFace, Datatype::kUint32, bytes.add(std::vector<uint32_t>{4294967295U, 4294967294U}),
          u32),
      makeChannel(
          bytes, "i32", Target::kFace, Datatype::kInt32, bytes.add(std::vector<int32_t>{2147483647, 2147483646}), i32),
      makeChannel(bytes, "f32", Target::kFace, Datatype::kFloat32, bytes.add(std::vector<float>{1e30F, 2.0F}), f32),
  };
  const MeshMapState state = decodeMeshMap(fixture.mesh);
  for (const char* name : {"u32", "i32", "f32"}) {
    const MeshMapChannelState* channel = state.find(Target::kFace, name);
    ASSERT_NE(channel, nullptr) << name;
    EXPECT_TRUE(std::isnan(channel->values[0])) << name << " sentinel drawn as data";
    EXPECT_FALSE(std::isnan(channel->values[1])) << name;
  }
  EXPECT_EQ(state.find(Target::kFace, "u32")->values[1], 4294967294.0);
  EXPECT_EQ(state.find(Target::kFace, "i32")->values[1], 2147483646.0);
}

// Category ids above 2^24 keep their identity.
TEST(MeshMapViewTest, LargeCategoryIdsStayDistinct) {
  Fixture fixture;
  Bytes& bytes = fixture.bytes;
  PJ::sdk::ChannelInfo info;
  info.kind = ChannelKind::kCategorical;
  PJ::sdk::ChannelCategory a;
  a.value = 16777216;
  a.color = PJ::sdk::ColorRGBA{10, 0, 0, 255};
  PJ::sdk::ChannelCategory b;
  b.value = 16777217;
  b.color = PJ::sdk::ColorRGBA{20, 0, 0, 255};
  info.categories = {a, b};
  fixture.mesh.channels = {makeChannel(
      bytes, "region", Target::kFace, Datatype::kUint32, bytes.add(std::vector<uint32_t>{16777216U, 16777217U}), info)};
  const MeshMapState state = decodeMeshMap(fixture.mesh);
  MeshMapUpload upload;
  fillMeshMapGeometry(state, upload);
  fillMeshMapColors(state, "face:region", upload);
  ASSERT_EQ(upload.colors.size(), 6u);
  EXPECT_EQ(upload.colors[0] & 0xFFU, 10U);
  EXPECT_EQ(upload.colors[3] & 0xFFU, 20U);
}

// float64 positions far from the origin keep their spacing: the decode removes a
// double-precision offset and moves it into the origin pose.
TEST(MeshMapViewTest, Float64PositionsKeepPrecisionFarFromTheOrigin) {
  Fixture fixture;
  fixture.mesh.position_type = Datatype::kFloat64;
  fixture.mesh.positions =
      fixture.bytes.add(std::vector<double>{4500000.10, 0, 0, 4500000.35, 0, 0, 4500000.35, 1, 0, 4500000.10, 1, 0});
  fixture.mesh.origin.position = {.x = 0.0, .y = 0.0, .z = 5.0};
  ASSERT_TRUE(PJ::validateMeshMap(fixture.mesh));
  const MeshMapState state = decodeMeshMap(fixture.mesh);
  EXPECT_NEAR(state.positions[3] - state.positions[0], 0.25F, 1e-3F);
  EXPECT_NEAR(state.origin.position.x + static_cast<double>(state.positions[0]), 4500000.10, 1e-6);
  EXPECT_NEAR(state.origin.position.z + static_cast<double>(state.positions[2]), 5.0, 1e-6);
  const AABB box = meshMapBounds(state);
  ASSERT_TRUE(box.valid);
  EXPECT_NEAR(box.min.x, 4500000.10, 0.5);
  EXPECT_NEAR(box.max.x, 4500000.35, 0.5);
  EXPECT_NEAR(box.min.z, 5.0, 1e-3);
}

// A categorical vertex channel follows the whole-face no-data rule too.
TEST(MeshMapViewTest, CategoricalVertexChannelDrawsAFaceTouchingNoDataAsNoData) {
  Fixture fixture;
  Bytes& bytes = fixture.bytes;
  PJ::sdk::ChannelInfo info;
  info.kind = ChannelKind::kCategorical;
  info.no_data = -1.0;
  fixture.mesh.channels = {makeChannel(
      bytes, "label", Target::kVertex, Datatype::kInt16, bytes.add(std::vector<int16_t>{1, 1, 1, -1}), info)};
  const MeshMapState state = decodeMeshMap(fixture.mesh);
  MeshMapUpload upload;
  fillMeshMapGeometry(state, upload);
  fillMeshMapColors(state, "vertex:label", upload);
  ASSERT_EQ(upload.colors.size(), 6u);
  EXPECT_EQ(upload.colors[0], categoricalColor(1));  // face 0 has no no-data corner
  for (size_t corner = 3; corner < 6; ++corner) {
    EXPECT_EQ(upload.colors[corner], upload.colors[5]) << "face 1 blends towards no data at corner " << corner;
  }
  EXPECT_NE(upload.colors[3], categoricalColor(1));
}

// Only uint8 vertex channels named red/green/blue/alpha are the vertex colour;
// float ones are data channels.
TEST(MeshMapViewTest, OnlyUint8ColorChannelsAreTheVertexColor) {
  Fixture fixture;
  Bytes& bytes = fixture.bytes;
  fixture.mesh.channels = {
      makeChannel(bytes, "red", Target::kVertex, Datatype::kFloat32, bytes.add(std::vector<float>{1, 0.5F, 0, 0})),
      makeChannel(bytes, "green", Target::kVertex, Datatype::kFloat32, bytes.add(std::vector<float>{0, 0, 0, 0})),
      makeChannel(bytes, "blue", Target::kVertex, Datatype::kFloat32, bytes.add(std::vector<float>{0, 0, 0, 0})),
  };
  const MeshMapState state = decodeMeshMap(fixture.mesh);
  const auto sources = meshMapColorSources(state);
  EXPECT_EQ(std::count(sources.begin(), sources.end(), std::string(kMeshMapSourceVertexColor)), 0);
  EXPECT_EQ(defaultMeshMapColorSource(state), "vertex:red");

  // The uint8 fixture is the vertex colour.
  EXPECT_EQ(defaultMeshMapColorSource(decodeMeshMap(Fixture{}.mesh)), "face:cost");
  const auto uint8_sources = meshMapColorSources(decodeMeshMap(Fixture{}.mesh));
  EXPECT_EQ(std::count(uint8_sources.begin(), uint8_sources.end(), std::string(kMeshMapSourceVertexColor)), 1);
}

// Colours with alpha below 255 mark the upload translucent.
TEST(MeshMapViewTest, ColoursReportTranslucency) {
  Fixture fixture;
  const MeshMapState state = decodeMeshMap(fixture.mesh);
  MeshMapUpload upload;
  fillMeshMapGeometry(state, upload);
  for (const std::string_view source :
       {std::string_view("face:cost"), std::string_view("face:surface"), kMeshMapSourceVertexColor,
        kMeshMapSourceSolid}) {
    fillMeshMapColors(state, source, upload);
    EXPECT_FALSE(upload.translucent) << source;
  }
  Fixture with_alpha;
  with_alpha.mesh.channels.push_back(makeChannel(
      with_alpha.bytes, "alpha", Target::kVertex, Datatype::kUint8,
      with_alpha.bytes.add(std::vector<uint8_t>{255, 255, 128, 255})));
  fillMeshMapColors(decodeMeshMap(with_alpha.mesh), kMeshMapSourceVertexColor, upload);
  EXPECT_TRUE(upload.translucent);

  Fixture categorical;
  categorical.mesh.channels[0].info.categories[0].color = PJ::sdk::ColorRGBA{200, 10, 20, 100};
  fillMeshMapColors(decodeMeshMap(categorical.mesh), "face:surface", upload);
  EXPECT_TRUE(upload.translucent);
}

}  // namespace
