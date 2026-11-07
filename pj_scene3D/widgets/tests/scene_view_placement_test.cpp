// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// TF R6 wiring of the view, admitted without a display
// (SceneViewTestAccess::admitFrame()): a stamped point layer is placed at its own
// stamp in the TF header clock, with its placement hold, while the TF triads stay
// at the cursor in the entry clock. Breaking either wiring moves a placement here.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QColor>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../rhi/passes/point_rhi_pass.h"
#include "pj_base/time.hpp"
#include "pj_scene3d_core/tf/object_placement.h"
#include "pj_scene3d_widgets/point_renderable.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_tf/tf_buffer.h"
#include "scene_view_test_access.h"

namespace pj::scene3d {
namespace {

using pj::tf::StampedTransform;
using namespace std::chrono_literals;

// One point at its frame's origin, stamped `stamp`: its placed bounds are its placement.
class FakeCloud : public PointRenderable {
 public:
  FakeCloud(std::string frame, std::optional<PJ::Timepoint> stamp) : frame_(std::move(frame)), stamp_(stamp) {}

  bool prepareForRender(std::uint64_t /*remaining_view_vertices*/) override {
    return true;
  }
  [[nodiscard]] const QByteArray& vertexBytes() const override {
    return bytes_;
  }
  [[nodiscard]] PointVertexFormat vertexFormat() const override {
    return PointVertexFormat::kXyz;
  }
  [[nodiscard]] std::size_t pointCount() const override {
    return 1;
  }
  [[nodiscard]] std::uint64_t geometryRevision() const override {
    return 1;
  }
  [[nodiscard]] const std::string& renderFrameId() const override {
    return frame_;
  }
  [[nodiscard]] std::optional<PJ::Timepoint> renderStamp() const override {
    return stamp_;
  }
  [[nodiscard]] PlacementHold* placementHold() const override {
    return &hold_;
  }
  [[nodiscard]] std::optional<AABB> sourceBounds() const override {
    return AABB{.min = glm::vec3(0.0F), .max = glm::vec3(0.0F), .valid = true};
  }
  [[nodiscard]] bool visible() const override {
    return true;
  }
  [[nodiscard]] PointShape shape() const override {
    return PointShape::kPoint;
  }
  [[nodiscard]] PointColorType colorType() const override {
    return PointColorType::kSolid;
  }
  [[nodiscard]] float sizeMeters() const override {
    return 0.1F;
  }
  [[nodiscard]] float sizePixels() const override {
    return 2.0F;
  }
  [[nodiscard]] QColor solidColor() const override {
    return Qt::white;
  }
  [[nodiscard]] PJ::Colormap colormap() const override {
    return PJ::Colormap::kTurbo;
  }
  [[nodiscard]] bool invertLut() const override {
    return false;
  }
  [[nodiscard]] float outsideRangeAlpha() const override {
    return 1.0F;
  }
  [[nodiscard]] int scalarAxis() const override {
    return 0;
  }
  [[nodiscard]] std::pair<float, float> scalarRange(const glm::mat4& /*fixed_from_source*/) const override {
    return {0.0F, 1.0F};
  }
  void noteRenderedScalarRange(float /*minimum*/, float /*maximum*/) override {}
  [[nodiscard]] bool isDepthCloud() const override {
    return false;
  }

 private:
  std::string frame_;
  std::optional<PJ::Timepoint> stamp_;
  QByteArray bytes_;
  mutable PlacementHold hold_;
};

PJ::Timepoint ms(int64_t value) {
  return PJ::Timepoint{std::chrono::milliseconds(value)};
}

// map <- base along x: header `stamp`, logged at `entry`.
void feed(TransformBuffer& tf, PJ::Timepoint stamp, PJ::Timepoint entry, double x) {
  const StampedTransform edge{stamp, "map", "base", Transform{{x, 0.0, 0.0}, glm::dquat{1.0, 0.0, 0.0, 0.0}}};
  ASSERT_TRUE(tf.setTransform(edge, entry).has_value());
}

// The cloud's placed x (its point is the frame origin) after admitting a frame at `cursor`.
float admittedCloudX(SceneViewWidget& view, FakeCloud& cloud, PJ::Timepoint cursor) {
  view.setTrackerTime(cursor);
  SceneViewTestAccess::pointPass(view).setLayers({&cloud});
  SceneViewTestAccess::admitFrame(view);
  return SceneViewTestAccess::pointPass(view).lastPointBounds().min.x;
}

// The 200 ms sample of base is logged at 300 ms: at cursor 250 ms the entry clock
// still holds the 100 ms one (x = 1), the header clock at the cloud's stamp the
// 200 ms one (x = 2).
TEST(SceneViewPlacement, StampedLayerAtItsStampAndTriadsAtTheCursor) {
  auto tf = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  feed(*tf, ms(100), ms(100), 1.0);
  feed(*tf, ms(200), ms(300), 2.0);
  SceneViewWidget view;
  view.setTransformBuffer(tf);
  view.setFixedFrame("map");
  view.setAxesVisible(true);
  FakeCloud cloud("base", ms(200));

  EXPECT_FLOAT_EQ(admittedCloudX(view, cloud, ms(250)), 2.0F) << "placed at the cloud's own stamp";

  std::vector<float> triad_x;
  for (const PoseTriadInstance& arm : SceneViewTestAccess::tfTriads(view)) {
    triad_x.push_back(arm.model[3].x);
  }
  ASSERT_FALSE(triad_x.empty());
  EXPECT_NE(std::find(triad_x.begin(), triad_x.end(), 1.0F), triad_x.end()) << "base's triad at the cursor";
  EXPECT_EQ(std::find(triad_x.begin(), triad_x.end(), 2.0F), triad_x.end()) << "no triad at the header clock";
}

// A cloud in the moving fixed frame itself is placed like any other stamped object:
// captured at 100 ms (base at x = 1), seen at 250 ms from base at x = 2.
TEST(SceneViewPlacement, StampedLayerInTheMovingFixedFrameStaysWhereItWasCaptured) {
  auto tf = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  feed(*tf, ms(100), ms(100), 1.0);
  feed(*tf, ms(200), ms(200), 2.0);
  SceneViewWidget view;
  view.setTransformBuffer(tf);
  view.setFixedFrame("base");
  FakeCloud cloud("base", ms(100));

  EXPECT_FLOAT_EQ(admittedCloudX(view, cloud, ms(250)), -1.0F);
}

// Live TF (a 10 s window) runs on to 12 s and evicts the sample at the cloud's 1 s
// stamp; the view places the cloud with the layer's hold, so it stays put.
TEST(SceneViewPlacement, EvictedTfSampleKeepsTheLayerWhereItWasPlaced) {
  auto tf = std::make_shared<TransformBuffer>(std::chrono::seconds(10));
  const auto feed_until = [&tf](int64_t first_ms, int64_t last_ms) {
    for (int64_t time = first_ms; time <= last_ms; time += 500) {
      feed(*tf, ms(time), ms(time), static_cast<double>(time) / 1000.0);
    }
  };
  feed_until(0, 2000);
  SceneViewWidget view;
  view.setTransformBuffer(tf);
  view.setFixedFrame("map");
  FakeCloud cloud("base", ms(1000));

  EXPECT_FLOAT_EQ(admittedCloudX(view, cloud, ms(2000)), 1.0F);
  feed_until(2500, 12000);
  EXPECT_FLOAT_EQ(admittedCloudX(view, cloud, ms(12000)), 1.0F) << "the view placed it with the layer's hold";
}

}  // namespace
}  // namespace pj::scene3d
