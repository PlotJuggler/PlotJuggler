// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The QRhi view's failure contract, driven by the pipeline fault injection of
// builds with tests (PJ_SCENE3D_TEST_FAIL_SHADER, see createGraphicsPipeline()):
//
//  - a base pipeline that cannot be created puts the view in its failure state:
//    no crash, only the backdrop, the reason in the dock's warning label and
//    renderFailed() once; a sibling QRhiWidget in the same window keeps
//    rendering; clearing the fault and re-initializing (a resize) recovers.
//    The pipelines are created as frames first draw with them, so the failure
//    comes with the first frames; none of them is presented with a pipeline
//    missing (each keeps the last full frame, or the backdrop);
//  - so does a family's pipeline built when its first layer arrives (the
//    families are built on demand), and a base pipeline first needed by a
//    direct frame after HDR frames;
//  - the same holds after a recovery in a new top-level (a new QRhi);
//  - Qt's own failure (no QRhi for the window) reaches the warning too;
//  - optional stages drop alone: the overlays, and the HDR chain when a
//    pipeline has no HDR variant.
//
// Needs a real display like the render harness (see scene3d_render_test_support.h).

#include <gtest/gtest.h>
#include <rhi/qrhi.h>

#include <QFile>
#include <QLabel>
#include <QLoggingCategory>
#include <QOpenGLWidget>
#include <QRhiWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <functional>
#include <glm/glm.hpp>
#include <vector>

#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_widgets/GraphicsApi.h"
#include "scene3d_render_test_support.h"

using namespace Qt::StringLiterals;

namespace {

using pj::scene3d::SceneViewWidget;
using pj::scene3d::test::grab;

constexpr QSize kViewSize{320, 240};
constexpr char kFailShader[] = "PJ_SCENE3D_TEST_FAIL_SHADER";

// Arms the fault injection for `shader` (a shader path suffix) until clear().
class PipelineFault {
 public:
  explicit PipelineFault(const char* shader) {
    qputenv(kFailShader, shader);
  }
  ~PipelineFault() {
    clear();
  }
  PipelineFault(const PipelineFault&) = delete;
  PipelineFault& operator=(const PipelineFault&) = delete;
  void clear() {
    qunsetenv(kFailShader);
  }
};

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
          view()->update();
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
  EXPECT_GE(view->stageTimesForTest().frames_withheld, 1U);
  const QString warning = view->renderingWarningForTest();
  EXPECT_TRUE(warning.contains(u"grid pipelines"_s)) << warning.toStdString();
  QLabel* label = scene.warningLabel();
  ASSERT_NE(label, nullptr);
  EXPECT_TRUE(label->isVisible());
  EXPECT_EQ(label->text(), warning);

  // The sibling renders through the same QRhi while the view keeps failing.
  const int sibling_frames = sibling->frames;
  view->update();
  sibling->update();
  ASSERT_TRUE(QTest::qWaitFor([&] { return sibling->frames > sibling_frames; }, 3000));
  const QImage green = sibling->grabFramebuffer();
  ASSERT_FALSE(green.isNull());
  const QRgb centre = green.pixel(green.width() / 2, green.height() / 2);
  EXPECT_TRUE(qGreen(centre) > 150 && qRed(centre) < 50 && qBlue(centre) < 50);

  fault.clear();
  scene.resize();
  ASSERT_TRUE(scene.renderUntil([&] { return view->renderingWarningForTest().isEmpty(); }))
      << view->renderingWarningForTest().toStdString();
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
  const std::uint64_t withheld = view->stageTimesForTest().frames_withheld;
  scene.addCube();
  ASSERT_TRUE(scene.renderUntil([&] { return scene.failures > 0; })) << "the marker family did not fail";
  EXPECT_EQ(scene.failures, 1);
  EXPECT_GT(view->stageTimesForTest().frames_withheld, withheld) << "a frame without the markers was presented";
  EXPECT_TRUE(uniform(grab(*view))) << "the failure state drew more than the backdrop";
  EXPECT_TRUE(view->renderingWarningForTest().contains(u"marker pipelines"_s))
      << view->renderingWarningForTest().toStdString();
  EXPECT_TRUE(scene.warningLabel()->isVisible());

  fault.clear();
  scene.resize();
  ASSERT_TRUE(scene.renderUntil([&] { return view->renderingWarningForTest().isEmpty(); }))
      << view->renderingWarningForTest().toStdString();
  EXPECT_GT(view->lastMarkerInstanceCountForTest(), 0);
  EXPECT_FALSE(uniform(grab(*view)));
  EXPECT_EQ(scene.failures, 1);
}

// A base pipeline the HDR frames never needed (the grid's direct pipelines are
// templates, built only for a frame without the HDR chain) fails on the first
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
  if (!view->lastHdrActiveForTest()) {
    GTEST_SKIP() << "this GPU draws without the HDR chain: the directs exist from the first frame";
  }
  ASSERT_FALSE(uniform(hdr));

  PipelineFault fault("/grid.vert.qsb");
  const auto& t = view->stageTimesForTest();
  const std::uint64_t withheld = t.frames_withheld;
  view->setForceHdrFallbackForTest(true);
  ASSERT_TRUE(scene.renderUntil([&] { return t.frames_withheld > withheld; })) << "no direct frame was withheld";
  ASSERT_TRUE(scene.renderUntil([&] { return scene.failures > 0; }));
  EXPECT_EQ(scene.failures, 1);
  EXPECT_TRUE(uniform(grab(*view)));
  EXPECT_TRUE(view->renderingWarningForTest().contains(u"grid pipelines"_s))
      << view->renderingWarningForTest().toStdString();

  fault.clear();
  view->setForceHdrFallbackForTest(false);
  scene.resize();
  ASSERT_TRUE(scene.renderUntil([&] { return view->renderingWarningForTest().isEmpty(); }))
      << view->renderingWarningForTest().toStdString();
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
  if (!view->lastHdrActiveForTest()) {
    GTEST_SKIP() << "this GPU draws without the HDR chain: the directs exist from the first frame";
  }
  const glm::mat4 shown = view->lastViewProjectionForTest();

  PipelineFault fault("/grid.vert.qsb");
  const auto& t = view->stageTimesForTest();
  const std::uint64_t withheld = t.frames_withheld;
  // A rotation, not a pan: the stored matrix is relative to the render origin,
  // which a pan moves along with the focal.
  view->camera().rotate(80.0F, 40.0F);
  view->setForceHdrFallbackForTest(true);
  ASSERT_TRUE(scene.renderUntil([&] { return t.frames_withheld > withheld; })) << "no direct frame was withheld";
  EXPECT_EQ(view->lastViewProjectionForTest(), shown) << "picking projects with a frame that was never shown";
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
    return scene.failures > 0 || view->shadowCapabilityErrorForTest().contains(u"shadow receiver"_s);
  })) << view->renderingWarningForTest().toStdString();
  for (int frame = 0; frame < 3; ++frame) {
    (void)grab(*view);  // the frames after the fallback, direct and HDR alike
  }
  EXPECT_EQ(scene.failures, 0) << view->renderingWarningForTest().toStdString();
  EXPECT_FALSE(uniform(grab(*view))) << "the scene went down with its optional shadow receiver";
  EXPECT_FALSE(view->lastShadowActiveForTest());
  EXPECT_TRUE(view->shadowCapabilityErrorForTest().contains(u"shadow receiver"_s))
      << view->shadowCapabilityErrorForTest().toStdString();

  fault.clear();
  scene.resize();
  EXPECT_TRUE(scene.renderUntil([&] { return view->lastShadowActiveForTest(); }))
      << view->shadowCapabilityErrorForTest().toStdString();
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
  if (!view->lastHdrActiveForTest()) {
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
      "/ssao.frag.qsb", [](const SceneViewWidget& view) { return view.lastSsaoActiveForTest(); },
      [](const SceneViewWidget& view) { return view.ssaoCapabilityErrorForTest(); });
}

TEST_F(Scene3dRhiFailureTest, EdlRetriesOnANewQRhiAtTheSameSize) {
  checkStageRetriesOnANewQRhi(
      "/mesh_mask.frag.qsb", [](const SceneViewWidget& view) { return view.lastEdlActiveForTest(); },
      [](const SceneViewWidget& view) { return view.edlCapabilityErrorForTest(); });
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
  ASSERT_TRUE(scene.renderUntil([&] { return view->renderingWarningForTest().isEmpty(); }))
      << view->renderingWarningForTest().toStdString();

  PipelineFault again("/grid.vert.qsb");
  QWidget other;
  auto* layout = new QVBoxLayout(&other);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(scene.dock());
  other.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&other));
  ASSERT_TRUE(scene.renderUntil([&] { return scene.failures == 2; })) << "no failure in the new window";
  EXPECT_TRUE(uniform(grab(*view))) << "the failure state drew more than the backdrop";
  EXPECT_TRUE(view->renderingWarningForTest().contains(u"grid pipelines"_s))
      << view->renderingWarningForTest().toStdString();
  EXPECT_TRUE(scene.warningLabel()->isVisible());

  again.clear();
  scene.resize({80, 80});
  ASSERT_TRUE(scene.renderUntil([&] { return view->renderingWarningForTest().isEmpty(); }))
      << view->renderingWarningForTest().toStdString();
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
  EXPECT_TRUE(view->renderingWarningForTest().contains(u"no graphics device"_s))
      << view->renderingWarningForTest().toStdString();
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
  EXPECT_TRUE(view->renderingWarningForTest().contains(u"overlays"_s)) << view->renderingWarningForTest().toStdString();
  EXPECT_TRUE(view->lastOverlayRectsForTest()[0].isNull()) << "the HUD drew without its pipeline";

  fault.clear();
  scene.resize();
  ASSERT_TRUE(scene.renderUntil([&] { return !view->lastOverlayRectsForTest()[0].isNull(); }));
  EXPECT_TRUE(view->renderingWarningForTest().isEmpty()) << view->renderingWarningForTest().toStdString();
}

TEST_F(Scene3dRhiFailureTest, MissingHdrPipelineVariantFallsBackToDirect) {
  FailureScene scene;
  SceneViewWidget* view = scene.view();
  ASSERT_TRUE(scene.show());
  view->setForceHdrFallbackForTest(true);  // only the direct pipelines exist
  if (grab(*view).isNull()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }
  ASSERT_FALSE(view->lastHdrActiveForTest());

  // From now on the grid's HDR clones cannot be created; its direct pipelines,
  // created by the direct frames above, stand. The HDR frame that lost its grid
  // clone is not presented (the last frame stays); the next one draws directly.
  PipelineFault fault("/scene.frag.qsb");
  const std::uint64_t withheld = view->stageTimesForTest().frames_withheld;
  view->setForceHdrFallbackForTest(false);
  ASSERT_TRUE(scene.renderUntil([&] { return view->renderingWarningForTest().contains(u"HDR variant"_s); }))
      << view->renderingWarningForTest().toStdString();
  EXPECT_GT(view->stageTimesForTest().frames_withheld, withheld) << "the HDR frame without its grid was presented";
  const QImage with_grid = grab(*view);
  EXPECT_FALSE(view->lastHdrActiveForTest());
  EXPECT_EQ(scene.failures, 0);
  view->setGridVisible(false);
  const QImage without_grid = grab(*view);
  view->setGridVisible(true);
  EXPECT_TRUE(with_grid != without_grid) << "the direct fallback lost the grid";

  fault.clear();
  scene.resize();  // the HDR rejection is latched per output size
  ASSERT_TRUE(scene.renderUntil([&] { return view->lastHdrActiveForTest(); }));
  EXPECT_TRUE(scene.renderUntil([&] { return view->renderingWarningForTest().isEmpty(); }))
      << view->renderingWarningForTest().toStdString();
}

}  // namespace
