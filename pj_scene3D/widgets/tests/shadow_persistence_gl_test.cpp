// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Real-GL regression test for the "shadow outlives its mesh" bug: a mesh casts a
// shadow on the solid grid floor, the user deletes the data, the mesh vanishes —
// but the shadow lingered.
//
// Mechanism: SceneViewWidget gates the COLOR pass on `tf_` (no TransformBuffer ⇒
// no layer draws its mesh) but the shadow PRE-pass was not gated the same way.
// Deleting the data clears the dock's TF binding (view->setTransformBuffer(nullptr)),
// yet a surviving caster layer (e.g. a local robot the prune keeps) still holds its
// last draw cache — so the ungated pre-pass kept depth-drawing it and the floor kept
// sampling a valid shadow map.
//
// This test stands in for that caster with a minimal pure-shadow-caster layer that
// reports valid caster bounds independent of TF (exactly what a stale draw cache
// does). It asserts the shadow pre-pass binds a shadow map while a TransformBuffer
// is present and binds NONE once it is removed, observed through the
// lastShadowMapIdForTest() seam (robust under software GL — no shadow-pixel
// thresholds through the tonemap).
//
// Skips when there is no usable GL or the live context is below 4.5 (the scene's
// #version 450 shaders won't compile), exactly like tf_connections_gl_test. CI runs
// it under xvfb-run with software GL (llvmpipe reports 4.5).

#include <gtest/gtest.h>

#include <QApplication>
#include <QGuiApplication>
#include <QString>
#include <QSurfaceFormat>
#include <QVBoxLayout>
#include <QWidget>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gl_scene_test_support.h"  // haveGl, liveGlVersion
#include "pj_base/time.hpp"
#include "pj_gpu_test_gate.h"  // PJ_SKIP_WITHOUT_GPU
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/passes/mesh_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace {

using pj::scene3d::AABB;
using pj::scene3d::FrameContext;
using pj::scene3d::MeshRenderPass;
using pj::scene3d::Scene3DLayer;
using pj::scene3d::SceneViewWidget;
using pj::scene3d::ViewParams;
using pj::scene3d::test::haveGl;
using pj::scene3d::test::liveGlVersion;

// A pure shadow caster: a single box held above the floor that always reports
// valid caster bounds and depth-draws itself, regardless of the TF buffer. That is
// exactly the state a real mesh layer (RobotModelLayer / SceneEntitiesLayer) is in
// after the data is deleted — its last draw cache survives because nothing dirtied
// it — so this isolates the SceneViewWidget gate asymmetry under test. render() is a
// deliberate no-op: the mesh's own pixels are irrelevant; only its cast shadow is.
class BoxShadowCaster final : public Scene3DLayer {
 public:
  BoxShadowCaster() {
    box_.kind = MeshRenderPass::GeometryKind::kBox;
    // 1 m cube centered 1.5 m above the z=0 floor — a non-degenerate caster the
    // light frustum can fit (fitDirectionalShadowCamera needs a non-zero radius).
    box_.model = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 1.5f)), glm::vec3(1.0f));
  }

  // --- Scene3DLayer GL + shadow hooks --------------------------------------
  void initializeGL() override {
    mesh_pass_.initializeGL();
  }
  void render(const ViewParams& /*view_params*/, const FrameContext& /*frame_ctx*/) override {}
  void releaseGL() override {
    mesh_pass_.releaseGL();
  }
  std::optional<AABB> meshShadowBounds(const FrameContext& /*frame_ctx*/) override {
    return mesh_pass_.worldBoundsOfDraws({box_});
  }
  void renderShadowCasters(const glm::mat4& light_view_proj, const FrameContext& /*frame_ctx*/) override {
    // The pre-pass runs before the color loop's initializeGL on the first paint, so
    // self-init here (idempotent) rather than relying on render() having run.
    mesh_pass_.initializeGL();
    mesh_pass_.renderDepthOnly(light_view_proj, {box_});
  }
  [[nodiscard]] QStringList fallbackFrames() const override {
    return {};
  }
  [[nodiscard]] QString sourceFrame() const override {
    return {};
  }

  // --- ISceneLayer boilerplate ---------------------------------------------
  [[nodiscard]] PJ::SceneLayerInfo info() const override {
    PJ::SceneLayerInfo info;
    info.topic_id = PJ::ObjectTopicId{1};
    info.display_name = u"box_caster"_s;
    info.visible = true;
    return info;
  }
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override {
    return PJ::liveTopicTimeRange(nullptr, PJ::ObjectTopicId{1});  // inverted-empty: positioned by TF, not data
  }
  bool attach(const PJ::SceneLayerContext& /*ctx*/) override {
    return true;
  }
  void detach() override {}
  void setTrackerTime(PJ::Timepoint /*time*/) override {}
  void setVisible(bool /*visible*/) override {}
  QWidget* createConfigWidget(QWidget* /*parent*/) override {
    return nullptr;
  }

 private:
  MeshRenderPass mesh_pass_;
  MeshRenderPass::DrawCall box_;
};

class ShadowPersistenceGlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal")) {
      PJ_SKIP_WITHOUT_GPU("platform '" << platform.toStdString() << "' has no real-GL QOpenGLWidget support");
    }
    host_ = std::make_unique<QWidget>();
    auto* layout = new QVBoxLayout(host_.get());
    layout->setContentsMargins(
        PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
        PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
    view_ = new SceneViewWidget();
    layout->addWidget(view_);
    host_->resize(480, 360);
    host_->show();
    QApplication::processEvents();
    if (!haveGl(*view_)) {
      PJ_SKIP_WITHOUT_GPU("No usable OpenGL context (headless without xvfb/GL).");
    }
    if (liveGlVersion(*view_) < std::pair<int, int>(4, 5)) {
      PJ_SKIP_WITHOUT_GPU("GL context below 4.5 cannot compile the scene's #version 450 shaders");
    }

    // A TransformBuffer with one frame so the view holds a non-null tf_ and a
    // resolvable fixed frame (the caster ignores it; the gate under test does not).
    tf_ = std::make_shared<pj::scene3d::TransformBuffer>(pj::scene3d::TransformBuffer::kKeepAll);
    ASSERT_TRUE(tf_->setTransform(
                       pj::scene3d::StampedTransform{
                           .stamp = PJ::fromRaw(0),
                           .parent_frame = "map",
                           .child_frame = "body",
                           .transform = pj::scene3d::Transform(glm::dvec3(0.0), glm::dquat{1.0, 0.0, 0.0, 0.0}),
                       })
                    .has_value());
    view_->setTransformBuffer(tf_);
    view_->setFixedFrame("map");
    view_->setTrackerTime(PJ::fromRaw(1'000'000));
    view_->meshShadingParams().shadows_enabled = true;
    view_->setGridStyle(pj::scene3d::GridRenderPass::Style::kFilledCells);  // the solid floor is the receiver

    caster_ = std::make_unique<BoxShadowCaster>();
    view_->setLayers({caster_.get()});
  }

  void TearDown() override {
    if (view_ != nullptr) {
      view_->setLayers({});  // drop the dangling caster before it is destroyed
    }
    host_.reset();
  }

  std::unique_ptr<QWidget> host_;
  std::shared_ptr<pj::scene3d::TransformBuffer> tf_;
  std::unique_ptr<BoxShadowCaster> caster_;
  SceneViewWidget* view_ = nullptr;
};

TEST_F(ShadowPersistenceGlTest, FloorShadowClearsWhenDataRemoved) {
  // Guard: with a TransformBuffer present, the caster casts — the shadow pre-pass
  // fits a frustum and binds a non-zero shadow map. Without this the test is vacuous.
  view_->grabFramebuffer();
  ASSERT_NE(view_->lastShadowMapIdForTest(), 0U)
      << "caster did not cast with a TransformBuffer present; test would be vacuous";

  // Delete the data: Scene3DDockWidget clears the view's TF binding this way. The
  // caster layer stays in the view holding its stale draw cache, but with no
  // TransformBuffer no layer may legitimately pose geometry — so nothing should cast.
  view_->setTransformBuffer(nullptr);
  view_->grabFramebuffer();

  EXPECT_EQ(view_->lastShadowMapIdForTest(), 0U)
      << "floor shadow persisted after the data (TransformBuffer) was removed";
}

}  // namespace

int main(int argc, char** argv) {
  // Request a 4.5 core context so QOpenGLWidget negotiates the format
  // SceneViewWidget expects (harmless under software GL).
  QSurfaceFormat fmt;
  fmt.setVersion(4, 5);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  QSurfaceFormat::setDefaultFormat(fmt);

  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
