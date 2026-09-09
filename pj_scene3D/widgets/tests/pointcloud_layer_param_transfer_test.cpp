// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Integration test for the copy/paste/apply-to-family feature: the generic
// serializeLayerParams/applyLayerParams pair (pj_scene_common) must round-trip
// EVERY user-tunable parameter between two real PointCloudLayers. This is the
// exact data path the Scene3DConfigPanel toolbar drives — copy serializes one
// layer's params, paste/apply-to-family feed that blob into other same-family
// layers. Verifying with the real layer (not a fake) also pins that
// PointCloudLayer::xmlSaveState/xmlLoadState cover the whole param set.

#include <gtest/gtest.h>

#include <QColor>
#include <QCoreApplication>
#include <QString>
#include <cstdint>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"
#include "pj_scene_common/layer_params.h"
using namespace Qt::StringLiterals;

namespace {

PJ::ObjectTopicId topic(uint32_t id) {
  PJ::ObjectTopicId topic_id;
  topic_id.id = id;
  return topic_id;
}

TEST(PointCloudLayerParamTransfer, SerializeApplyRoundTripsEveryParamBetweenRealLayers) {
  using pj::scene3d::PointCloudLayer;
  using pj::scene3d::PointcloudRenderPass;

  PointCloudLayer src(topic(1), u"A"_s, PJ::sdk::BuiltinObjectType::kPointCloud);
  PointCloudLayer dst(topic(2), u"B"_s, PJ::sdk::BuiltinObjectType::kPointCloud);

  // Push src away from EVERY default, so a no-op apply can't masquerade as a pass.
  src.setShape(PointcloudRenderPass::Shape::kPoint);          // default kSphere
  src.setSizeMeters(0.25f);                                   // default 0.02
  src.setSizePixels(7.5f);                                    // default 2; fractional must NOT truncate
  src.setColorType(PointcloudRenderPass::ColorType::kSolid);  // default kField
  src.setSolidColor(QColor(10, 20, 30));
  src.setColormap(PointcloudRenderPass::Colormap::kViridis);  // default kTurbo
  src.setInvertLut(true);                                     // default false
  src.setOutsideRangeOpacity(0.35f);                          // default 1.0
  src.setOutsideRangeVisible(false);                          // default true
  src.setAutoRange(false);                                    // default true
  src.setManualRange(-3.0f, 9.0f);

  ASSERT_NE(dst.shape(), src.shape()) << "test premise: dst still at defaults before paste";

  const QString xml = PJ::serializeLayerParams(src);
  ASSERT_FALSE(xml.isEmpty());
  ASSERT_TRUE(PJ::applyLayerParams(dst, xml));

  // Every user-tunable parameter transferred to the target layer.
  EXPECT_EQ(dst.shape(), src.shape());
  EXPECT_FLOAT_EQ(dst.sizeMeters(), src.sizeMeters());
  // Point size is fractional (the spinbox offers 0.5 steps and gl_PointSize is a
  // float); it must survive set + XML round-trip without truncating to an int.
  EXPECT_FLOAT_EQ(src.sizePixels(), 7.5f);
  EXPECT_FLOAT_EQ(dst.sizePixels(), src.sizePixels());
  EXPECT_EQ(dst.colorType(), src.colorType());
  EXPECT_EQ(dst.solidColor(), src.solidColor());
  EXPECT_EQ(dst.colormap(), src.colormap());
  EXPECT_EQ(dst.invertLut(), src.invertLut());
  EXPECT_FLOAT_EQ(dst.outsideRangeOpacity(), src.outsideRangeOpacity());
  EXPECT_EQ(dst.outsideRangeVisible(), src.outsideRangeVisible());
  EXPECT_EQ(dst.autoRange(), src.autoRange());
  EXPECT_FLOAT_EQ(dst.manualRangeMin(), src.manualRangeMin());
  EXPECT_FLOAT_EQ(dst.manualRangeMax(), src.manualRangeMax());

  // Strongest check: dst now serializes byte-identically to src — the exact blob
  // copy/paste and apply-to-family hand around.
  EXPECT_EQ(PJ::serializeLayerParams(dst), xml);
}

}  // namespace
