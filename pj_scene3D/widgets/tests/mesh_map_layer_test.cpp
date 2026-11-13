// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Behavioural coverage for MeshMapLayer on the canonical-codec route: attach and
// the `<topic>_updates` pairing, the auto colour pick, staging only on a change,
// updates applied incrementally going forward and from the map going back, a late
// update with an older stamp, an update for another geometry, a missing
// requested channel, the vertex-colour and solid modes, XML round-trip, and the
// size refusal.

#include "pj_scene3d_widgets/layers/mesh_map_layer.h"

#include <gtest/gtest.h>

#include <QDomDocument>
#include <QString>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/mesh_map.hpp"
#include "pj_base/builtin/mesh_map_codec.hpp"
#include "pj_base/builtin/mesh_map_update.hpp"
#include "pj_base/builtin/mesh_map_update_codec.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;
using pj::scene3d::kMeshMapColorSolid;
using pj::scene3d::kMeshMapColorVertex;
using pj::scene3d::MeshMapColorMode;
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

// A unit square of two faces. Vertex record: x, y, z float32 + red, green, blue,
// alpha uint8. Face record: vertex_indices + cost float32. The bytes live in the
// returned vector, which the caller keeps alive until serialization.
MeshMap makeMesh(std::vector<uint8_t>& bytes, PJ::Timestamp ts, const std::string& geometry_id = "1") {
  bytes.clear();
  const float xyz[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
  for (int v = 0; v < 4; ++v) {
    for (const float c : xyz[v]) {
      append(bytes, c);
    }
    append(bytes, static_cast<uint8_t>(50 * v));
    append(bytes, uint8_t{0});
    append(bytes, uint8_t{0});
    append(bytes, uint8_t{255});
  }
  const uint32_t faces[2][3] = {{0, 1, 2}, {0, 2, 3}};
  const float cost[2] = {1.0F, 3.0F};
  for (int f = 0; f < 2; ++f) {
    for (const uint32_t i : faces[f]) {
      append(bytes, i);
    }
    append(bytes, cost[f]);
  }
  MeshMap mesh;
  mesh.timestamp_ns = ts;
  mesh.frame_id = "map";
  mesh.geometry_id = geometry_id;
  mesh.vertex_count = 4;
  mesh.vertex_stride = 16;
  mesh.vertex_fields = {
      {.name = "x", .offset = 0, .datatype = Datatype::kFloat32, .count = 1},
      {.name = "y", .offset = 4, .datatype = Datatype::kFloat32, .count = 1},
      {.name = "z", .offset = 8, .datatype = Datatype::kFloat32, .count = 1},
      {.name = "red", .offset = 12, .datatype = Datatype::kUint8, .count = 1},
      {.name = "green", .offset = 13, .datatype = Datatype::kUint8, .count = 1},
      {.name = "blue", .offset = 14, .datatype = Datatype::kUint8, .count = 1},
      {.name = "alpha", .offset = 15, .datatype = Datatype::kUint8, .count = 1},
  };
  mesh.face_count = 2;
  mesh.face_stride = 16;
  mesh.face_data_offset = 64;
  mesh.face_fields = {
      {.name = "vertex_indices", .offset = 0, .datatype = Datatype::kUint32, .count = 3},
      {.name = "cost", .offset = 12, .datatype = Datatype::kFloat32, .count = 1},
  };
  mesh.data = PJ::Span<const uint8_t>(bytes.data(), bytes.size());
  return mesh;
}

std::vector<uint8_t> serializedUpdate(
    PJ::Timestamp ts, uint32_t face, float cost, const std::string& geometry_id = "1") {
  std::vector<uint8_t> bytes;
  append(bytes, face);
  append(bytes, cost);
  MeshMapUpdate update;
  update.timestamp_ns = ts;
  update.frame_id = "map";
  update.geometry_id = geometry_id;
  update.target = MeshMapUpdate::Target::kFaces;
  update.count = 1;
  update.stride = 8;
  update.fields = {
      {.name = "index", .offset = 0, .datatype = Datatype::kUint32, .count = 1},
      {.name = "cost", .offset = 4, .datatype = Datatype::kFloat32, .count = 1},
  };
  update.data = PJ::Span<const uint8_t>(bytes.data(), bytes.size());
  return PJ::serializeMeshMapUpdate(update);
}

struct Rig {
  PJ::SessionManager session;
  PJ::ObjectTopicId topic_id = registerObjectTopic(session, "/navmap");
  PJ::ObjectTopicId updates_id = registerObjectTopic(session, "/navmap_updates");
  pj::scene3d::Scene3DLayerContext ctx;
  pj::scene3d::MeshMapLayer layer{topic_id, u"navmap"_s};

  Rig() {
    ctx.session = &session;
  }
  void pushMap(PJ::Timestamp ts, const std::string& geometry_id = "1") {
    std::vector<uint8_t> bytes;
    const MeshMap mesh = makeMesh(bytes, ts, geometry_id);
    ASSERT_TRUE(session.objectStore().pushOwned(topic_id, ts, PJ::serializeMeshMap(mesh)).has_value());
  }
  void pushUpdate(PJ::Timestamp ts, uint32_t face, float cost, const std::string& geometry_id = "1") {
    ASSERT_TRUE(
        session.objectStore().pushOwned(updates_id, ts, serializedUpdate(ts, face, cost, geometry_id)).has_value());
  }
  // The staged cost of `face` (values are three per face).
  [[nodiscard]] float stagedCost(uint32_t face) const {
    const auto* mesh = layer.stagedMeshForTest();
    return mesh != nullptr && mesh->values.size() > 3 * face ? mesh->values[3 * face] : std::nanf("");
  }
};

TEST(MeshMapLayer, AttachesPairsUpdatesAndAutoPicksTheChannel) {
  Rig rig;
  rig.pushMap(100);
  ASSERT_TRUE(rig.layer.attach(rig.ctx)) << "a parser-less canonical PJ.MeshMap topic must attach";
  EXPECT_TRUE(rig.layer.hasUpdatesTopicForTest());
  EXPECT_EQ(rig.layer.sourceFrame(), u"map"_s);
  EXPECT_EQ(rig.layer.resolvedColorByForTest(), u"cost"_s);

  rig.layer.renderAtForTest(100);
  const auto* mesh = rig.layer.stagedMeshForTest();
  ASSERT_NE(mesh, nullptr);
  EXPECT_EQ(mesh->positions.size(), 18u);
  EXPECT_EQ(mesh->color_mode, MeshMapColorMode::kChannel);
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 3.0F);
  EXPECT_TRUE(mesh->color_range.valid);
  EXPECT_TRUE(rig.layer.worldBounds().has_value());

  const auto revision = rig.layer.stagedRevision();
  rig.layer.renderAtForTest(100);
  rig.layer.renderAtForTest(150);
  EXPECT_EQ(rig.layer.stagedRevision(), revision) << "nothing changed: no re-stage";
}

TEST(MeshMapLayer, AppliesUpdatesForwardAndRebuildsGoingBack) {
  Rig rig;
  rig.pushMap(100);
  rig.pushUpdate(200, 0, 7.0F);
  rig.pushUpdate(300, 1, 9.0F);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));

  rig.layer.renderAtForTest(250);
  EXPECT_FLOAT_EQ(rig.stagedCost(0), 7.0F);
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 3.0F);
  const int rebuilds = rig.layer.rebuildCountForTest();

  rig.layer.renderAtForTest(350);  // forward: only the new update
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 9.0F);
  EXPECT_EQ(rig.layer.rebuildCountForTest(), rebuilds);
  EXPECT_EQ(rig.layer.appliedUpdateCountForTest(), 2);

  rig.layer.renderAtForTest(150);  // back before both updates: rebuilt from the map
  EXPECT_FLOAT_EQ(rig.stagedCost(0), 1.0F);
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 3.0F);
  EXPECT_EQ(rig.layer.rebuildCountForTest(), rebuilds + 1);
}

TEST(MeshMapLayer, ALateUpdateWithAnOlderStampRebuilds) {
  Rig rig;
  rig.pushMap(100);
  rig.pushUpdate(300, 1, 9.0F);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(400);
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 9.0F);

  rig.pushUpdate(200, 0, 5.0F);  // arrives after, stamped before what was applied
  rig.layer.renderAtForTest(400);
  EXPECT_FLOAT_EQ(rig.stagedCost(0), 5.0F);
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 9.0F);
}

TEST(MeshMapLayer, UpdatesForAnotherGeometryAreSkippedWithAWarning) {
  Rig rig;
  rig.pushMap(100, "1");
  rig.pushUpdate(200, 0, 7.0F, "2");
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(250);
  EXPECT_FLOAT_EQ(rig.stagedCost(0), 1.0F);
  EXPECT_FALSE(rig.layer.statusWarning().isEmpty());
}

TEST(MeshMapLayer, ColorSourcesAndAMissingRequest) {
  Rig rig;
  rig.pushMap(100);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));

  rig.layer.setColorBy(QString::fromUtf8(kMeshMapColorVertex.data(), kMeshMapColorVertex.size()));
  rig.layer.renderAtForTest(100);
  ASSERT_NE(rig.layer.stagedMeshForTest(), nullptr);
  EXPECT_EQ(rig.layer.stagedMeshForTest()->color_mode, MeshMapColorMode::kVertexColor);

  rig.layer.setColorBy(QString::fromUtf8(kMeshMapColorSolid.data(), kMeshMapColorSolid.size()));
  rig.layer.renderAtForTest(100);
  EXPECT_EQ(rig.layer.stagedMeshForTest()->color_mode, MeshMapColorMode::kSolid);

  rig.layer.setColorBy(u"elevation"_s);  // not in this map: falls back, warns, keeps the request
  rig.layer.renderAtForTest(100);
  EXPECT_EQ(rig.layer.resolvedColorByForTest(), u"cost"_s);
  EXPECT_FALSE(rig.layer.statusWarning().isEmpty());
}

TEST(MeshMapLayer, XmlRoundTrip) {
  Rig rig;
  rig.layer.setColorBy(u"cost"_s);
  rig.layer.setColormap(PJ::Colormap::kViridis);
  rig.layer.setAutoRange(false);
  rig.layer.setManualRange(-1.0, 2.5);
  rig.layer.setLighting(false);
  rig.layer.setOpacity(0.5);
  rig.layer.setShowLinks(false);
  QDomDocument doc;
  const QDomElement saved = rig.layer.xmlSaveState(doc);

  pj::scene3d::MeshMapLayer restored{rig.topic_id, u"copy"_s};
  ASSERT_TRUE(restored.xmlLoadState(saved));
  EXPECT_EQ(restored.colormap(), PJ::Colormap::kViridis);
  EXPECT_FALSE(restored.autoRange());
  EXPECT_DOUBLE_EQ(restored.manualRange().first, -1.0);
  EXPECT_DOUBLE_EQ(restored.manualRange().second, 2.5);
  EXPECT_FALSE(restored.lighting());
  EXPECT_DOUBLE_EQ(restored.opacity(), 0.5);
  EXPECT_FALSE(restored.showLinks());

  QDomElement wrong = doc.createElement(u"grid_map"_s);
  EXPECT_FALSE(restored.xmlLoadState(wrong));
}

TEST(MeshMapLayer, BeforeTheFirstMapNothingIsStaged) {
  Rig rig;
  rig.pushMap(100);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(50);
  EXPECT_EQ(rig.layer.stagedMeshForTest(), nullptr);
}

}  // namespace
