// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Behavioural coverage for GridMapLayer: the parser and canonical-codec resolve
// routes, field auto-pick vs a requested field that is missing (fallback + status
// warning), the staged upload happening only on a sample / selector change, NaN
// excluded from the auto colormap range, renderKey without a payload resolve,
// source-frame bounds with a rotated origin, XML round-trip, and the dataset
// replace / detach reset.

#include "pj_scene3d_widgets/layers/grid_map_layer.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QDomDocument>
#include <QString>
#include <QWidget>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/grid_map.hpp"
#include "pj_base/builtin/grid_map_codec.hpp"
#include "pj_base/time.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene3d_widgets/transform_service.h"
#include "pj_widgets/DoubleScrubber.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;
using PJ::sdk::PointField;

constexpr std::string_view kSchema = "mock/grid_map";
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr int kCols = 4;
constexpr int kRows = 3;
constexpr int kHole = 6;  // cell (c=2, r=1)

std::atomic<int> g_parser_calls{0};
std::atomic<bool> g_emit_oversized{false};   // parser route: declare more rows than the payload backs
std::atomic<bool> g_emit_wrong_type{false};  // parser route: hand back an object that is not a GridMap

// 4x3 cells, two interleaved float32 fields: elevation = cell index (NaN at kHole),
// cost = 100 + cell index. The static buffer outlives every parse.
const std::vector<uint8_t>& gridBytes() {
  static const std::vector<uint8_t> bytes = [] {
    std::vector<uint8_t> out(kCols * kRows * 8);
    for (int i = 0; i < kCols * kRows; ++i) {
      const float elevation = i == kHole ? kNaN : static_cast<float>(i);
      const float cost = 100.0f + static_cast<float>(i);
      std::memcpy(out.data() + i * 8, &elevation, 4);
      std::memcpy(out.data() + i * 8 + 4, &cost, 4);
    }
    return out;
  }();
  return bytes;
}

PJ::sdk::GridMap makeGrid(PJ::Timestamp ts) {
  PJ::sdk::GridMap grid;
  grid.timestamp_ns = ts;
  grid.frame_id = "map";
  grid.origin.position = {1.0, 2.0, 0.0};
  grid.cell_size = {0.5, 0.5};
  grid.column_count = kCols;
  grid.row_count = kRows;
  grid.cell_stride = 8;
  grid.row_stride = 8 * kCols;
  grid.fields = {
      PointField{.name = "elevation", .offset = 0, .datatype = PointField::Datatype::kFloat32, .count = 1},
      PointField{.name = "cost", .offset = 4, .datatype = PointField::Datatype::kFloat32, .count = 1}};
  grid.data = PJ::Span<const uint8_t>(gridBytes().data(), gridBytes().size());
  return grid;
}

PJ::Expected<PJ::sdk::ObjectRecord> emitGrid(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  if (g_emit_wrong_type.load()) {
    return PJ::sdk::ObjectRecord{.ts = ts, .object = std::string("not a grid map")};
  }
  PJ::sdk::GridMap grid = makeGrid(ts);
  if (g_emit_oversized.load()) {
    grid.row_count = 30;
  }
  return PJ::sdk::ObjectRecord{.ts = ts, .object = grid};
}

void registerGridParser(PJ::SessionManager& session, PJ::ObjectTopicId topic_id) {
  session.registerObjectTopicParser(topic_id, makeBoundHandle(kSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kSchema, PJ::sdk::BuiltinObjectType::kGridMap, &g_parser_calls, &emitGrid);
                                    }));
}

struct Rig {
  PJ::SessionManager session;
  PJ::ObjectTopicId topic_id = registerObjectTopic(session, "/grid_map");
  pj::scene3d::Scene3DLayerContext ctx;
  pj::scene3d::GridMapLayer layer{topic_id, u"grid"_s};

  Rig() {
    g_parser_calls.store(0);
    ctx.session = &session;
  }
  void pushAndBind(PJ::Timestamp ts = 100) {
    ASSERT_TRUE(session.objectStore().pushOwned(topic_id, ts, std::vector<uint8_t>{0x01}).has_value());
    registerGridParser(session, topic_id);
  }
  // Canonical route: a serialized PJ.GridMap blob and no parser.
  void pushCanonical(const PJ::sdk::GridMap& grid) {
    ASSERT_TRUE(session.objectStore().pushOwned(topic_id, grid.timestamp_ns, PJ::serializeGridMap(grid)).has_value());
  }
};

TEST(GridMapLayer, AutoPickAndRescrubDoesNoRepack) {
  Rig rig;
  rig.pushAndBind();
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  EXPECT_EQ(g_parser_calls.load(), 1);  // bootstrap decoded the first sample
  EXPECT_EQ(rig.layer.resolvedElevationFieldForTest(), u"elevation"_s);

  rig.layer.renderAtForTest(100);
  EXPECT_TRUE(rig.layer.hasGridForTest());
  EXPECT_TRUE(rig.layer.passHasStagedGridForTest());
  EXPECT_EQ(rig.layer.packCountForTest(), 1);
  EXPECT_EQ(rig.layer.resolvedColorFieldForTest(), u"elevation"_s);  // color follows elevation
  const int parses = g_parser_calls.load();

  rig.layer.renderAtForTest(100);
  rig.layer.renderAtForTest(150);
  EXPECT_EQ(rig.layer.packCountForTest(), 1) << "re-scrub to the staged sample must not re-pack";
  EXPECT_EQ(g_parser_calls.load(), parses);
}

TEST(GridMapLayer, StagedUploadHoldsNaNHoleAndFiniteAutoRange) {
  Rig rig;
  rig.pushAndBind();
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(100);

  const auto& staged = rig.layer.stagedGridForTest();
  EXPECT_EQ(staged.column_count, 4u);
  EXPECT_EQ(staged.row_count, 3u);
  EXPECT_EQ(staged.frame_id, "map");
  ASSERT_EQ(staged.elevation.size(), 12u);
  EXPECT_TRUE(std::isnan(staged.elevation[kHole]));
  EXPECT_FLOAT_EQ(staged.elevation[11], 11.0f);
  EXPECT_TRUE(staged.color_is_elevation) << "color following elevation must not pack the channel twice";
  EXPECT_TRUE(staged.color.empty());
  ASSERT_TRUE(staged.color_range.valid);
  EXPECT_FLOAT_EQ(staged.color_range.lo, 0.0f);
  EXPECT_FLOAT_EQ(staged.color_range.hi, 11.0f);  // NaN never widens or poisons the range
}

TEST(GridMapLayer, SelectorChangeRepacksFromCachedGridWithoutReparse) {
  Rig rig;
  rig.pushAndBind();
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(100);
  const int parses = g_parser_calls.load();

  rig.layer.setColorField(u"cost"_s);
  rig.layer.renderAtForTest(100);
  EXPECT_EQ(g_parser_calls.load(), parses) << "a selector change must reuse the cached canonical grid";
  EXPECT_EQ(rig.layer.packCountForTest(), 2);
  EXPECT_EQ(rig.layer.resolvedColorFieldForTest(), u"cost"_s);
  const auto& staged = rig.layer.stagedGridForTest();
  EXPECT_FALSE(staged.color_is_elevation);
  EXPECT_FLOAT_EQ(staged.color[kHole], 106.0f);  // the hole is per field
  EXPECT_FLOAT_EQ(staged.color_range.lo, 100.0f);
  EXPECT_FLOAT_EQ(staged.color_range.hi, 111.0f);
}

TEST(GridMapLayer, MissingRequestedFieldFallsBackWithWarningAndNoneIsFlat) {
  Rig rig;
  rig.pushAndBind();
  rig.layer.setElevationField(u"height"_s);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(100);
  EXPECT_EQ(rig.layer.resolvedElevationFieldForTest(), u"elevation"_s);
  EXPECT_TRUE(rig.layer.statusWarning().contains(u"height"_s)) << rig.layer.statusWarning().toStdString();
  EXPECT_TRUE(rig.layer.passHasStagedGridForTest());

  // The request is persisted verbatim so it is honoured once the field appears.
  QDomDocument doc;
  EXPECT_EQ(rig.layer.xmlSaveState(doc).attribute(u"elevation_field"_s), u"height"_s);

  rig.layer.setElevationField(QString::fromUtf8(pj::scene3d::kGridMapElevationNone));
  rig.layer.renderAtForTest(100);
  EXPECT_TRUE(rig.layer.resolvedElevationFieldForTest().isEmpty());
  EXPECT_TRUE(rig.layer.stagedGridForTest().elevation.empty());
  EXPECT_EQ(rig.layer.resolvedColorFieldForTest(), u"elevation"_s);  // first scalar field
  EXPECT_TRUE(rig.layer.statusWarning().isEmpty());
  const auto bounds = rig.layer.worldBounds();
  ASSERT_TRUE(bounds.has_value());
  EXPECT_FLOAT_EQ(bounds->min.z, 0.0f);
  EXPECT_FLOAT_EQ(bounds->max.z, 0.0f);
}

TEST(GridMapLayer, RenderKeyNeverResolvesPayload) {
  Rig rig;
  auto fetch_count = std::make_shared<int>(0);
  pushLazyCounting(rig.session.objectStore(), rig.topic_id, 100, std::vector<uint8_t>{0x01}, fetch_count);
  registerGridParser(rig.session, rig.topic_id);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  const int fetches_after_attach = *fetch_count;

  const uint64_t key_a = rig.layer.renderKey(PJ::fromRaw(100));
  const uint64_t key_before = rig.layer.renderKey(PJ::fromRaw(50));
  EXPECT_EQ(*fetch_count, fetches_after_attach) << "renderKey resolved payload bytes";
  EXPECT_NE(key_a, key_before);

  rig.layer.renderAtForTest(100);
  const int fetches_after_render = *fetch_count;
  auto old_fetch_count = std::make_shared<int>(0);
  pushLazyCounting(rig.session.objectStore(), rig.topic_id, 50, std::vector<uint8_t>{0x01}, old_fetch_count);
  rig.layer.renderAtForTest(101);  // same active sample: metadata-only skip
  EXPECT_EQ(*fetch_count, fetches_after_render);
  EXPECT_EQ(*old_fetch_count, 0);
}

TEST(GridMapLayer, WorldBoundsAreSourceFrameWithRotatedOrigin) {
  Rig rig;
  // Canonical route with a rotated origin: 90 degrees about z, so local +x maps to +y.
  PJ::sdk::GridMap grid = makeGrid(100);
  grid.origin.orientation = {0.0, 0.0, std::sqrt(0.5), std::sqrt(0.5)};
  rig.pushCanonical(grid);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(100);
  const auto bounds = rig.layer.worldBounds();
  ASSERT_TRUE(bounds.has_value());
  // Local extent 2.0 x 1.5 rotated: x spans [1 - 1.5, 1], y spans [2, 4]; z is the elevation range.
  EXPECT_NEAR(bounds->min.x, -0.5f, 1e-4f);
  EXPECT_NEAR(bounds->max.x, 1.0f, 1e-4f);
  EXPECT_NEAR(bounds->min.y, 2.0f, 1e-4f);
  EXPECT_NEAR(bounds->max.y, 4.0f, 1e-4f);
  EXPECT_NEAR(bounds->min.z, 0.0f, 1e-4f);
  EXPECT_NEAR(bounds->max.z, 11.0f, 1e-4f);
}

TEST(GridMapLayer, DockTransformsSourceBoundsIntoFixedFrame) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerObjectTopic(session, "/grid_map");
  ASSERT_TRUE(session.objectStore().pushOwned(topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  registerGridParser(session, topic_id);

  pj::scene3d::TransformService transform_service(session);
  const auto tf_buffer = transform_service.transformBuffer(1);
  pj::scene3d::StampedTransform fixed_from_map;
  fixed_from_map.stamp = PJ::fromRaw(100);
  fixed_from_map.parent_frame = "world";
  fixed_from_map.child_frame = "map";
  fixed_from_map.transform.t = {100.0, 200.0, 300.0};
  ASSERT_TRUE(tf_buffer->setTransform(fixed_from_map).has_value());

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(topic_id, PJ::sdk::BuiltinObjectType::kGridMap, u"grid"_s));
  dock.onTrackerTime(0.0);  // the latched grid clamps the render time to its 100 ns sample
  dock.setFixedFrame(u"world"_s);
  auto* layer = dynamic_cast<pj::scene3d::GridMapLayer*>(dock.layerFor(topic_id));
  ASSERT_NE(layer, nullptr);
  layer->renderAtForTest(100);

  const auto bounds = dock.sceneView()->sceneBoundsForTest();
  ASSERT_TRUE(bounds.valid);
  EXPECT_FLOAT_EQ(bounds.min.x, 101.0f);
  EXPECT_FLOAT_EQ(bounds.max.x, 103.0f);
  EXPECT_FLOAT_EQ(bounds.min.y, 202.0f);
  EXPECT_FLOAT_EQ(bounds.max.y, 203.5f);
  EXPECT_FLOAT_EQ(bounds.min.z, 300.0f);
  EXPECT_FLOAT_EQ(bounds.max.z, 311.0f);
}

// Bounds are computed inside renderAt (during paint) after the dock has already
// unioned the layers' bounds for this frame, so a changed extent must ask for
// another repaint (PointCloudLayer::onGpuAabb does the same).
TEST(GridMapLayer, BoundsChangeRequestsRepaint) {
  Rig rig;
  rig.pushAndBind();
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  int repaints = 0;
  QObject::connect(&rig.layer, &PJ::ISceneLayer::repaintRequested, &rig.layer, [&repaints]() { ++repaints; });

  rig.layer.renderAtForTest(100);
  ASSERT_TRUE(rig.layer.worldBounds().has_value());
  EXPECT_EQ(repaints, 1) << "a first sample must re-fit the scene bounds";
  rig.layer.renderAtForTest(100);
  EXPECT_EQ(repaints, 1) << "an unchanged extent must not repaint";

  rig.layer.renderAtForTest(50);  // before the only sample: no bounds
  EXPECT_FALSE(rig.layer.worldBounds().has_value());
  EXPECT_EQ(repaints, 2);
  rig.layer.renderAtForTest(50);
  EXPECT_EQ(repaints, 2);
}

TEST(GridMapLayer, DatasetReplaceRequestsRepaint) {
  Rig rig;
  rig.pushAndBind();
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(100);
  int repaints = 0;
  QObject::connect(&rig.layer, &PJ::ISceneLayer::repaintRequested, &rig.layer, [&repaints]() { ++repaints; });
  emit rig.session.datasetAboutToBeReplaced(rig.session.objectStore().descriptor(rig.topic_id).dataset_id);
  EXPECT_EQ(repaints, 1) << "the invalidated grid must be repainted from the new bytes";
}

// The resolve memo must fingerprint the schema, not just the names: swapping the
// datatypes of two same-named fields changes which one auto-picks as elevation.
TEST(GridMapLayer, FieldSchemaChangeWithSameNamesReResolves) {
  Rig rig;
  const auto with_types = [](PJ::Timestamp ts, PointField::Datatype a_type, PointField::Datatype b_type) {
    PJ::sdk::GridMap grid = makeGrid(ts);
    grid.fields = {
        PointField{.name = "a", .offset = 0, .datatype = a_type, .count = 1},
        PointField{.name = "b", .offset = 4, .datatype = b_type, .count = 1}};
    return grid;
  };
  rig.pushCanonical(with_types(100, PointField::Datatype::kUint32, PointField::Datatype::kFloat32));
  rig.pushCanonical(with_types(200, PointField::Datatype::kFloat32, PointField::Datatype::kUint32));
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  int fields_changed = 0;
  QObject::connect(
      &rig.layer, &pj::scene3d::GridMapLayer::fieldsChanged, &rig.layer, [&fields_changed]() { ++fields_changed; });

  rig.layer.renderAtForTest(100);
  EXPECT_EQ(rig.layer.resolvedElevationFieldForTest(), u"b"_s) << "the first float field";
  EXPECT_EQ(fields_changed, 0) << "attach already resolved the first sample: a memo hit";
  rig.layer.renderAtForTest(200);
  EXPECT_EQ(rig.layer.resolvedElevationFieldForTest(), u"a"_s) << "same names, swapped types: must re-resolve";
  EXPECT_EQ(fields_changed, 1);
  rig.layer.renderAtForTest(100);
  rig.layer.renderAtForTest(200);
  EXPECT_EQ(fields_changed, 3);
}

// A panel built before the first sample (streaming) lists no fields; it must
// fill in, and select the effective picks, once the first grid is decoded,
// without the rebuild counting as a user edit.
TEST(GridMapLayer, ConfigPanelGainsFieldsFromTheFirstSample) {
  Rig rig;
  registerGridParser(rig.session, rig.topic_id);  // parser, but NO sample yet
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  std::unique_ptr<QWidget> panel(rig.layer.createConfigWidget(nullptr));
  auto* elevation = panel->findChild<QComboBox*>(u"grid_map_elevation_field"_s);
  auto* color = panel->findChild<QComboBox*>(u"grid_map_color_field"_s);
  ASSERT_NE(elevation, nullptr);
  ASSERT_NE(color, nullptr);
  EXPECT_EQ(elevation->count(), 1) << "only the None entry before any sample";
  EXPECT_EQ(color->count(), 0);
  EXPECT_FALSE(color->isEnabled());
  int configuration_changes = 0;
  QObject::connect(&rig.layer, &PJ::ISceneLayer::configurationChanged, &rig.layer, [&configuration_changes]() {
    ++configuration_changes;
  });

  ASSERT_TRUE(rig.session.objectStore().pushOwned(rig.topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  rig.layer.renderAtForTest(100);
  EXPECT_EQ(elevation->count(), 3);
  EXPECT_EQ(elevation->currentData().toString(), u"elevation"_s);
  EXPECT_EQ(color->count(), 2);
  EXPECT_EQ(color->currentData().toString(), u"elevation"_s);
  EXPECT_TRUE(color->isEnabled());
  EXPECT_EQ(configuration_changes, 0) << "a combo rebuild is not a user edit";

  panel.reset();
  rig.layer.setColorField(u"cost"_s);
  rig.layer.renderAtForTest(100);  // the fieldsChanged wire must be dead with its panel
}

// A selected elevation channel with no finite sample draws nothing, so it
// contributes no bounds; only the explicit "None" flat mode is a plane at z = 0.
TEST(GridMapLayer, AllNaNElevationHasNoBoundsButNoneIsFlat) {
  Rig rig;
  std::vector<uint8_t> bytes(kCols * kRows * 8);
  for (int i = 0; i < kCols * kRows; ++i) {
    const float cost = 100.0f + static_cast<float>(i);
    std::memcpy(bytes.data() + i * 8, &kNaN, 4);
    std::memcpy(bytes.data() + i * 8 + 4, &cost, 4);
  }
  PJ::sdk::GridMap grid = makeGrid(100);
  grid.data = PJ::Span<const uint8_t>(bytes.data(), bytes.size());
  rig.pushCanonical(grid);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(100);
  EXPECT_EQ(rig.layer.resolvedElevationFieldForTest(), u"elevation"_s);
  EXPECT_TRUE(rig.layer.passHasStagedGridForTest());
  EXPECT_FALSE(rig.layer.worldBounds().has_value()) << "an all-NaN heightfield has no extent";

  rig.layer.setElevationField(QString::fromUtf8(pj::scene3d::kGridMapElevationNone));
  rig.layer.renderAtForTest(100);
  const auto bounds = rig.layer.worldBounds();
  ASSERT_TRUE(bounds.has_value());
  EXPECT_FLOAT_EQ(bounds->min.z, 0.0f);
  EXPECT_FLOAT_EQ(bounds->max.z, 0.0f);
}

TEST(GridMapLayer, CanonicalCodecRouteStagesTheSameUpload) {
  Rig rig;
  rig.pushCanonical(makeGrid(100));
  pj::scene3d::GridMapLayer& layer = rig.layer;
  ASSERT_TRUE(layer.attach(rig.ctx)) << "a parser-less canonical PJ.GridMap topic must attach";
  EXPECT_EQ(layer.sourceFrame(), u"map"_s);
  layer.setColorField(u"cost"_s);
  layer.renderAtForTest(100);
  ASSERT_TRUE(layer.passHasStagedGridForTest());
  const auto& staged = layer.stagedGridForTest();
  ASSERT_EQ(staged.elevation.size(), 12u);
  ASSERT_EQ(staged.color.size(), 12u);
  EXPECT_TRUE(std::isnan(staged.elevation[kHole]));
  EXPECT_FLOAT_EQ(staged.elevation[5], 5.0f);
  EXPECT_FLOAT_EQ(staged.color[5], 105.0f);
  EXPECT_FLOAT_EQ(staged.cell_size.x, 0.5f);
  const auto bounds = layer.worldBounds();
  ASSERT_TRUE(bounds.has_value());
  EXPECT_FLOAT_EQ(bounds->min.x, 1.0f);
  EXPECT_FLOAT_EQ(bounds->max.x, 3.0f);
  EXPECT_FLOAT_EQ(bounds->min.y, 2.0f);
  EXPECT_FLOAT_EQ(bounds->max.y, 3.5f);
  EXPECT_FLOAT_EQ(bounds->max.z, 11.0f);
}

// Parser route: the parser does not validate, so the layer's validateGridMap()
// call is what refuses the layout.
TEST(GridMapLayer, ParserGridFailingValidationSurfacesWarningAndClears) {
  Rig rig;
  rig.pushAndBind();
  g_emit_oversized.store(true);
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(100);
  g_emit_oversized.store(false);
  EXPECT_FALSE(rig.layer.passHasStagedGridForTest());
  EXPECT_FALSE(rig.layer.worldBounds().has_value());
  EXPECT_TRUE(rig.layer.statusWarning().contains(u"Invalid grid map"_s)) << rig.layer.statusWarning().toStdString();
  const int parses = g_parser_calls.load();
  rig.layer.renderAtForTest(100);
  EXPECT_EQ(g_parser_calls.load(), parses) << "a refused sample must not be re-decoded every tick";
}

// Canonical route: the codec itself rejects the layout at decode time.
TEST(GridMapLayer, CanonicalDecodeFailureSurfacesWarningAndClears) {
  Rig rig;
  PJ::sdk::GridMap grid = makeGrid(100);
  grid.row_count = 30;  // declares more cells than the payload carries
  rig.pushCanonical(grid);
  pj::scene3d::GridMapLayer& layer = rig.layer;
  ASSERT_TRUE(layer.attach(rig.ctx));
  layer.renderAtForTest(100);
  EXPECT_FALSE(layer.passHasStagedGridForTest());
  EXPECT_FALSE(layer.worldBounds().has_value());
  EXPECT_FALSE(layer.statusWarning().isEmpty());
}

// A sample shown with a warning, then a scrub into the gap before it: the
// warning describes nothing on screen any more, so it clears. The resolve memo
// survives the gap: scrubbing back onto the same schema is a memo hit that
// re-applies its warning.
TEST(GridMapLayer, EmptyTickClearsTheActiveWarningAndKeepsTheResolveMemo) {
  Rig rig;
  rig.pushAndBind();
  rig.layer.setElevationField(u"height"_s);  // missing: falls back with a warning
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(100);
  ASSERT_FALSE(rig.layer.statusWarning().isEmpty());
  int fields_changed = 0;
  QObject::connect(
      &rig.layer, &pj::scene3d::GridMapLayer::fieldsChanged, &rig.layer, [&fields_changed]() { ++fields_changed; });

  rig.layer.renderAtForTest(50);
  EXPECT_TRUE(rig.layer.statusWarning().isEmpty()) << "nothing is shown: nothing to warn about";
  rig.layer.renderAtForTest(100);
  EXPECT_TRUE(rig.layer.passHasStagedGridForTest());
  EXPECT_FALSE(rig.layer.statusWarning().isEmpty());
  EXPECT_EQ(fields_changed, 0) << "the same schema after the gap must hit the resolve memo";
}

// The parser route can hand back any object type; one that is not a GridMap is
// refused like a decode failure (warning, cleared, not retried every tick)
// instead of leaving the previous map on screen.
TEST(GridMapLayer, ParserObjectOfAnotherTypeIsRefusedOnce) {
  Rig rig;
  rig.pushAndBind();
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(100);
  ASSERT_TRUE(rig.layer.passHasStagedGridForTest());

  ASSERT_TRUE(rig.session.objectStore().pushOwned(rig.topic_id, 200, std::vector<uint8_t>{0x02}).has_value());
  g_emit_wrong_type.store(true);
  rig.layer.renderAtForTest(200);
  g_emit_wrong_type.store(false);
  EXPECT_FALSE(rig.layer.passHasStagedGridForTest()) << "the old map must not stay visible";
  EXPECT_FALSE(rig.layer.worldBounds().has_value());
  EXPECT_FALSE(rig.layer.statusWarning().isEmpty());
  const int parses = g_parser_calls.load();
  rig.layer.renderAtForTest(200);
  EXPECT_EQ(g_parser_calls.load(), parses) << "a refused sample must not be re-decoded every tick";
}

TEST(GridMapLayer, StreamingAttachBeforeSampleAndBackScrub) {
  Rig rig;
  registerGridParser(rig.session, rig.topic_id);  // parser, but NO sample yet
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  EXPECT_EQ(g_parser_calls.load(), 0);
  rig.layer.renderAtForTest(100);
  EXPECT_FALSE(rig.layer.hasGridForTest());

  ASSERT_TRUE(rig.session.objectStore().pushOwned(rig.topic_id, 100, std::vector<uint8_t>{0x01}).has_value());
  rig.layer.renderAtForTest(100);
  EXPECT_TRUE(rig.layer.passHasStagedGridForTest());
  EXPECT_EQ(rig.layer.sourceFrame(), u"map"_s);

  rig.layer.renderAtForTest(50);  // before the only sample: cleared
  EXPECT_FALSE(rig.layer.passHasStagedGridForTest());
  rig.layer.renderAtForTest(100);
  EXPECT_TRUE(rig.layer.passHasStagedGridForTest()) << "grid must re-stage after a back-scrub through an empty tick";
}

TEST(GridMapLayer, DatasetReplaceAndDetachResetEverything) {
  Rig rig;
  rig.pushAndBind();
  rig.layer.setElevationField(u"height"_s);  // missing: leaves a resolve warning behind
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  rig.layer.renderAtForTest(100);
  ASSERT_TRUE(rig.layer.hasGridForTest());
  ASSERT_FALSE(rig.layer.statusWarning().isEmpty());

  // Only the persisted requests and display settings survive: every identity
  // learned from the old bytes (frame, field list, effective picks, warning, the
  // staged arrays themselves) is gone.
  const auto expect_reset = [&rig]() {
    EXPECT_FALSE(rig.layer.hasGridForTest());
    EXPECT_FALSE(rig.layer.passHasStagedGridForTest());
    EXPECT_TRUE(rig.layer.stagedGridForTest().elevation.empty()) << "clearGrid must drop the staged arrays";
    EXPECT_EQ(rig.layer.stagedGridForTest().column_count, 0u);
    EXPECT_FALSE(rig.layer.worldBounds().has_value());
    EXPECT_TRUE(rig.layer.sourceFrame().isEmpty());
    EXPECT_TRUE(rig.layer.fallbackFrames().isEmpty());
    EXPECT_TRUE(rig.layer.fieldNamesForTest().empty());
    EXPECT_TRUE(rig.layer.resolvedElevationFieldForTest().isEmpty());
    EXPECT_TRUE(rig.layer.resolvedColorFieldForTest().isEmpty());
    EXPECT_TRUE(rig.layer.statusWarning().isEmpty());
  };
  emit rig.session.datasetAboutToBeReplaced(rig.session.objectStore().descriptor(rig.topic_id).dataset_id);
  expect_reset();

  rig.layer.renderAtForTest(100);  // the (unchanged) sample re-stages from scratch
  EXPECT_TRUE(rig.layer.passHasStagedGridForTest());
  EXPECT_EQ(rig.layer.packCountForTest(), 2);
  EXPECT_EQ(rig.layer.sourceFrame(), u"map"_s);
  EXPECT_EQ(rig.layer.resolvedElevationFieldForTest(), u"elevation"_s);
  EXPECT_FALSE(rig.layer.statusWarning().isEmpty()) << "the persisted request is re-resolved against the new bytes";

  rig.layer.detach();
  expect_reset();
  QDomDocument doc;
  EXPECT_EQ(rig.layer.xmlSaveState(doc).attribute(u"elevation_field"_s), u"height"_s) << "requests persist";
}

// The parser route hands back an object whose anchor was allocated inside the
// plugin DSO; caching it must pin the binding keepalive until the cache drops, or
// the extension catalog unloads the DSO first and the cached grid's dtor faults.
TEST(GridMapLayer, CachedParserGridHoldsTheBindingKeepalive) {
  Rig rig;
  rig.pushAndBind();
  ASSERT_TRUE(rig.layer.attach(rig.ctx));
  const auto keepalive = rig.session.parserKeepaliveForObjectTopic(rig.topic_id);
  ASSERT_NE(keepalive, nullptr);
  const long baseline = keepalive.use_count();

  rig.layer.renderAtForTest(100);
  ASSERT_TRUE(rig.layer.hasGridForTest());
  EXPECT_GT(keepalive.use_count(), baseline) << "a cached parser-decoded grid must hold the binding keepalive";

  rig.layer.detach();
  EXPECT_EQ(keepalive.use_count(), baseline) << "detach must release the keepalive with the cached grid";
}

TEST(GridMapLayer, XmlRoundTrip) {
  Rig rig;
  pj::scene3d::GridMapLayer& source = rig.layer;
  source.setElevationField(u"elevation"_s);
  source.setColorField(u"cost"_s);
  source.setColormap(PJ::Colormap::kViridis);
  source.setAutoRange(false);
  source.setManualRange(-2.0, 8.0);
  source.setLighting(false);
  source.setOpacity(0.5);

  QDomDocument doc;
  const QDomElement saved = source.xmlSaveState(doc);

  pj::scene3d::GridMapLayer restored(rig.topic_id, u"grid"_s);
  int configuration_changes = 0;
  QObject::connect(&restored, &PJ::ISceneLayer::configurationChanged, &restored, [&configuration_changes]() {
    ++configuration_changes;
  });
  ASSERT_TRUE(restored.xmlLoadState(saved));
  EXPECT_GT(configuration_changes, 0);
  configuration_changes = 0;
  ASSERT_TRUE(restored.xmlLoadState(saved));
  EXPECT_EQ(configuration_changes, 0) << "re-applying identical parameters is not a workspace mutation";

  QDomDocument doc2;
  const QDomElement re_saved = restored.xmlSaveState(doc2);
  EXPECT_EQ(re_saved.attribute("elevation_field"), u"elevation"_s);
  EXPECT_EQ(re_saved.attribute("color_field"), u"cost"_s);
  EXPECT_EQ(re_saved.attribute("colormap").toInt(), static_cast<int>(PJ::Colormap::kViridis));
  EXPECT_EQ(re_saved.attribute("auto_range").toInt(), 0);
  EXPECT_DOUBLE_EQ(re_saved.attribute("range_lo").toDouble(), -2.0);
  EXPECT_DOUBLE_EQ(re_saved.attribute("range_hi").toDouble(), 8.0);
  EXPECT_EQ(re_saved.attribute("lighting").toInt(), 0);
  EXPECT_DOUBLE_EQ(re_saved.attribute("opacity").toDouble(), 0.5);

  QDomElement bad = doc.createElement(u"grid_map"_s);
  bad.setAttribute(u"opacity"_s, 2.0);
  EXPECT_FALSE(restored.xmlLoadState(bad));
}

// The setter normalizes what xmlLoadState rejects, from one rule: finite,
// float-representable, ordered. Whatever the setter accepts round-trips.
TEST(GridMapLayer, ManualRangeIsNormalizedBySetterAndRejectedOnLoad) {
  Rig rig;
  pj::scene3d::GridMapLayer& layer = rig.layer;
  layer.setManualRange(10.0, 1.0);
  EXPECT_EQ(layer.manualRangeForTest(), (std::pair{1.0, 10.0}));
  layer.setManualRange(std::numeric_limits<double>::quiet_NaN(), 5.0);
  EXPECT_EQ(layer.manualRangeForTest(), (std::pair{1.0, 10.0})) << "a non-finite bound is ignored";
  layer.setManualRange(-1.0e300, 1.0e300);
  const auto [lo, hi] = layer.manualRangeForTest();
  EXPECT_EQ(lo, static_cast<double>(std::numeric_limits<float>::lowest())) << "clamped to what the pass stores";
  EXPECT_EQ(hi, static_cast<double>(std::numeric_limits<float>::max()));

  QDomDocument doc;
  pj::scene3d::GridMapLayer restored(rig.topic_id, u"grid"_s);
  layer.setManualRange(10.0, 1.0);
  EXPECT_TRUE(restored.xmlLoadState(layer.xmlSaveState(doc))) << "the setter's output must always load";
  layer.setManualRange(-1.0e300, 1.0e300);
  EXPECT_TRUE(restored.xmlLoadState(layer.xmlSaveState(doc)));

  QDomElement inverted = doc.createElement(u"grid_map"_s);
  inverted.setAttribute(u"range_lo"_s, 5.0);
  inverted.setAttribute(u"range_hi"_s, 1.0);
  EXPECT_FALSE(restored.xmlLoadState(inverted));
  QDomElement beyond_float = doc.createElement(u"grid_map"_s);
  beyond_float.setAttribute(u"range_lo"_s, 0.0);
  beyond_float.setAttribute(u"range_hi"_s, 1.0e300);
  EXPECT_FALSE(restored.xmlLoadState(beyond_float));
}

// Nudging one range scrubber past the other drags the sibling along (the
// PointCloudLayer panel contract) instead of pushing an inverted pair.
TEST(GridMapLayer, ConfigPanelRangeScrubbersStayOrdered) {
  Rig rig;
  rig.layer.setAutoRange(false);
  rig.layer.setManualRange(0.25, 0.75);
  std::unique_ptr<QWidget> panel(rig.layer.createConfigWidget(nullptr));
  auto* min_spin = panel->findChild<PJ::DoubleScrubber*>(u"grid_map_range_min"_s);
  auto* max_spin = panel->findChild<PJ::DoubleScrubber*>(u"grid_map_range_max"_s);
  ASSERT_NE(min_spin, nullptr);
  ASSERT_NE(max_spin, nullptr);

  min_spin->setValue(0.375);  // still below max: the sibling stays put
  EXPECT_DOUBLE_EQ(max_spin->value(), 0.75);
  EXPECT_EQ(rig.layer.manualRangeForTest(), (std::pair{0.375, 0.75}));

  min_spin->setValue(1.0);  // past max: max follows up
  EXPECT_DOUBLE_EQ(max_spin->value(), 1.0);
  EXPECT_EQ(rig.layer.manualRangeForTest(), (std::pair{1.0, 1.0}));

  max_spin->setValue(0.5);  // past min: min follows down
  EXPECT_DOUBLE_EQ(min_spin->value(), 0.5);
  EXPECT_EQ(rig.layer.manualRangeForTest(), (std::pair{0.5, 0.5}));
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
