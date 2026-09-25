// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Frame budgets of the Scene3D view, in both builds (P5 budgets, Gate A). Each
// case builds a real Scene3DDockWidget scene through public APIs, drives frames
// and prints one line:
//
//   S3D_BENCH case=<name> build=gl|rhi backend=<API> device="<adapter>" frames=N
//     wall_ms=<median per frame> wall_min wall_max cpu_ms=<GUI-thread CPU per
//     frame> rss_mib hwm_mib gpu_drm_mib gpu_vma_mib idle_frames first_show_ms
//     [case extras]
//
// wall is request-to-finished-frame (QRhiWidget::frameSubmitted / the GL
// view's frameSwapped); frames are driven back to back, so a GPU-bound case
// reports its throughput. rss/hwm: VmRSS and its peak since show() (Linux).
// gpu_drm_mib: the DRM driver's per-process total (Linux fdinfo; n/a on
// software rasterizers and the NVIDIA driver); gpu_vma_mib: the top-level
// QRhi's allocator (Vulkan). idle_frames: frames the settled view still
// rendered in 1 s, which the QRhi build requires to be 0.
//
// Sizes default to a ctest smoke; PJ_S3D_BENCH_FULL=1 selects the budget sizes
// and PJ_S3D_BENCH_<NAME> overrides one (CLOUD_POINTS, STREAM_POINTS, CUBES,
// LINE_VERTICES, MARKERS, LINKS, FRAMES, MOVES); PJ_S3D_BENCH_DUMP_DIR=<dir>
// saves each case's final frame there. Needs a real display; skips on
// the offscreen QPA. The QRhi build registers it per backend (PJ_GRAPHICS_API).

#include <gtest/gtest.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QLayout>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <random>
#include <string>
#include <vector>

#ifndef PJ_SCENE3D_RHI
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#endif

#include "pj_base/builtin/frame_transforms.hpp"
#include "pj_base/builtin/frame_transforms_codec.hpp"
#include "pj_base/builtin/scene_entities.hpp"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_bench_probes.h"
#include "scene3d_render_test_support.h"

namespace {

using BT = PJ::sdk::BuiltinObjectType;
using pj::scene3d::SceneViewWidget;

constexpr char kFrame[] = "map";
constexpr int kWarmupFrames = 3;
constexpr PJ::Timestamp kStepNs = 10'000'000;  // data time per driven frame

#ifdef PJ_SCENE3D_RHI
constexpr char kBuild[] = "rhi";
#else
constexpr char kBuild[] = "gl";
#endif

bool fullSizes() {
  return qEnvironmentVariableIntValue("PJ_S3D_BENCH_FULL") == 1;
}
// PJ_S3D_BENCH_<name>, else the budget size under PJ_S3D_BENCH_FULL=1, else the smoke size.
long long benchSize(const char* name, long long smoke, long long full) {
  bool ok = false;
  const long long value = qEnvironmentVariable(("PJ_S3D_BENCH_" + std::string(name)).c_str()).toLongLong(&ok);
  if (ok && value > 0) {
    return value;
  }
  return fullSizes() ? full : smoke;
}
int benchFrames() {
  return static_cast<int>(benchSize("FRAMES", 5, 60));
}
long long cloudPoints() {
  return benchSize("CLOUD_POINTS", 20'000, 10'000'000);
}
int robotLinks() {
  return static_cast<int>(benchSize("LINKS", 5, 50));
}

// `count` points uniformly in a 20 x 20 x 2 m box around the origin.
std::vector<uint8_t> randomCloud(long long count, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> xy(-10.0F, 10.0F);
  std::uniform_real_distribution<float> z(0.0F, 2.0F);
  std::vector<glm::vec3> points(static_cast<std::size_t>(count));
  for (auto& point : points) {
    point = {xy(rng), xy(rng), z(rng)};
  }
  return pj::scene3d::test::serializeCloud(points, kFrame);
}

// A star-shaped robot: `links` box/cylinder/sphere visuals, each on a
// continuous joint of base_link, so TF poses every link.
QByteArray robotUrdf(int links) {
  QByteArray urdf = "<robot name=\"bench\">\n  <link name=\"base_link\"/>\n";
  for (int i = 0; i < links; ++i) {
    const QByteArray n = QByteArray::number(i);
    const char* geometry = i % 3 == 0   ? "<box size=\"0.3 0.2 0.1\"/>"
                           : i % 3 == 1 ? "<cylinder radius=\"0.1\" length=\"0.3\"/>"
                                        : "<sphere radius=\"0.12\"/>";
    urdf += "  <link name=\"link_" + n + "\"><visual><geometry>" + geometry + "</geometry></visual></link>\n";
    urdf += "  <joint name=\"joint_" + n + "\" type=\"continuous\"><parent link=\"base_link\"/><child link=\"link_" +
            n + "\"/><axis xyz=\"0 0 1\"/></joint>\n";
  }
  return urdf + "</robot>\n";
}

// The robot's poses at step `k`: base_link at the map origin, link i on a
// 10-wide grid, spinning about Z.
std::vector<uint8_t> robotTf(int links, int k, PJ::Timestamp t) {
  PJ::sdk::FrameTransforms transforms;
  PJ::sdk::FrameTransform base;
  base.timestamp = t;
  base.parent_frame_id = kFrame;
  base.child_frame_id = "base_link";
  transforms.transforms.push_back(base);
  for (int i = 0; i < links; ++i) {
    PJ::sdk::FrameTransform edge;
    edge.timestamp = t;
    edge.parent_frame_id = "base_link";
    edge.child_frame_id = "link_" + std::to_string(i);
    edge.translation = {0.5 * (i % 10) - 2.25, 0.5 * (i / 10) - 1.0, 0.3};
    const double angle = 0.05 * k + i;
    edge.rotation.z = std::sin(angle / 2.0);
    edge.rotation.w = std::cos(angle / 2.0);
    transforms.transforms.push_back(edge);
  }
  return PJ::serializeFrameTransforms(transforms);
}

// One Scene3D dock on a real session, the numbers of one case, and its report.
class SceneBench {
 public:
  // Binds the session TF service first, as the app does: the GL renderer
  // draws nothing without a transform buffer, even in the fixed frame.
  SceneBench() {
    fixture_.window().resize(1200, 700);
    fixture_.dock()->setTransformService(&fixture_.transforms());
  }

  pj::scene3d::test::Scene3DRenderFixture& fixture() {
    return fixture_;
  }
  PJ::Scene3DDockWidget& dock() {
    return *fixture_.dock();
  }
  SceneViewWidget& view() {
    return *fixture_.dock()->sceneView();
  }
  [[nodiscard]] std::uint64_t finished() const {
    return counter_ == nullptr ? 0 : counter_->count();
  }
  // Per-frame wall times of the last drive().
  [[nodiscard]] const std::vector<double>& lastWallMs() const {
    return wall_ms_;
  }

  // A TF topic of the robot's link poses at steps 0..steps-1, ingested.
  void addRobotTf(int links, int steps) {
    const auto time_of = [](int k) { return 100 + static_cast<PJ::Timestamp>(k) * kStepNs; };
    const PJ::ObjectTopicId tf = fixture_.add("/tf", BT::kFrameTransforms, robotTf(links, 0, time_of(0)));
    for (int k = 1; k < steps; ++k) {
      fixture_.push(tf, time_of(k), robotTf(links, k, time_of(k)));
    }
    fixture_.transforms().ingestFrameTransformsForDataset(fixture_.dataset());
  }
  // A local-file URDF robot layer.
  void addRobot(int links) {
    ASSERT_TRUE(urdf_dir_.isValid());
    const QString path = urdf_dir_.filePath(QStringLiteral("bench.urdf"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(robotUrdf(links));
    file.close();
    ASSERT_NE(dock().addRobotModelLayer(path).id, 0U);
  }

  // Shows the window, waits for the first finished frame (false when none
  // came; first_show_ms is show() to it), lets the scene settle and puts the
  // camera over it.
  [[nodiscard]] bool show() {
    if (fixture_.dock()->sceneView() == nullptr) {
      return false;
    }
    counter_ = std::make_unique<pj_bench::FrameCounter>(&view());
    dock().setFixedFrame(QString::fromLatin1(kFrame));
    dock().onTrackerTime(100.0e-9);  // every scene starts at t = 100 ns
    pj_bench::resetPeakRss();
    QElapsedTimer timer;
    timer.start();
    fixture_.window().show();
    if (!pj_bench::waitUntil([this] { return finished() > 0; })) {
      return false;
    }
    first_show_ms_ = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
    settle();  // after the QRhi view's one-shot fit, which the camera below replaces
    camera_.focal = {0.0F, 0.0F, 0.5F};
    camera_.radius = 22.0F;
    camera_.azimuth = glm::radians(-60.0F);
    camera_.elevation = glm::radians(35.0F);
    view().camera().adoptState(camera_);
    return true;
  }
  // Lets queued decodes, uploads and fits land: returns once no frame came for 1 s.
  void settle() {
    (void)pj_bench::idleFrames([this] { return finished(); }, 1000, 15000, 0);
  }

  // Moves the orbit camera by `degrees` of azimuth.
  void orbit(float degrees = 1.0F) {
    camera_.azimuth += glm::radians(degrees);
    view().camera().adoptState(camera_);
  }
  // Pans the camera focal by `dy` metres along +Y (from wherever follow left it).
  void pan(float dy) {
    camera_ = view().camera().state();
    camera_.focal.y += dy;
    view().camera().adoptState(camera_);
  }

  // Runs `step`, requests a frame and waits until one is finished: ms from
  // `step` to that frame, or negative on timeout.
  double frame(const std::function<void()>& step) {
    const std::uint64_t before = finished();
    QElapsedTimer timer;
    timer.start();
    step();
    view().update();
    if (!pj_bench::waitUntil([&] { return finished() > before; })) {
      return -1.0;
    }
    return static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
  }

  // kWarmupFrames, then `frames` measured frames of `step(k)` (k counts every
  // driven frame from 0); `prepare(k)` runs untimed before each.
  void drive(int frames, const std::function<void(int)>& step, const std::function<void(int)>& prepare = {}) {
    wall_ms_.clear();
    double cpu_start = 0.0;
    for (int k = 0; k < kWarmupFrames + frames; ++k) {
      if (k == kWarmupFrames) {
        cpu_start = pj_bench::threadCpuMs();
      }
      if (prepare) {
        prepare(k);
      }
      const double ms = frame([&] { step(k); });
      ASSERT_GE(ms, 0.0) << "frame " << k << " never finished";
      if (k >= kWarmupFrames) {
        wall_ms_.push_back(ms);
      }
    }
    cpu_ms_ = frames > 0 ? (pj_bench::threadCpuMs() - cpu_start) / frames : 0.0;
  }

  // Float (reparent to a new top-level), redock and tab-switch (hide, then
  // show) the dock `moves` times, each timed to its first finished frame;
  // the medians as "float_ms=.. redock_ms=.. tab_ms=..".
  std::string dockMoves(int moves) {
    std::vector<double> float_ms;
    std::vector<double> redock_ms;
    std::vector<double> tab_ms;
    PJ::Scene3DDockWidget* moved = &dock();
    QLayout* layout = fixture_.window().layout();
    const auto timed = [this](const std::function<void()>& action) {
      const std::uint64_t before = finished();
      QElapsedTimer timer;
      timer.start();
      action();
      const bool done = pj_bench::waitUntil([&] { return finished() > before; });
      const double ms = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
      settle();
      return done ? ms : -1.0;
    };
    for (int i = 0; i < moves; ++i) {
      float_ms.push_back(timed([&] {
        moved->setParent(nullptr);
        moved->resize(fixture_.window().size());
        moved->show();
      }));
      redock_ms.push_back(timed([&] {
        layout->addWidget(moved);
        moved->show();
      }));
      moved->hide();
      QCoreApplication::processEvents();
      tab_ms.push_back(timed([&] { moved->show(); }));
    }
    const auto median = [](std::vector<double> values) { return pj_bench::stats(std::move(values)).median; };
    return "float_ms=" + std::to_string(median(float_ms)) + " redock_ms=" + std::to_string(median(redock_ms)) +
           " tab_ms=" + std::to_string(median(tab_ms));
  }

  // Prints the case line: the last drive()'s numbers, the probes, then `extra`,
  // after counting the settled view's idle frames (asserted 0 in the QRhi build).
  void report(const char* name, const std::string& extra = {}) {
    if (const QString dir = qEnvironmentVariable("PJ_S3D_BENCH_DUMP_DIR"); !dir.isEmpty()) {
      pj::scene3d::test::grab(view()).save(
          dir + u'/' + QString::fromLatin1(name) + u'_' + QString::fromLatin1(kBuild) + u".png");
    }
    const std::uint64_t idle_frames = pj_bench::idleFrames([this] { return finished(); });
    const pj_bench::Stats wall = pj_bench::stats(wall_ms_);
#ifdef PJ_SCENE3D_RHI
    QRhi* rhi = pj_bench::rhiOf(view());
    const double vma = pj_bench::rhiAllocatorMib(rhi);
    const QString backend = view().rhiBackendNameForTest();
    const QString device = rhi != nullptr ? QString::fromUtf8(rhi->driverInfo().deviceName) : QString();
#else
    const double vma = -1.0;
    const QString backend = QStringLiteral("OpenGL");
    QString device;
    view().makeCurrent();
    if (QOpenGLContext* context = view().context(); context != nullptr) {
      device = QString::fromLatin1(reinterpret_cast<const char*>(context->functions()->glGetString(GL_RENDERER)));
    }
    view().doneCurrent();
#endif
    std::printf(
        "S3D_BENCH case=%s build=%s backend=%s device=\"%s\" frames=%zu wall_ms=%.2f wall_min=%.2f wall_max=%.2f "
        "cpu_ms=%.2f rss_mib=%s hwm_mib=%s gpu_drm_mib=%s gpu_vma_mib=%s idle_frames=%llu first_show_ms=%.1f%s%s\n",
        name, kBuild, qPrintable(backend), qPrintable(device), wall_ms_.size(), wall.median, wall.min, wall.max,
        cpu_ms_, pj_bench::fmt(pj_bench::rssMib()).constData(), pj_bench::fmt(pj_bench::peakRssMib()).constData(),
        pj_bench::fmt(pj_bench::drmTotalMib()).constData(), pj_bench::fmt(vma).constData(),
        static_cast<unsigned long long>(idle_frames), first_show_ms_, extra.empty() ? "" : " ", extra.c_str());
    std::fflush(stdout);
#ifdef PJ_SCENE3D_RHI
    EXPECT_EQ(idle_frames, 0U) << "a settled, unchanged view kept rendering";
#endif
  }

 private:
  pj::scene3d::test::Scene3DRenderFixture fixture_;
  QTemporaryDir urdf_dir_;
  std::unique_ptr<pj_bench::FrameCounter> counter_;
  pj::scene3d::CameraState camera_;
  std::vector<double> wall_ms_;
  double cpu_ms_ = 0.0;
  double first_show_ms_ = 0.0;
};

#ifdef PJ_SCENE3D_RHI
// What the last frame drew, so a run that silently drew nothing shows up.
std::string drawn(SceneViewWidget& view) {
  return "points=" + std::to_string(view.lastPointVertexCountForTest()) +
         " markers=" + std::to_string(view.lastMarkerInstanceCountForTest()) +
         " models=" + std::to_string(view.lastModelDrawCountForTest()) +
         " hdr=" + std::to_string(static_cast<int>(view.lastHdrActiveForTest()));
}
#else
std::string drawn(SceneViewWidget& /*view*/) {
  return {};
}
#endif

class Scene3dFrameBenchmark : public ::testing::Test {
 protected:
  void SetUp() override {
    PJ_SKIP_ON_HEADLESS_QPA();
  }
};

#define SHOW_OR_SKIP(bench)                                          \
  if (!(bench).show()) {                                             \
    PJ_SKIP_WITHOUT_GPU("the Scene3D view never presented a frame"); \
  }

// A static cloud under a slowly orbiting camera.
TEST_F(Scene3dFrameBenchmark, CloudStatic) {
  SceneBench bench;
  bench.fixture().add("/points", BT::kPointCloud, randomCloud(cloudPoints(), 1));
  SHOW_OR_SKIP(bench);
  ASSERT_NO_FATAL_FAILURE(bench.drive(benchFrames(), [&](int) { bench.orbit(); }));
  bench.report("cloud_static", drawn(bench.view()));
}

// A new cloud every frame (live streaming): pushed untimed, then the tracker
// moves onto it.
TEST_F(Scene3dFrameBenchmark, CloudStream) {
  SceneBench bench;
  const long long points = benchSize("STREAM_POINTS", 5'000, 300'000);
  const std::vector<std::vector<uint8_t>> clouds{randomCloud(points, 1), randomCloud(points, 2)};
  const PJ::ObjectTopicId id = bench.fixture().add("/points", BT::kPointCloud, clouds[0]);
  SHOW_OR_SKIP(bench);
  const auto time_of = [](int k) { return 100 + static_cast<PJ::Timestamp>(k + 1) * kStepNs; };
  ASSERT_NO_FATAL_FAILURE(bench.drive(
      benchFrames(), [&](int k) { bench.dock().onTrackerTime(static_cast<double>(time_of(k)) / 1.0e9); },
      [&](int k) { bench.fixture().push(id, time_of(k), clouds[static_cast<std::size_t>(k % 2)]); }));
  bench.report("cloud_stream", drawn(bench.view()));
}

// Many cube markers and long line strips, orbiting.
TEST_F(Scene3dFrameBenchmark, CubesAndLinesOrbit) {
  SceneBench bench;
  const long long cube_count = benchSize("CUBES", 1'000, 100'000);
  const long long line_vertices = benchSize("LINE_VERTICES", 10'000, 1'000'000);
  PJ::sdk::SceneEntity cubes;
  cubes.frame_id = kFrame;
  cubes.id = "cubes";
  const long long side = std::max(1LL, std::llround(std::cbrt(static_cast<double>(cube_count))));
  for (long long i = 0; i < cube_count; ++i) {
    PJ::sdk::CubePrimitive cube;
    cube.pose.position = {
        0.2 * static_cast<double>(i % side) - 0.1 * static_cast<double>(side),
        0.2 * static_cast<double>((i / side) % side) - 0.1 * static_cast<double>(side),
        0.2 * static_cast<double>(i / (side * side))};
    cube.size = {0.1, 0.1, 0.1};
    cube.color = {static_cast<uint8_t>(i % 256), 160, 90, 255};
    cubes.cubes.push_back(cube);
  }
  PJ::sdk::SceneEntity lines;
  lines.frame_id = kFrame;
  lines.id = "lines";
  const long long strips = std::max(1LL, line_vertices / 10'000);
  for (long long s = 0; s < strips; ++s) {
    PJ::sdk::LinePrimitive strip;
    strip.thickness = 0.02;
    strip.color = {230, 200, 40, 255};
    for (long long v = 0; v < line_vertices / strips; ++v) {
      const double angle = 0.01 * static_cast<double>(v);
      const double radius = 3.0 + 0.05 * static_cast<double>(s);
      strip.points.push_back({radius * std::cos(angle), radius * std::sin(angle), 0.0005 * static_cast<double>(v)});
    }
    lines.lines.push_back(std::move(strip));
  }
  PJ::sdk::SceneEntities scene;
  scene.entities.push_back(std::move(cubes));
  scene.entities.push_back(std::move(lines));
  bench.fixture().add("/entities", BT::kSceneEntities, PJ::serializeSceneEntities(scene));
  SHOW_OR_SKIP(bench);
  ASSERT_NO_FATAL_FAILURE(bench.drive(benchFrames(), [&](int) { bench.orbit(); }));
  bench.report("cubes_lines_orbit", drawn(bench.view()));
}

// Many sphere markers while the camera pans and follows a TF frame that moves
// every frame.
TEST_F(Scene3dFrameBenchmark, MarkersPanFollow) {
  SceneBench bench;
  const long long marker_count = benchSize("MARKERS", 100, 10'000);
  const int frames = benchFrames();
  const auto time_of = [](int k) { return 100 + static_cast<PJ::Timestamp>(k) * kStepNs; };
  const auto base_tf = [&](int k) {
    PJ::sdk::FrameTransforms transforms;
    PJ::sdk::FrameTransform edge;
    edge.timestamp = time_of(k);
    edge.parent_frame_id = kFrame;
    edge.child_frame_id = "base";
    edge.translation = {0.02 * k, 0.0, 0.0};
    transforms.transforms.push_back(edge);
    return PJ::serializeFrameTransforms(transforms);
  };
  const PJ::ObjectTopicId tf = bench.fixture().add("/tf", BT::kFrameTransforms, base_tf(0));
  for (int k = 1; k <= kWarmupFrames + frames; ++k) {
    bench.fixture().push(tf, time_of(k), base_tf(k));
  }
  bench.fixture().transforms().ingestFrameTransformsForDataset(bench.fixture().dataset());
  PJ::sdk::SceneEntities scene;
  const long long side = std::max(1LL, std::llround(std::sqrt(static_cast<double>(marker_count))));
  for (long long i = 0; i < marker_count; ++i) {
    PJ::sdk::SceneEntity entity;
    entity.frame_id = kFrame;
    entity.id = "m" + std::to_string(i);
    PJ::sdk::SpherePrimitive sphere;
    sphere.pose.position = {
        0.25 * static_cast<double>(i % side) - 0.125 * static_cast<double>(side),
        0.25 * static_cast<double>(i / side) - 0.125 * static_cast<double>(side), 0.2};
    sphere.size = {0.15, 0.15, 0.15};
    sphere.color = {80, 160, static_cast<uint8_t>(i % 256), 255};
    entity.spheres.push_back(sphere);
    scene.entities.push_back(std::move(entity));
  }
  bench.fixture().add("/markers", BT::kSceneEntities, PJ::serializeSceneEntities(scene));
  SHOW_OR_SKIP(bench);
  bench.dock().setFollowFrame(QStringLiteral("base"));
  ASSERT_NO_FATAL_FAILURE(bench.drive(frames, [&](int k) {
    bench.dock().onTrackerTime(static_cast<double>(time_of(k + 1)) / 1.0e9);
    bench.pan(0.01F);
  }));
  bench.report("markers_pan_follow", drawn(bench.view()));
}

// A URDF robot whose every link TF moves each frame; the model never reloads.
TEST_F(Scene3dFrameBenchmark, RobotTfAnimated) {
  SceneBench bench;
  const int links = robotLinks();
  const int frames = benchFrames();
  bench.addRobotTf(links, kWarmupFrames + frames + 1);
  ASSERT_NO_FATAL_FAILURE(bench.addRobot(links));
  SHOW_OR_SKIP(bench);
  ASSERT_NO_FATAL_FAILURE(bench.drive(frames, [&](int k) {
    bench.dock().onTrackerTime(static_cast<double>(100 + static_cast<PJ::Timestamp>(k + 1) * kStepNs) / 1.0e9);
  }));
  bench.report("robot_tf_animated", drawn(bench.view()));
}

// `count` small clouds, one layer each, orbiting: the per-layer overhead.
// The smoke runs a tenth of the layers.
void runLayers(const char* name, long long count) {
  if (!fullSizes()) {
    count = std::max(1LL, count / 10);
  }
  SceneBench bench;
  for (long long i = 0; i < count; ++i) {
    std::vector<glm::vec3> points(100);
    for (std::size_t p = 0; p < points.size(); ++p) {
      points[p] = {
          static_cast<float>(i % 32) * 0.5F - 8.0F + 0.004F * static_cast<float>(p),
          static_cast<float>(i / 32) * 0.5F - 8.0F, 0.3F};
    }
    bench.fixture().add(
        "/points_" + std::to_string(i), BT::kPointCloud, pj::scene3d::test::serializeCloud(points, kFrame));
  }
  SHOW_OR_SKIP(bench);
  ASSERT_NO_FATAL_FAILURE(bench.drive(benchFrames(), [&](int) { bench.orbit(); }));
  bench.report(name, "layers=" + std::to_string(count) + " " + drawn(bench.view()));
}
TEST_F(Scene3dFrameBenchmark, Layers10) {
  runLayers("layers_10", 10);
}
TEST_F(Scene3dFrameBenchmark, Layers100) {
  runLayers("layers_100", 100);
}
TEST_F(Scene3dFrameBenchmark, Layers1000) {
  runLayers("layers_1000", 1000);
}

// Float, redock and tab-switch of a dock holding a large cloud, then of one
// holding a many-link robot, each to its first finished frame. Float is a
// reparent to a new top-level (what an ADS floating container is), tab-switch
// a hide/show on the same QRhi (what hiding a workspace tab does; an ADS
// dock-area tab switch reparents through a parentless state, like a float).
TEST_F(Scene3dFrameBenchmark, DockMovesCloud) {
  SceneBench bench;
  bench.fixture().add("/points", BT::kPointCloud, randomCloud(cloudPoints(), 1));
  SHOW_OR_SKIP(bench);
  const std::string moves = bench.dockMoves(static_cast<int>(benchSize("MOVES", 2, 5)));
  bench.report("dock_moves_cloud", moves + " " + drawn(bench.view()));
}
TEST_F(Scene3dFrameBenchmark, DockMovesRobot) {
  SceneBench bench;
  const int links = robotLinks();
  bench.addRobotTf(links, 1);
  ASSERT_NO_FATAL_FAILURE(bench.addRobot(links));
  SHOW_OR_SKIP(bench);
  const std::string moves = bench.dockMoves(static_cast<int>(benchSize("MOVES", 2, 5)));
  bench.report("dock_moves_robot", moves + " " + drawn(bench.view()));
}

// Show to first finished frame of a cloud plus a robot, in a fresh process:
// backend, pipelines and first uploads (first_show_ms).
TEST_F(Scene3dFrameBenchmark, FirstShow) {
  SceneBench bench;
  const int links = robotLinks();
  bench.addRobotTf(links, 1);
  bench.fixture().add("/points", BT::kPointCloud, randomCloud(cloudPoints(), 1));
  ASSERT_NO_FATAL_FAILURE(bench.addRobot(links));
  SHOW_OR_SKIP(bench);
  std::string extra = drawn(bench.view());
#ifdef PJ_SCENE3D_RHI
  if (QRhi* rhi = pj_bench::rhiOf(bench.view()); rhi != nullptr) {
    extra += " pipeline_ms=" + std::to_string(rhi->statistics().totalPipelineCreationTime);
  }
#endif
  bench.report("first_show", extra);
}

#ifdef PJ_SCENE3D_RHI
// The HDR chain's depth replay (every draw re-submitted into the single-sample
// depth target, forced by setForceDepthReplayForTest()) against the default
// chain (the multisample depth resolve where the backend has one; the replay
// elsewhere), in alternating blocks on one large cloud. The GL renderer has
// neither.
TEST_F(Scene3dFrameBenchmark, DepthReplayOnOff) {
  SceneBench bench;
  bench.fixture().add("/points", BT::kPointCloud, randomCloud(cloudPoints(), 1));
  SHOW_OR_SKIP(bench);
  const int frames = std::max(2, benchFrames() / 2);
  std::vector<double> replay;
  std::vector<double> chain;
  bool resolves = false;
  for (int round = 0; round < 4; ++round) {
    const bool forced = round % 2 == 0;
    bench.view().setForceDepthReplayForTest(forced);
    ASSERT_NO_FATAL_FAILURE(bench.drive(frames, [&](int) { bench.orbit(); }));
    (forced ? replay : chain).push_back(pj_bench::stats(bench.lastWallMs()).median);
    resolves = resolves || bench.view().depthResolveActiveForTest();
  }
  bench.view().setForceDepthReplayForTest(false);
  bench.report(
      "depth_replay", "replay_ms=" + std::to_string(pj_bench::stats(replay).median) +
                          " default_ms=" + std::to_string(pj_bench::stats(chain).median) +
                          " resolves=" + std::to_string(static_cast<int>(resolves)) + " " + drawn(bench.view()));
}
#endif

}  // namespace
