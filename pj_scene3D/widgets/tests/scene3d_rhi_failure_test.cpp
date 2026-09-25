// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The QRhi view's failure contract, driven by the pipeline fault injection of
// builds with tests (PJ_SCENE3D_TEST_FAIL_SHADER, see createGraphicsPipeline()):
//
//  - a base pipeline that cannot be created puts the view in its failure state:
//    no crash, only the backdrop, the reason in the dock's warning label and
//    renderFailed() once; a sibling QRhiWidget in the same window keeps
//    rendering; clearing the fault and re-initializing (a resize) recovers;
//  - the same holds after a recovery in a new top-level (a new QRhi);
//  - Qt's own failure (no QRhi for the window) reaches the warning too;
//  - optional stages drop alone: the overlays, and the HDR chain when a
//    pipeline has no HDR variant.
//
// Needs a real display like the render harness (see scene3d_render_test_support.h).

#include <gtest/gtest.h>
#include <rhi/qrhi.h>

#include <QLabel>
#include <QLoggingCategory>
#include <QOpenGLWidget>
#include <QRhiWidget>
#include <QTest>
#include <QVBoxLayout>
#include <functional>
#include <glm/glm.hpp>
#include <vector>

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

  int failures = 0;

 private:
  pj::scene3d::test::Scene3DRenderFixture fixture_;
};

class Scene3dRhiFailureTest : public ::testing::Test {
 protected:
  void SetUp() override {
    PJ_SKIP_ON_HEADLESS_QPA();
  }
};

TEST_F(Scene3dRhiFailureTest, BasePipelineFailureClearsWarnsAndRecovers) {
  PipelineFault fault("/trail.frag.qsb");
  FailureScene scene;
  SceneViewWidget* view = scene.view();
  ClearWidget* sibling = scene.addSibling();
  ASSERT_TRUE(scene.show());
  QImage failed;
  for (int frame = 0; frame < 3; ++frame) {
    failed = grab(*view);
  }
  if (failed.isNull() || view->rhiBackendNameForTest().isEmpty()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }

  EXPECT_TRUE(uniform(failed)) << "the failure state drew more than the backdrop";
  EXPECT_EQ(scene.failures, 1);
  const QString warning = view->renderingWarningForTest();
  EXPECT_TRUE(warning.contains(u"trail pipelines"_s)) << warning.toStdString();
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
  PipelineFault fault("/trail.frag.qsb");
  FailureScene scene;
  SceneViewWidget* view = scene.view();
  ASSERT_TRUE(scene.show());
  if (grab(*view).isNull() || view->rhiBackendNameForTest().isEmpty()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }
  ASSERT_EQ(scene.failures, 1);
  for (const int grow : {40, 80, 120}) {
    scene.resize({grow, grow});
    ASSERT_FALSE(grab(*view).isNull());
  }
  EXPECT_EQ(scene.failures, 1);
  EXPECT_EQ(BackendLogCounter::lines, 1);
}

// After a recovery the fault returns and the view moves to another top-level
// (a new QRhi): it fails there once more, and recovers there once the fault is gone.
TEST_F(Scene3dRhiFailureTest, FailureAfterRecoveryRecoversInANewWindow) {
  PipelineFault fault("/trail.frag.qsb");
  FailureScene scene;
  SceneViewWidget* view = scene.view();
  ASSERT_TRUE(scene.show());
  if (grab(*view).isNull() || view->rhiBackendNameForTest().isEmpty()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }
  ASSERT_EQ(scene.failures, 1);
  fault.clear();
  scene.resize();
  ASSERT_TRUE(scene.renderUntil([&] { return view->renderingWarningForTest().isEmpty(); }))
      << view->renderingWarningForTest().toStdString();

  PipelineFault again("/trail.frag.qsb");
  QWidget other;
  auto* layout = new QVBoxLayout(&other);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(scene.dock());
  other.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&other));
  ASSERT_TRUE(scene.renderUntil([&] { return scene.failures == 2; })) << "no failure in the new window";
  EXPECT_TRUE(uniform(grab(*view))) << "the failure state drew more than the backdrop";
  EXPECT_TRUE(view->renderingWarningForTest().contains(u"trail pipelines"_s))
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

  // From now on the grid's HDR clones cannot be created; its direct pipelines stand.
  PipelineFault fault("/scene.frag.qsb");
  view->setForceHdrFallbackForTest(false);
  ASSERT_TRUE(scene.renderUntil([&] { return view->renderingWarningForTest().contains(u"HDR variant"_s); }))
      << view->renderingWarningForTest().toStdString();
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
