// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/scene_frame.h"

#include <gtest/gtest.h>

namespace PJ {
namespace {

TEST(SceneFrameTest, DefaultConstructionIsEmpty) {
  SceneFrame sf;
  EXPECT_EQ(sf.timestamp, 0);
  EXPECT_TRUE(sf.empty());
}

TEST(SceneFrameTest, ImageAnnotationEmptyByDefault) {
  ImageAnnotation a;
  EXPECT_TRUE(a.empty());
  a.points.emplace_back();
  EXPECT_FALSE(a.empty());
}

TEST(SceneFrameTest, EqualityIsValueBased) {
  PointsAnnotation lhs;
  lhs.topology = AnnotationTopology::kLineLoop;
  lhs.points = {{1.0, 2.0}, {3.0, 4.0}};
  lhs.color = {255, 0, 0, 255};

  PointsAnnotation rhs = lhs;
  EXPECT_EQ(lhs, rhs);

  rhs.points[0].x = 99.0;
  EXPECT_NE(lhs, rhs);
}

TEST(SceneFrameTest, ColorDefaultIsOpaqueBlack) {
  ColorRGBA c;
  EXPECT_EQ(c.r, 0);
  EXPECT_EQ(c.g, 0);
  EXPECT_EQ(c.b, 0);
  EXPECT_EQ(c.a, 255);
}

TEST(SceneFrameTest, FillColorDefaultIsTransparent) {
  PointsAnnotation a;
  EXPECT_EQ(a.fill_color.a, 0);  // a=0 means no fill — disabled by default
}

TEST(SceneFrameTest, NestedSceneFrameEquality) {
  SceneFrame a;
  a.timestamp = 1234;
  ImageAnnotation ia;
  ia.timestamp_ns = 1234;
  ia.image_topic = "/camera/image";
  ia.points.emplace_back();
  a.annotations.push_back(ia);

  SceneFrame b = a;
  EXPECT_EQ(a, b);
  EXPECT_FALSE(a.empty());

  b.annotations[0].image_topic = "/other";
  EXPECT_NE(a, b);
}

}  // namespace
}  // namespace PJ
