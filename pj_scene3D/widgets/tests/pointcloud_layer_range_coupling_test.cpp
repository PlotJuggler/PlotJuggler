// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// PointCloudLayer config panel: the manual colormap "Range Min" / "Range Max"
// scrubbers must stay ordered (min <= max). Nudging one past the other drags
// the other along instead of producing an inverted range: raise min above max
// and max follows up; drop max below min and min follows down. Exercises the
// real DoubleScrubber widgets the config panel builds, not a stand-in.

#include <gtest/gtest.h>

#include <QApplication>
#include <QWidget>
#include <cstdint>
#include <memory>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"
#include "pj_widgets/DoubleScrubber.h"
using namespace Qt::StringLiterals;

namespace {

using pj::scene3d::PointCloudLayer;

PJ::ObjectTopicId topic(uint32_t id) {
  PJ::ObjectTopicId topic_id;
  topic_id.id = id;
  return topic_id;
}

// Builds a layer in manual-range mode and returns its config widget plus the two
// range scrubbers, found by objectName.
struct RangePanel {
  std::unique_ptr<QWidget> widget;
  PJ::DoubleScrubber* min_spin = nullptr;
  PJ::DoubleScrubber* max_spin = nullptr;
};

RangePanel makePanel(PointCloudLayer& layer, float lo, float hi) {
  layer.setAutoRange(false);
  layer.setManualRange(lo, hi);
  RangePanel p;
  p.widget.reset(layer.createConfigWidget(nullptr));
  p.min_spin = p.widget->findChild<PJ::DoubleScrubber*>(u"pointcloud_range_min"_s);
  p.max_spin = p.widget->findChild<PJ::DoubleScrubber*>(u"pointcloud_range_max"_s);
  return p;
}

TEST(PointCloudLayerRangeCoupling, RaisingMinAboveMaxDragsMaxUp) {
  PointCloudLayer layer(topic(1), u"A"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  RangePanel p = makePanel(layer, 0.5f, 0.5f);
  ASSERT_NE(p.min_spin, nullptr);
  ASSERT_NE(p.max_spin, nullptr);

  p.min_spin->setValue(0.6);

  EXPECT_DOUBLE_EQ(p.max_spin->value(), 0.6) << "max scrubber must follow min up";
  EXPECT_FLOAT_EQ(layer.manualRangeMin(), 0.6f);
  EXPECT_FLOAT_EQ(layer.manualRangeMax(), 0.6f);
}

TEST(PointCloudLayerRangeCoupling, LoweringMaxBelowMinDragsMinDown) {
  PointCloudLayer layer(topic(2), u"B"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  RangePanel p = makePanel(layer, 0.5f, 0.5f);
  ASSERT_NE(p.min_spin, nullptr);
  ASSERT_NE(p.max_spin, nullptr);

  p.max_spin->setValue(0.4);

  EXPECT_DOUBLE_EQ(p.min_spin->value(), 0.4) << "min scrubber must follow max down";
  EXPECT_FLOAT_EQ(layer.manualRangeMin(), 0.4f);
  EXPECT_FLOAT_EQ(layer.manualRangeMax(), 0.4f);
}

TEST(PointCloudLayerRangeCoupling, NonCrossingEditsLeaveSiblingUntouched) {
  // Exactly-representable binary fractions (0.25, 0.75, 0.375, 0.625) so the
  // seeded float values survive the float->double widening without rounding
  // noise — the assertions check that the untouched sibling keeps its value.
  PointCloudLayer layer(topic(3), u"C"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  RangePanel p = makePanel(layer, 0.25f, 0.75f);
  ASSERT_NE(p.min_spin, nullptr);
  ASSERT_NE(p.max_spin, nullptr);

  p.min_spin->setValue(0.375);  // still below max 0.75 -> max stays put
  EXPECT_DOUBLE_EQ(p.max_spin->value(), 0.75);

  p.max_spin->setValue(0.625);  // still above min 0.375 -> min stays put
  EXPECT_DOUBLE_EQ(p.min_spin->value(), 0.375);

  EXPECT_FLOAT_EQ(layer.manualRangeMin(), 0.375f);
  EXPECT_FLOAT_EQ(layer.manualRangeMax(), 0.625f);
}

}  // namespace
