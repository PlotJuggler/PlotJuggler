// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/camera/camera_fit_policy.h"

#include <gtest/gtest.h>

namespace {
using pj::scene3d::AABB;
using pj::scene3d::CameraFitPolicy;

const int kA = 0;
const int kB = 1;
const CameraFitPolicy::LayerKey a = &kA;
const CameraFitPolicy::LayerKey b = &kB;

AABB unitBox() {
  AABB box;
  box.min = {0.0F, 0.0F, 0.0F};
  box.max = {1.0F, 1.0F, 1.0F};
  box.valid = true;
  return box;
}

TEST(CameraFitPolicy, FitsOncePerNewLayerOnItsFirstBounds) {
  CameraFitPolicy policy;
  policy.setLayers({a});
  EXPECT_FALSE(policy.takeFit({}, unitBox())) << "no contributor yet";
  EXPECT_FALSE(policy.takeFit({a}, AABB{})) << "invalid bounds never fit";
  EXPECT_TRUE(policy.takeFit({a}, unitBox()));
  EXPECT_FALSE(policy.takeFit({a}, unitBox())) << "already fitted";
  policy.setLayers({a, b});
  EXPECT_TRUE(policy.takeFit({a, b}, unitBox())) << "a new layer fits again";
  policy.setLayers({b});
  policy.setLayers({a, b});
  EXPECT_TRUE(policy.takeFit({a}, unitBox())) << "a re-added layer is new";
}

TEST(CameraFitPolicy, RestoredCameraIsNeverRefit) {
  CameraFitPolicy policy;
  policy.setLayers({});
  policy.setLayers({a});
  policy.noteCameraRestored();
  policy.setLayers({a, b});  // a deferred restore adds a layer later
  EXPECT_FALSE(policy.hasPending());
  EXPECT_FALSE(policy.takeFit({a, b}, unitBox()));
  policy.setLayers({});  // the layout is cleared: fits resume
  policy.setLayers({a});
  EXPECT_TRUE(policy.takeFit({a}, unitBox()));
}

}  // namespace
