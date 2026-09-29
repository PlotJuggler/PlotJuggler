// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The QRhi view's failure contract, driven by the pipeline fault injection of
// builds with tests (PJ_RHI_TEST_FAIL_SHADER, see createPipeline()):
//
//  - a base pipeline that cannot be created puts the view in its failure state:
//    no crash, only the backdrop, the reason in the dock's warning label and
//    renderFailed() once; a sibling QRhiWidget in the same window keeps
//    rendering; clearing the fault and re-initializing (a resize) recovers.
//    The pipelines are created as frames first draw with them, so the failure
//    comes with the first frames; none of them is presented with a pipeline
//    missing (each keeps the last full frame, or the backdrop);
//  - so does a family's pipeline built when its first layer arrives (the
//    families are built on demand, and of their pipelines only the variants a
//    frame binds), and a base pipeline first needed by a direct frame after HDR
//    frames;
//  - the same holds after a recovery in a new top-level (a new QRhi);
//  - Qt's own failure (no QRhi for the window) reaches the warning too;
//  - optional stages drop alone: the overlays, the HDR chain when a pipeline
//    has no HDR variant, and each quality stage whose allocation fails.
//
// Needs a real display like the render harness (see scene3d_render_test_support.h).

#include <gtest/gtest.h>
#include <rhi/qrhi.h>

#include <QFile>
#include <QLabel>
#include <QLoggingCategory>
#include <QOpenGLWidget>
#include <QRhiWidget>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <QVariant>
#include <functional>
#include <glm/glm.hpp>
#include <vector>

#include "../rhi/passes/marker_rhi_pass.h"
#include "../rhi/scene_view_widget_rhi_quality_p.h"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_widgets/GraphicsApi.h"
#include "scene3d_render_test_support.h"
#include "scene_view_test_access.h"

using namespace Qt::StringLiterals;

namespace {

using pj::scene3d::SceneViewTestAccess;
using pj::scene3d::SceneViewWidget;
using pj::scene3d::test::grab;
using pj::scene3d::test::PipelineFault;

constexpr QSize kViewSize{320, 240};

// A GPU sibling sharing the window's QRhi: clears to green, counting its frames.
class ClearWidget : public QRhiWidget {
 public:
  ClearWidget() {
    setApi(PJ::preferredGraphicsApi());
    setFixedSize(kViewSize / 2);
  }
  int frames = 0;

 protected:
  void render(QRhiCommandBuffer* command_buffer) override {
    command_buffer->beginPass(renderTarget(), QColor(0, 200, 0), {1.0F, 0});
    command_buffer->endPass();
    ++frames;
  }
};

// Nothing but one colour: the cleared backdrop.
bool uniform(const QImage& image) {
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (image.pixel(x, y) != image.pixel(0, 0)) {
        return false;
      }
    }
  }
  return true;
}

// A Scene3D dock with a point block (grid on) and a count of its view's renderFailed().
class FailureScene {
 public:
  FailureScene() {
    std::vector<glm::vec3> points;
    for (int i = 0; i < 64; ++i) {
      points.emplace_back(
          0.1F * static_cast<float>(i % 4), 0.1F * static_cast<float>((i / 4) % 4), 0.1F * static_cast<float>(i / 16));
    }
    fixture_.add("/points", PJ::sdk::BuiltinObjectType::kPointCloud, pj::scene3d::test::serializeCloud(points, "map"));
    dock()->setFixedFrame(u"map"_s);
    dock()->setFixedSize(kViewSize);
    QObject::connect(view(), &QRhiWidget::renderFailed, view(), [this] { ++failures; });
  }
  PJ::Scene3DDockWidget* dock() {
    return fixture_.dock();
  }
  SceneViewWidget* view() {
    return dock()->sceneView();
  }
  QLabel* warningLabel() {
    return dock()->findChild<QLabel*>(u"scene3dRenderingWarning"_s);
  }
  ClearWidget* addSibling() {
    auto* sibling = new ClearWidget();
    addWidget(sibling);
    return sibling;
  }
  void addWidget(QWidget* widget) {
    fixture_.window().layout()->addWidget(widget);
  }
  bool show() {
    fixture_.window().show();
    return QTest::qWaitForWindowExposed(&fixture_.window());
  }
  // A new output size: QRhiWidget runs initialize() again. Grows by `grow`,
  // since the view's minimum size is kViewSize.
  void resize(QSize grow = {40, 40}) {
    dock()->setFixedSize(kViewSize + grow);
  }
  // Renders until `done` holds (true) or 3 s pass (false).
  bool renderUntil(const std::function<bool()>& done) {
    return QTest::qWaitFor(
        [&] {
          view()->invalidateContent();
          return done();
        },
        3000);
  }

  // A local URDF robot with one box link over the point block: a mesh shadow
  // caster, whose TF buffer is what enables shadows.
  void addRobot() {
    ASSERT_TRUE(urdf_dir_.isValid());
    const QString path = urdf_dir_.filePath(u"caster.urdf"_s);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const QByteArray urdf = R"(<robot name="caster"><link name="map"><visual>
        <origin xyz="0.15 0.15 0.6"/><geometry><box size="0.3 0.3 0.3"/></geometry>
      </visual></link></robot>)";
    ASSERT_EQ(file.write(urdf), urdf.size());
    file.close();
    EXPECT_NE(dock()->addRobotModelLayer(path).id, 0U);
  }

  // A SceneEntities layer with one cube at the point block: the view's first marker layer.
  void addCube() {
    PJ::sdk::SceneEntity entity;
    entity.frame_id = "map";
    entity.id = "cube";
    PJ::sdk::CubePrimitive cube;
    cube.pose.position = {0.15, 0.15, 0.15};
    cube.size = {0.3, 0.3, 0.3};
    cube.color = {200, 40, 40, 255};
    entity.cubes.push_back(cube);
    PJ::sdk::SceneEntities entities;
    entities.entities.push_back(std::move(entity));
    fixture_.add("/entities", PJ::sdk::BuiltinObjectType::kSceneEntities, PJ::serializeSceneEntities(entities));
  }

  int failures = 0;

 private:
  pj::scene3d::test::Scene3DRenderFixture fixture_;
  QTemporaryDir urdf_dir_;
};

class Scene3dRhiFailureTest : public ::testing::Test {
 protected:
  void SetUp() override {
    PJ_SKIP_ON_HEADLESS_QPA();
  }
};

TEST_F(Scene3dRhiFailureTest, BasePipelineFailureClearsWarnsAndRecovers) {
  PipelineFault fault("/grid.vert.qsb");
  FailureScene scene;
  SceneViewWidget* view = scene.view();
  ClearWidget* sibling = scene.addSibling();
  ASSERT_TRUE(scene.show());
  // Every frame up to the failure state shows only the backdrop: none is
  // presented with the grid pipelines missing.
  QImage failed;
  for (int frame = 0; frame < 10 && scene.failures == 0; ++frame) {
    failed = grab(*view);
    if (failed.isNull() || view->rhiBackendName().isEmpty()) {
      PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
    }
    EXPECT_TRUE(uniform(failed)) << "frame " << frame << " drew more than the backdrop";
  }
  failed = grab(*view);
  EXPECT_TRUE(uniform(failed)) << "the failure state drew more than the backdrop";
  EXPECT_EQ(scene.failures, 1);
  EXPECT_GE(SceneViewTestAccess::stageTimes(*view).frames_withheld, 1U);
  const QString warning = view->renderingWarning();
  EXPECT_TRUE(warning.contains(u"grid pipelines"_s)) << warning.toStdString();
  QLabel* label = scene.warningLabel();
  ASSERT_NE(label, nullptr);
  EXPECT_TRUE(label->isVisible());
  EXPECT_EQ(label->text(), warning);

  // The sibling renders through the same QRhi while the view keeps failing.
  const int sibling_frames = sibling->frames;
  view->invalidateContent();
  sibling->update();
  ASSERT_TRUE(QTest::qWaitFor([&] { return sibling->frames > sibling_frames; }, 3000));
  const QImage green = sibling->grabFramebuffer();
  ASSERT_FALSE(green.isNull());
  const QRgb centre = green.pixel(green.width() / 2, green.height() / 2);
  EXPECT_TRUE(qGreen(centre) > 150 && qRed(centre) < 50 && qBlue(centre) < 50);

  fault.clear();
  scene.resize();
  ASSERT_TRUE(scene.renderUntil([&] { return view->renderingWarning().isEmpty(); }))
      << view->renderingWarning().toStdString();
  EXPECT_FALSE(uniform(grab(*view))) << "the recovered view draws nothing";
  EXPECT_FALSE(label->isVisible());
  EXPECT_EQ(scene.failures, 1);
}

// A family built on demand fails like a base part: the first marker layer
// arrives in a rendering view while the marker pipelines cannot be created.
TEST_F(Scene3dRhiFailureTest, FamilyBuiltOnDemandFailsAndRecovers) {
  FailureScene scene;
  SceneViewWidget* view = scene.view();
  ASSERT_TRUE(scene.show());
  const QImage points = grab(*view);
  if (points.isNull() || view->rhiBackendName().isEmpty()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }
  ASSERT_FALSE(uniform(points));
  ASSERT_EQ(scene.failures, 0);

  PipelineFault fault("/marker.frag.qsb");
  const std::uint64_t withheld = SceneViewTestAccess::stageTimes(*view).frames_withheld;
  scene.addCube();
  ASSERT_TRUE(scene.renderUntil([&] { return scene.failures > 0; })) << "the marker family did not fail";
  EXPECT_EQ(scene.failures, 1);
  EXPECT_GT(SceneViewTestAccess::stageTimes(*view).frames_withheld, withheld)
      << "a frame without the markers was presented";
  EXPECT_TRUE(uniform(grab(*view))) << "the failure state drew more than the backdrop";
  EXPECT_TRUE(view->renderingWarning().contains(u"marker pipelines"_s)) << view->renderingWarning().toStdString();
  EXPECT_TRUE(scene.warningLabel()->isVisible());

  fault.clear();
  scene.resize();
  ASSERT_TRUE(scene.renderUntil([&] { return view->renderingWarning().isEmpty(); }))
      << view->renderingWarning().toStdString();
  EXPECT_GT(SceneViewTestAccess::markerPass(*view).lastInstanceCount(), 0);
  EXPECT_FALSE(uniform(grab(*view)));
  EXPECT_EQ(scene.failures, 1);
}

// Only what the scene draws is built: the families of its layers (beside the
// base's grid and pose pass), and of their registered pipelines only the
// variants its frames bind. A GPU running the HDR chain builds no direct
// pipeline; one without it builds every direct one and no HDR variant.
TEST_F(Scene3dRhiFailureTest, FamiliesAndVariantsAreBuiltOnDemand) {
  FailureScene scene;
  SceneViewWidget* view = scene.view();
  ASSERT_TRUE(scene.show());
  if (grab(*view).isNull() || view->rhiBackendName().isEmpty()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }
  const bool hdr = SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kHdr).last_active;
  const auto expect_lazy = [&](const SceneViewTestAccess::PipelineCounts& counts, const char* what) {
    if (hdr) {
      EXPECT_EQ(counts.directs, 0) << what << ": an HDR frame built a direct pipeline";
      EXPECT_GT(counts.hdr_variants, 0) << what;
      EXPECT_LT(counts.hdr_variants, 2 * counts.registered) << what << ": every variant was built";
    } else {
      EXPECT_EQ(counts.directs, counts.registered) << what;
      EXPECT_EQ(counts.hdr_variants, 0) << what << ": a direct frame built an HDR variant";
    }
  };
  EXPECT_EQ(SceneViewTestAccess::builtFamilies(*view), QStringList{u"points"_s});
  const SceneViewTestAccess::PipelineCounts points = SceneViewTestAccess::pipelineCounts(*view);
  expect_lazy(points, "points");

  // A SceneEntities layer contributes markers and models.
  scene.addCube();
  ASSERT_TRUE(scene.renderUntil([&] { return SceneViewTestAccess::markerPass(*view).lastInstanceCount() > 0; }));
  EXPECT_EQ(SceneViewTestAccess::builtFamilies(*view), (QStringList{u"points"_s, u"markers"_s, u"models"_s}));
  const SceneViewTestAccess::PipelineCounts markers = SceneViewTestAccess::pipelineCounts(*view);
  EXPECT_GT(markers.registered, points.registered);
  expect_lazy(markers, "points and markers");
  if (hdr) {
    EXPECT_LT(markers.hdr_variants - points.hdr_variants, markers.registered - points.registered)
        << "the new families built a variant of every pipeline they registered";
    // Without the chain: every direct pipeline, and the variants gone with it.
    const auto& times = SceneViewTestAccess::stageTimes(*view);
    const std::uint64_t renders = times.full_renders;
    SceneViewTestAccess::forceStageFallback(*view, SceneViewTestAccess::Stage::kHdr, true);
    ASSERT_TRUE(scene.renderUntil([&] { return times.full_renders > renders; }));
    ASSERT_FALSE(SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kHdr).last_active);
    const SceneViewTestAccess::PipelineCounts direct = SceneViewTestAccess::pipelineCounts(*view);
    EXPECT_EQ(direct.directs, direct.registered);
    EXPECT_EQ(direct.hdr_variants, 0);
  }
}

// A base pipeline the HDR frames never needed (the grid's direct pipelines are
// created only for a frame without the HDR chain) fails on the first
// direct frame: that frame is withheld (its target keeps the last HDR frame),
// then the failure state; the fault gone, a resize recovers.
TEST_F(Scene3dRhiFailureTest, BaseTemplateFailsOnTheFirstDirectFrame) {
  FailureScene scene;
  SceneViewWidget* view = scene.view();
  ASSERT_TRUE(scene.show());
  const QImage hdr = grab(*view);
  if (hdr.isNull() || view->rhiBackendName().isEmpty()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }
  if (!SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kHdr).last_active) {
    GTEST_SKIP() << "this GPU draws without the HDR chain: the directs exist from the first frame";
  }
  ASSERT_FALSE(uniform(hdr));

  PipelineFault fault("/grid.vert.qsb");
  const auto& t = SceneViewTestAccess::stageTimes(*view);
  const std::uint64_t withheld = t.frames_withheld;
  SceneViewTestAccess::forceStageFallback(*view, SceneViewTestAccess::Stage::kHdr, true);
  ASSERT_TRUE(scene.renderUntil([&] { return t.frames_withheld > withheld; })) << "no direct frame was withheld";
  ASSERT_TRUE(scene.renderUntil([&] { return scene.failures > 0; }));
  EXPECT_EQ(scene.failures, 1);
  EXPECT_TRUE(uniform(grab(*view)));
  EXPECT_TRUE(view->renderingWarning().contains(u"grid pipelines"_s)) << view->renderingWarning().toStdString();

  fault.clear();
  SceneViewTestAccess::forceStageFallback(*view, SceneViewTestAccess::Stage::kHdr, false);
  scene.resize();
  ASSERT_TRUE(scene.renderUntil([&] { return view->renderingWarning().isEmpty(); }))
      << view->renderingWarning().toStdString();
  EXPECT_FALSE(uniform(grab(*view)));
  EXPECT_EQ(scene.failures, 1);
}

// A withheld frame never reaches the screen, so frame picking keeps projecting
// with the camera of the image still shown, not the one the withheld frame had.
TEST_F(Scene3dRhiFailureTest, WithheldFrameKeepsThePickCamera) {
  FailureScene scene;
  SceneViewWidget* view = scene.view();
  ASSERT_TRUE(scene.show());
  const QImage hdr = grab(*view);
  if (hdr.isNull() || view->rhiBackendName().isEmpty()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }
  if (!SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kHdr).last_active) {
    GTEST_SKIP() << "this GPU draws without the HDR chain: the directs exist from the first frame";
  }
  const glm::mat4 shown = SceneViewTestAccess::lastViewProjection(*view);

  PipelineFault fault("/grid.vert.qsb");
  const auto& t = SceneViewTestAccess::stageTimes(*view);
  const std::uint64_t withheld = t.frames_withheld;
  // A rotation, not a pan: the stored matrix is relative to the render origin,
  // which a pan moves along with the focal.
  view->camera().rotate(80.0F, 40.0F);
  SceneViewTestAccess::forceStageFallback(*view, SceneViewTestAccess::Stage::kHdr, true);
  ASSERT_TRUE(scene.renderUntil([&] { return t.frames_withheld > withheld; })) << "no direct frame was withheld";
  EXPECT_EQ(SceneViewTestAccess::lastViewProjection(*view), shown)
      << "picking projects with a frame that was never shown";
}

// The grid's shadow receiver is an optional stage: a GPU that cannot build its
// pipeline draws the scene without shadows instead of failing the view, and a
// new output size probes it again.
TEST_F(Scene3dRhiFailureTest, ShadowReceiverFailureTurnsShadowsOff) {
  PipelineFault fault("/grid_cell_shadow.frag.qsb");
  FailureScene scene;
  scene.addRobot();  // the shadow caster
  SceneViewWidget* view = scene.view();
  view->setGridStyle(SceneViewWidget::GridStyle::kFilledCells);  // the cells receive the shadow
  ASSERT_TRUE(scene.show());
  if (grab(*view).isNull() || view->rhiBackendName().isEmpty()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }
  ASSERT_TRUE(scene.renderUntil([&] {
    return scene.failures > 0 || SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kShadow)
                                     .capability_error.contains(u"shadow receiver"_s);
  })) << view->renderingWarning().toStdString();
  for (int frame = 0; frame < 3; ++frame) {
    (void)grab(*view);  // the frames after the fallback, direct and HDR alike
  }
  EXPECT_EQ(scene.failures, 0) << view->renderingWarning().toStdString();
  EXPECT_FALSE(uniform(grab(*view))) << "the scene went down with its optional shadow receiver";
  EXPECT_FALSE(SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kShadow).last_active);
  EXPECT_TRUE(
      SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kShadow)
          .capability_error.contains(u"shadow receiver"_s))
      << SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kShadow).capability_error.toStdString();

  fault.clear();
  scene.resize();
  EXPECT_TRUE(scene.renderUntil([&] {
    return SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kShadow).last_active;
  })) << SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kShadow).capability_error.toStdString();
  EXPECT_EQ(scene.failures, 0);
}

// An optional composite stage that failed on one QRhi is retried on the next
// one, even at the same output size: floating or re-docking the view gives it a
// new top-level and a new QRhi, and the failure belonged to the old device. The
// fault is removed without touching the stage, so only the new QRhi can retry it.
void checkStageRetriesOnANewQRhi(
    const char* shader, const std::function<bool(const SceneViewWidget&)>& active,
    const std::function<QString(const SceneViewWidget&)>& error) {
  PipelineFault fault(shader);
  FailureScene scene;
  scene.addRobot();  // EDL's mesh mask exists only with a model family
  SceneViewWidget* view = scene.view();
  ASSERT_TRUE(scene.show());
  if (grab(*view).isNull() || view->rhiBackendName().isEmpty()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }
  if (!SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kHdr).last_active) {
    GTEST_SKIP() << "this GPU draws without the HDR chain, which SSAO and EDL need";
  }
  ASSERT_TRUE(scene.renderUntil([&] { return !error(*view).isEmpty(); })) << "the fault never reached " << shader;
  EXPECT_FALSE(active(*view));
  EXPECT_EQ(scene.failures, 0);

  fault.clear();
  const QSize size = view->size();
  QWidget other;
  auto* layout = new QVBoxLayout(&other);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(scene.dock());
  other.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&other));
  ASSERT_EQ(view->size(), size) << "the new window changed the output size";
  EXPECT_TRUE(scene.renderUntil([&] { return active(*view); })) << error(*view).toStdString();
  EXPECT_TRUE(error(*view).isEmpty()) << error(*view).toStdString();
  EXPECT_EQ(scene.failures, 0);
}

TEST_F(Scene3dRhiFailureTest, SsaoRetriesOnANewQRhiAtTheSameSize) {
  checkStageRetriesOnANewQRhi(
      "/ssao.frag.qsb",
      [](const SceneViewWidget& view) {
        return SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kSsao).last_active;
      },
      [](const SceneViewWidget& view) {
        return SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kSsao).capability_error;
      });
}

TEST_F(Scene3dRhiFailureTest, EdlRetriesOnANewQRhiAtTheSameSize) {
  checkStageRetriesOnANewQRhi(
      "/mesh_mask.frag.qsb",
      [](const SceneViewWidget& view) {
        return SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kEdl).last_active;
      },
      [](const SceneViewWidget& view) {
        return SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kEdl).capability_error;
      });
}

// An allocation that fails part way through a quality stage (pjTestFailPipeline
// names it; test builds) drops that stage alone: its partial resources are
// released, the scene still draws without it, and a new output size probes it
// again.
void checkStageAllocationFailure(
    SceneViewTestAccess::Stage stage, const char* resource, const std::function<bool(const SceneViewWidget&)>& active,
    const std::function<QString(const SceneViewWidget&)>& error) {
  FailureScene scene;
  scene.addRobot();  // a shadow caster, and a mesh for EDL's mask
  SceneViewWidget* view = scene.view();
  ASSERT_TRUE(scene.show());
  if (grab(*view).isNull() || view->rhiBackendName().isEmpty()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }
  if (!SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kHdr).last_active) {
    GTEST_SKIP() << "this GPU draws without the HDR chain, which SSAO and EDL need";
  }
  ASSERT_TRUE(scene.renderUntil([&] { return active(*view); })) << resource << " never ran";

  view->setProperty("pjTestFailPipeline", QString::fromLatin1(resource));
  // Released and let go at once: the next frame allocates the stage again.
  SceneViewTestAccess::forceStageFallback(*view, stage, true);
  SceneViewTestAccess::forceStageFallback(*view, stage, false);
  ASSERT_TRUE(scene.renderUntil([&] { return !error(*view).isEmpty(); })) << "the fault never reached " << resource;
  EXPECT_FALSE(active(*view));
  EXPECT_EQ(scene.failures, 0) << view->renderingWarning().toStdString();
  EXPECT_FALSE(uniform(grab(*view))) << "the scene went down with its optional " << resource;

  view->setProperty("pjTestFailPipeline", QVariant());
  scene.resize({80, 80});
  EXPECT_TRUE(scene.renderUntil([&] { return active(*view); })) << error(*view).toStdString();
  EXPECT_TRUE(error(*view).isEmpty()) << error(*view).toStdString();
  EXPECT_EQ(scene.failures, 0);
}

TEST_F(Scene3dRhiFailureTest, ShadowAllocationFailureDropsShadowsAlone) {
  checkStageAllocationFailure(
      SceneViewTestAccess::Stage::kShadow, "shadow target",
      [](const SceneViewWidget& view) {
        return SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kShadow).last_active;
      },
      [](const SceneViewWidget& view) {
        return SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kShadow).capability_error;
      });
}

TEST_F(Scene3dRhiFailureTest, HdrAllocationFailureDrawsDirectly) {
  checkStageAllocationFailure(
      SceneViewTestAccess::Stage::kHdr, "hdr attachments",
      [](const SceneViewWidget& view) {
        return SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kHdr).last_active;
      },
      [](const SceneViewWidget& view) {
        return SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kHdr).capability_error;
      });
}

TEST_F(Scene3dRhiFailureTest, SsaoAllocationFailureDropsSsaoAlone) {
  checkStageAllocationFailure(
      SceneViewTestAccess::Stage::kSsao, "ssao target",
      [](const SceneViewWidget& view) {
        return SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kSsao).last_active;
      },
      [](const SceneViewWidget& view) {
        return SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kSsao).capability_error;
      });
}

TEST_F(Scene3dRhiFailureTest, EdlAllocationFailureDropsEdlAlone) {
  checkStageAllocationFailure(
      SceneViewTestAccess::Stage::kEdl, "edl target",
      [](const SceneViewWidget& view) {
        return SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kEdl).last_active;
      },
      [](const SceneViewWidget& view) {
        return SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kEdl).capability_error;
      });
}

// Backend lines ("<widget>: <backend>, QRhiDriverInfo(...)", category
// pj.graphics) logged while alive.
class BackendLogCounter {
 public:
  BackendLogCounter() {
    QLoggingCategory::setFilterRules(u"pj.graphics.info=true"_s);
    previous_ = qInstallMessageHandler(&BackendLogCounter::handle);
  }
  ~BackendLogCounter() {
    qInstallMessageHandler(previous_);
    QLoggingCategory::setFilterRules({});
  }
  BackendLogCounter(const BackendLogCounter&) = delete;
  BackendLogCounter& operator=(const BackendLogCounter&) = delete;
  static inline int lines = 0;

 private:
  static void handle(QtMsgType, const QMessageLogContext& context, const QString& message) {
    if (qstrcmp(context.category, "pj.graphics") == 0 && message.contains(u"QRhiDriverInfo("_s)) {
      ++lines;
    }
  }
  QtMessageHandler previous_ = nullptr;
};

// Every retry of a failed view (a resize: a new render target) re-initializes
// it, but the backend is logged once per QRhi.
TEST_F(Scene3dRhiFailureTest, RetriesLogTheBackendOncePerQRhi) {
  BackendLogCounter log;
  PipelineFault fault("/grid.vert.qsb");
  FailureScene scene;
  SceneViewWidget* view = scene.view();
  ASSERT_TRUE(scene.show());
  if (grab(*view).isNull() || view->rhiBackendName().isEmpty()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }
  ASSERT_TRUE(scene.renderUntil([&] { return scene.failures > 0; }));
  ASSERT_EQ(scene.failures, 1);
  for (const int grow : {40, 80, 120}) {
    scene.resize({grow, grow});
    for (int frame = 0; frame < 4; ++frame) {  // the retry fails again on its first frames
      ASSERT_FALSE(grab(*view).isNull());
    }
  }
  EXPECT_EQ(scene.failures, 1);
  EXPECT_EQ(BackendLogCounter::lines, 1);
}

// After a recovery the fault returns and the view moves to another top-level
// (a new QRhi): it fails there once more, and recovers there once the fault is gone.
TEST_F(Scene3dRhiFailureTest, FailureAfterRecoveryRecoversInANewWindow) {
  PipelineFault fault("/grid.vert.qsb");
  FailureScene scene;
  SceneViewWidget* view = scene.view();
  ASSERT_TRUE(scene.show());
  if (grab(*view).isNull() || view->rhiBackendName().isEmpty()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }
  ASSERT_TRUE(scene.renderUntil([&] { return scene.failures > 0; }));
  ASSERT_EQ(scene.failures, 1);
  fault.clear();
  scene.resize();
  ASSERT_TRUE(scene.renderUntil([&] { return view->renderingWarning().isEmpty(); }))
      << view->renderingWarning().toStdString();

  PipelineFault again("/grid.vert.qsb");
  QWidget other;
  auto* layout = new QVBoxLayout(&other);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(scene.dock());
  other.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&other));
  ASSERT_TRUE(scene.renderUntil([&] { return scene.failures == 2; })) << "no failure in the new window";
  EXPECT_TRUE(uniform(grab(*view))) << "the failure state drew more than the backdrop";
  EXPECT_TRUE(view->renderingWarning().contains(u"grid pipelines"_s)) << view->renderingWarning().toStdString();
  EXPECT_TRUE(scene.warningLabel()->isVisible());

  again.clear();
  scene.resize({80, 80});
  ASSERT_TRUE(scene.renderUntil([&] { return view->renderingWarning().isEmpty(); }))
      << view->renderingWarning().toStdString();
  EXPECT_FALSE(uniform(grab(*view))) << "the recovered view draws nothing";
  EXPECT_FALSE(scene.warningLabel()->isVisible());
  EXPECT_EQ(scene.failures, 2);
}

// A window an OpenGL widget pinned to OpenGL gives a view asking for another
// API no QRhi: QRhiWidget's own renderFailed() still sets the dock's warning.
TEST_F(Scene3dRhiFailureTest, NoQRhiShowsTheWarning) {
  FailureScene scene;
  SceneViewWidget* view = scene.view();
  view->setApi(QRhiWidget::Api::Null);
  // A window already composited through an OpenGL widget; the view joins it.
  QWidget gl_window;
  auto* layout = new QVBoxLayout(&gl_window);
  auto* gl_widget = new QOpenGLWidget();
  gl_widget->setFixedSize(kViewSize / 2);
  layout->addWidget(gl_widget);
  gl_window.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&gl_window));
  layout->addWidget(scene.dock());
  ASSERT_TRUE(scene.renderUntil([&] { return scene.failures > 0; })) << "Qt reported no failure";
  EXPECT_TRUE(view->renderingWarning().contains(u"no graphics device"_s)) << view->renderingWarning().toStdString();
  ASSERT_NE(scene.warningLabel(), nullptr);
  EXPECT_TRUE(scene.warningLabel()->isVisible());
}

TEST_F(Scene3dRhiFailureTest, OverlayFailureKeepsTheScene) {
  PipelineFault fault("/overlay.frag.qsb");
  FailureScene scene;
  SceneViewWidget* view = scene.view();
  view->setShowPerfHud(true);
  ASSERT_TRUE(scene.show());
  const QImage image = grab(*view);
  if (image.isNull()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }

  EXPECT_FALSE(uniform(image)) << "the scene must still draw";
  EXPECT_EQ(scene.failures, 0);
  EXPECT_TRUE(view->renderingWarning().contains(u"overlays"_s)) << view->renderingWarning().toStdString();
  EXPECT_TRUE(SceneViewTestAccess::lastOverlayRects(*view)[0].isNull()) << "the HUD drew without its pipeline";

  fault.clear();
  scene.resize();
  ASSERT_TRUE(scene.renderUntil([&] { return !SceneViewTestAccess::lastOverlayRects(*view)[0].isNull(); }));
  EXPECT_TRUE(view->renderingWarning().isEmpty()) << view->renderingWarning().toStdString();
}

TEST_F(Scene3dRhiFailureTest, MissingHdrPipelineVariantFallsBackToDirect) {
  FailureScene scene;
  SceneViewWidget* view = scene.view();
  ASSERT_TRUE(scene.show());
  SceneViewTestAccess::forceStageFallback(
      *view, SceneViewTestAccess::Stage::kHdr, true);  // only the direct pipelines exist
  if (grab(*view).isNull()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }
  ASSERT_FALSE(SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kHdr).last_active);

  // From now on the grid's HDR variants cannot be created; its direct pipelines,
  // created by the direct frames above, stand. The HDR frame that lost its grid
  // variant is not presented (the last frame stays); the next one draws directly.
  PipelineFault fault("/scene.frag.qsb");
  const std::uint64_t withheld = SceneViewTestAccess::stageTimes(*view).frames_withheld;
  SceneViewTestAccess::forceStageFallback(*view, SceneViewTestAccess::Stage::kHdr, false);
  ASSERT_TRUE(scene.renderUntil([&] { return view->renderingWarning().contains(u"HDR variant"_s); }))
      << view->renderingWarning().toStdString();
  EXPECT_GT(SceneViewTestAccess::stageTimes(*view).frames_withheld, withheld)
      << "the HDR frame without its grid was presented";
  const QImage with_grid = grab(*view);
  EXPECT_FALSE(SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kHdr).last_active);
  EXPECT_EQ(scene.failures, 0);
  view->setGridVisible(false);
  const QImage without_grid = grab(*view);
  view->setGridVisible(true);
  EXPECT_TRUE(with_grid != without_grid) << "the direct fallback lost the grid";

  fault.clear();
  scene.resize();  // the HDR rejection is latched per output size
  ASSERT_TRUE(scene.renderUntil(
      [&] { return SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kHdr).last_active; }));
  EXPECT_TRUE(scene.renderUntil([&] { return view->renderingWarning().isEmpty(); }))
      << view->renderingWarning().toStdString();
}

}  // namespace
