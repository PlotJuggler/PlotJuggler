// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Behavioural coverage for MeshMapLayer on the canonical-codec route: attach and
// update-topic pairing (the `<topic>_updates` sibling, and any MeshMapUpdate
// topic carrying the map's geometry_id), the auto colour pick, staging only on a
// change, updates applied incrementally going forward and from the map going
// back, a late update with an older stamp, an update for another geometry, colour
// sources and a missing request, XML round-trip.

#include "pj_scene3d_widgets/layers/mesh_map_layer.h"

#include <gtest/gtest.h>

#include <QDomDocument>
#include <QString>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/mesh_map.hpp"
#include "pj_base/builtin/mesh_map_codec.hpp"
#include "pj_base/builtin/mesh_map_update.hpp"
#include "pj_base/builtin/mesh_map_update_codec.hpp"
#include "pj_base/time.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;
using pj::scene3d::kMeshMapSourceSolid;
using pj::scene3d::kMeshMapSourceVertexColor;
using pj::scene3d::MeshMapColorMode;
using PJ::sdk::MeshChannel;
using PJ::sdk::MeshMap;
using PJ::sdk::MeshMapUpdate;
using Datatype = PJ::sdk::PointField::Datatype;
using Target = PJ::sdk::MeshChannel::Target;

template <typename T>
PJ::Span<const uint8_t> keep(std::vector<std::vector<uint8_t>>& storage, const std::vector<T>& values) {
  storage.emplace_back(values.size() * sizeof(T));
  if (!values.empty()) {
    std::memcpy(storage.back().data(), values.data(), storage.back().size());
  }
  return {storage.back().data(), storage.back().size()};
}

MeshChannel channel(std::string name, Target target, Datatype datatype, PJ::Span<const uint8_t> data) {
  MeshChannel out;
  out.name = std::move(name);
  out.target = target;
  out.datatype = datatype;
  out.data = data;
  return out;
}

// A unit square of two faces, a float `cost` face channel and an RGB vertex
// colour; `tweak` edits the map before it is serialized.
std::vector<uint8_t> serializedMap(
    PJ::Timestamp ts, const std::string& geometry_id, const std::function<void(MeshMap&)>& tweak = {}) {
  std::vector<std::vector<uint8_t>> storage;
  storage.reserve(8);
  MeshMap mesh;
  mesh.timestamp_ns = ts;
  mesh.frame_id = "map";
  mesh.geometry_id = geometry_id;
  mesh.vertex_count = 4;
  mesh.positions = keep(storage, std::vector<float>{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0});
  mesh.face_count = 2;
  mesh.indices = keep(storage, std::vector<uint32_t>{0, 1, 2, 0, 2, 3});
  mesh.channels = {
      channel("cost", Target::kFace, Datatype::kFloat32, keep(storage, std::vector<float>{1.0F, 3.0F})),
      channel("red", Target::kVertex, Datatype::kUint8, keep(storage, std::vector<uint8_t>{0, 50, 100, 150})),
      channel("green", Target::kVertex, Datatype::kUint8, keep(storage, std::vector<uint8_t>{0, 0, 0, 0})),
      channel("blue", Target::kVertex, Datatype::kUint8, keep(storage, std::vector<uint8_t>{0, 0, 0, 0})),
  };
  if (tweak) {
    tweak(mesh);
  }
  return PJ::serializeMeshMap(mesh);
}

std::vector<uint8_t> serializedUpdate(PJ::Timestamp ts, uint32_t face, float cost, const std::string& geometry_id) {
  std::vector<std::vector<uint8_t>> storage;
  storage.reserve(2);
  PJ::sdk::MeshChannelPatch patch;
  patch.channel = channel("cost", Target::kFace, Datatype::kFloat32, keep(storage, std::vector<float>{cost}));
  patch.indices = keep(storage, std::vector<uint32_t>{face});
  MeshMapUpdate update;
  update.timestamp_ns = ts;
  update.geometry_id = geometry_id;
  update.patches.push_back(patch);
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
  void pushMap(
      PJ::Timestamp ts, const std::string& geometry_id = "1", const std::function<void(MeshMap&)>& tweak = {}) {
    ASSERT_TRUE(session.objectStore().pushOwned(topic_id, ts, serializedMap(ts, geometry_id, tweak)).has_value());
  }
  void pushUpdate(
      PJ::Timestamp ts, uint32_t face, float cost, const std::string& geometry_id = "1",
      std::optional<PJ::ObjectTopicId> topic = std::nullopt) {
    ASSERT_TRUE(session.objectStore()
                    .pushOwned(topic.value_or(updates_id), ts, serializedUpdate(ts, face, cost, geometry_id))
                    .has_value());
  }
  // A MeshMapUpdate topic under an unrelated name, as a producer advertises it.
  PJ::ObjectTopicId registerUpdateTopic(const std::string& name) {
    const auto topic = session.objectStore().registerTopic(
        PJ::ObjectTopicDescriptor{
            .dataset_id = 1, .topic_name = name, .metadata_json = R"({"builtin_object_type":"kMeshMapUpdate"})"});
    EXPECT_TRUE(topic.has_value());
    return topic.has_value() ? topic.value() : PJ::ObjectTopicId{};
  }
  // The staged value of `face` (values are three per face).
  [[nodiscard]] float stagedCost(uint32_t face) const {
    const auto* mesh = layer.stagedMeshForTest();
    return mesh != nullptr && mesh->values.size() > 3 * face ? mesh->values[3 * face] : std::nanf("");
  }
};

TEST(MeshMapLayer, AttachesPairsUpdatesAndAutoPicksTheChannel) {
  Rig rig;
  rig.pushMap(100);
  ASSERT_TRUE(rig.layer.attach(rig.ctx)) << "a parser-less canonical PJ.MeshMap topic must attach";
  EXPECT_EQ(rig.layer.updateTopicCountForTest(), 1u);
  EXPECT_EQ(rig.layer.sourceFrame(), u"map"_s);
  EXPECT_EQ(rig.layer.resolvedColorByForTest(), u"face:cost"_s);

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
  const int rebuilds = rig.layer.rebaseCountForTest();

  rig.layer.renderAtForTest(350);  // forward: only the new update
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 9.0F);
  EXPECT_EQ(rig.layer.rebaseCountForTest(), rebuilds);
  EXPECT_EQ(rig.layer.appliedUpdateCountForTest(), 2);

  rig.layer.renderAtForTest(150);  // back before both updates: rebuilt from the map
  EXPECT_FLOAT_EQ(rig.stagedCost(0), 1.0F);
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 3.0F);
  EXPECT_EQ(rig.layer.rebaseCountForTest(), rebuilds + 1);
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

  // The newest arrival (update@200) is the highest UID at 250 and at 350 alike:
  // only the set of updates at or before each time tells them apart.
  rig.layer.renderAtForTest(250);
  EXPECT_FLOAT_EQ(rig.stagedCost(0), 5.0F);
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 3.0F) << "update@300 still shown at 250";
  rig.layer.renderAtForTest(350);
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 9.0F) << "update@300 never applied at 350";
  rig.layer.renderAtForTest(250);
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 3.0F) << "scrubbing back kept update@300";
}

// Two maps of one geometry and a geometry-paired update after each: the pairing
// outlives the rebuild from the second map, so the scrub range still reaches the
// last update.
TEST(MeshMapLayer, APairedTopicKeepsItsTimeRangeAcrossMaps) {
  Rig rig;
  const PJ::ObjectTopicId costs = rig.registerUpdateTopic("/mesh/costs");
  rig.pushMap(100);
  rig.pushMap(200);
  rig.pushUpdate(150, 0, 7.0F, "1", costs);
  rig.pushUpdate(250, 1, 8.0F, "1", costs);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(150);
  EXPECT_FLOAT_EQ(rig.stagedCost(0), 7.0F);
  rig.layer.renderAtForTest(200);
  EXPECT_EQ(PJ::toRaw(rig.layer.timeRange().max), 250) << "the tracker would be clamped at the last map";
  rig.layer.renderAtForTest(250);
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 8.0F);
}

// A dataset that drops one update topic and gains another keeps its topic count:
// the new one must still be paired.
TEST(MeshMapLayer, AReplacedUpdateTopicIsPaired) {
  Rig rig;
  const PJ::ObjectTopicId old_costs = rig.registerUpdateTopic("/mesh/old_costs");
  rig.pushMap(100);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(100);
  rig.session.objectStore().removeTopic(old_costs);
  const PJ::ObjectTopicId new_costs = rig.registerUpdateTopic("/mesh/new_costs");
  ASSERT_NE(new_costs, old_costs);
  rig.pushUpdate(200, 0, 7.0F, "1", new_costs);
  rig.layer.renderAtForTest(250);
  EXPECT_FLOAT_EQ(rig.stagedCost(0), 7.0F);
}

// Rewinding on the same map rebuilds the channels but not the geometry.
TEST(MeshMapLayer, ARewindOnTheSameMapKeepsTheGeometry) {
  Rig rig;
  rig.pushMap(100);
  rig.pushUpdate(200, 0, 7.0F);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(250);
  ASSERT_NE(rig.layer.stagedMeshForTest(), nullptr);
  const auto geometry = rig.layer.stagedMeshForTest()->geometry_revision;
  const int rebuilds = rig.layer.rebaseCountForTest();
  rig.layer.renderAtForTest(150);
  EXPECT_EQ(rig.layer.rebaseCountForTest(), rebuilds + 1);
  EXPECT_FLOAT_EQ(rig.stagedCost(0), 1.0F);
  EXPECT_EQ(rig.layer.stagedMeshForTest()->geometry_revision, geometry) << "the positions were re-uploaded";
}

// An update that rewrote faces of the coloured channel stages that span alone;
// anything that needs every colour again (a rewind here) stages whole.
TEST(MeshMapLayer, ASparseUpdateStagesOnlyTheFacesItWrote) {
  Rig rig;
  rig.pushMap(100);
  rig.pushUpdate(200, 1, 9.0F);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(150);
  EXPECT_FALSE(rig.layer.stagedColorDelta().has_value()) << "the first stage is whole";
  const auto revision = rig.layer.stagedRevision();
  const auto geometry = rig.layer.stagedMeshForTest()->geometry_revision;

  rig.layer.renderAtForTest(250);
  EXPECT_EQ(rig.layer.stagedRevision(), revision + 1);
  ASSERT_TRUE(rig.layer.stagedColorDelta().has_value());
  EXPECT_EQ(*rig.layer.stagedColorDelta(), (pj::scene3d::ElementRange{.begin = 3, .end = 6}));
  EXPECT_EQ(rig.layer.stagedMeshForTest()->geometry_revision, geometry);
  EXPECT_FLOAT_EQ(rig.stagedCost(0), 1.0F);
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 9.0F);
  EXPECT_FLOAT_EQ(rig.layer.stagedMeshForTest()->color_range.hi, 9.0F);

  rig.layer.renderAtForTest(150);
  EXPECT_FALSE(rig.layer.stagedColorDelta().has_value());
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 3.0F);
}

// An update to a channel the mesh is not coloured by changes nothing on screen.
TEST(MeshMapLayer, AnUpdateToAnotherChannelDoesNotRestage) {
  Rig rig;
  rig.pushMap(100);
  rig.pushUpdate(200, 1, 9.0F);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.setColorBy(QString::fromUtf8(kMeshMapSourceVertexColor.data()));
  rig.layer.renderAtForTest(150);
  const auto revision = rig.layer.stagedRevision();
  rig.layer.renderAtForTest(250);
  EXPECT_EQ(rig.layer.appliedUpdateCountForTest(), 1);
  EXPECT_EQ(rig.layer.stagedRevision(), revision);

  rig.layer.setColorBy(u"face:cost"_s);  // the update is there once it is shown
  rig.layer.renderAtForTest(250);
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 9.0F);
}

// Updates for another mesh, on a topic this map never paired with, neither
// rebuild nor re-stage it, whether they arrive in order or late; a late update
// that does name this geometry still applies.
TEST(MeshMapLayer, AnotherMeshsUpdatesLeaveThisLayerAlone) {
  Rig rig;
  const PJ::ObjectTopicId other = rig.registerUpdateTopic("/other_mesh/costs");
  rig.pushMap(100);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(300);
  const int rebuilds = rig.layer.rebaseCountForTest();
  const auto revision = rig.layer.stagedRevision();

  rig.pushUpdate(400, 0, 7.0F, "2", other);  // in order
  rig.layer.renderAtForTest(500);
  EXPECT_EQ(rig.layer.stagedRevision(), revision) << "re-staged for another mesh's update";
  rig.pushUpdate(200, 0, 7.0F, "2", other);  // late
  rig.layer.renderAtForTest(500);
  EXPECT_EQ(rig.layer.rebaseCountForTest(), rebuilds) << "rebuilt for another mesh's late update";
  EXPECT_EQ(rig.layer.stagedRevision(), revision);
  EXPECT_FLOAT_EQ(rig.stagedCost(0), 1.0F);

  rig.pushUpdate(250, 1, 6.0F, "1", other);  // late, and this map's
  rig.layer.renderAtForTest(500);
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 6.0F);
}

// A refused map is decoded once per (map, updates) state, not on every tick, and
// keeps its warning.
TEST(MeshMapLayer, ARefusedMapIsNotDecodedAgain) {
  Rig rig;
  rig.pushMap(100, "1", [](MeshMap& mesh) { mesh.face_count = 3; });  // fewer indices than faces: invalid
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(100);
  EXPECT_EQ(rig.layer.stagedMeshForTest(), nullptr);
  EXPECT_FALSE(rig.layer.statusWarning().isEmpty());
  const int decodes = rig.layer.rebaseCountForTest();
  rig.layer.renderAtForTest(100);
  rig.layer.renderAtForTest(150);
  EXPECT_EQ(rig.layer.rebaseCountForTest(), decodes);
  EXPECT_FALSE(rig.layer.statusWarning().isEmpty());
}

// An origin pose that cannot place the mesh refuses it, as a bad grid origin does.
TEST(MeshMapLayer, ANonFiniteOriginIsRefused) {
  Rig rig;
  rig.pushMap(100, "1", [](MeshMap& mesh) { mesh.origin.position.x = std::numeric_limits<double>::quiet_NaN(); });
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(100);
  EXPECT_EQ(rig.layer.stagedMeshForTest(), nullptr);
  EXPECT_FALSE(rig.layer.worldBounds().has_value());
  EXPECT_FALSE(rig.layer.statusWarning().isEmpty());
}

TEST(MeshMapLayer, PairsAnyUpdateTopicByGeometryId) {
  Rig rig;
  const PJ::ObjectTopicId costs = rig.registerUpdateTopic("/mesh/vertex_costs");
  rig.pushMap(100, "42");
  rig.pushUpdate(200, 0, 7.0F, "42", costs);  // matches: applies
  rig.pushUpdate(250, 1, 8.0F, "", costs);    // no id on a non-sibling topic: ignored
  rig.pushUpdate(260, 1, 6.0F, "41", costs);  // another geometry: ignored
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  EXPECT_EQ(rig.layer.updateTopicCountForTest(), 2u);
  rig.layer.renderAtForTest(300);
  EXPECT_FLOAT_EQ(rig.stagedCost(0), 7.0F);
  EXPECT_FLOAT_EQ(rig.stagedCost(1), 3.0F);
  EXPECT_TRUE(rig.layer.statusWarning().isEmpty()) << "a shared topic's other geometries are not this map's fault";
}

TEST(MeshMapLayer, SiblingUpdatesForAnotherGeometryAreSkippedWithAWarning) {
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

  rig.layer.setColorBy(QString::fromUtf8(kMeshMapSourceVertexColor.data(), kMeshMapSourceVertexColor.size()));
  rig.layer.renderAtForTest(100);
  ASSERT_NE(rig.layer.stagedMeshForTest(), nullptr);
  EXPECT_EQ(rig.layer.stagedMeshForTest()->color_mode, MeshMapColorMode::kRgba);

  rig.layer.setColorBy(QString::fromUtf8(kMeshMapSourceSolid.data(), kMeshMapSourceSolid.size()));
  rig.layer.renderAtForTest(100);
  EXPECT_EQ(rig.layer.stagedMeshForTest()->color_mode, MeshMapColorMode::kSolid);

  rig.layer.setColorBy(u"vertex:red"_s);  // a vertex channel through the colormap
  rig.layer.renderAtForTest(100);
  EXPECT_EQ(rig.layer.stagedMeshForTest()->color_mode, MeshMapColorMode::kChannel);

  rig.layer.setColorBy(u"face:elevation"_s);  // not in this map: falls back, warns, keeps the request
  rig.layer.renderAtForTest(100);
  EXPECT_EQ(rig.layer.resolvedColorByForTest(), u"face:cost"_s);
  EXPECT_FALSE(rig.layer.statusWarning().isEmpty());

  rig.layer.setColorBy(u"face:cost"_s);  // a valid request clears the warning
  rig.layer.renderAtForTest(100);
  EXPECT_TRUE(rig.layer.statusWarning().isEmpty());
}

TEST(MeshMapLayer, XmlRoundTrip) {
  Rig rig;
  rig.layer.setColorBy(u"face:cost"_s);
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
  EXPECT_EQ(restored.coloring().colormap(), PJ::Colormap::kViridis);
  EXPECT_FALSE(restored.coloring().autoRange());
  EXPECT_DOUBLE_EQ(restored.coloring().manualRange().first, -1.0);
  EXPECT_DOUBLE_EQ(restored.coloring().manualRange().second, 2.5);
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
