// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Frame budgets of the Scene3D view (Gate A, TECHNICAL_NOTES §9). Each
// case builds a real Scene3DDockWidget scene through public APIs, drives frames
// and prints one line:
//
//   S3D_BENCH case=<name> build=rhi backend=<API> device="<adapter>" frames=N
//     wall_ms=<median per frame> wall_min wall_p90 wall_max cpu_ms=<GUI-thread
//     CPU per frame, mean> cpu_p50 cpu_p90 faults=<minor page faults per frame,
//     median> ingest_ms ingest_cpu_ms rss_mib hwm_mib gpu_drm_mib gpu_vma_mib
//     idle_frames first_show_ms cpu_mhz cold_ms warmup_frames warmup_ms gpu_mhz
//     window_ns=<begin>-<end> [stages_ms=...] [case extras]
//
// wall is request-to-finished-frame (QRhiWidget::frameSubmitted); frames are driven back to back, so a GPU-bound case
// reports its throughput. The measured frames follow a time-based warm-up
// (pj_bench::WarmUp: at least 250 ms, 5 steady frames and a settled GPU clock,
// at most 120 frames; warmup_frames, warmup_ms, and gpu_mhz at its end); cold_ms is the first
// warm-up frame, the first interaction after a second of idle, when the GPU
// clock is still low; gpu_mhz_range is the clock over the measured frames (i915
// can still raise it among them). cpu and faults cover the same span as wall. A case
// that feeds new data per frame (CloudStream) ingests it untimed before the
// request; ingest_ms / ingest_cpu_ms report that ingestion (median wall, mean
// GUI-thread CPU), apart from the frame. rss/hwm: VmRSS and its peak since
// show() (Linux). gpu_drm_mib: the DRM driver's per-process total (Linux fdinfo;
// n/a on software rasterizers and the NVIDIA driver); gpu_vma_mib: the
// top-level QRhi's allocator (Vulkan). idle_frames: frames the settled view
// still rendered in 1 s, required to be 0: its full render() calls (test
// hooks), not frameSubmitted, which Qt also emits when it only composites the
// retained texture (a repaint elsewhere in the window). cpu_mhz: the
// mean scaling_cur_freq at the report. window_ns: the measured span in
// CLOCK_MONOTONIC ns, to cut a `perf record -k CLOCK_MONOTONIC` profile to it.
//
// QSG_RHI_PROFILE=1 adds gpu_ms (the view's GPU time per frame,
// QRhi timestamps; the profiling itself slows the frame), PJ_S3D_BENCH_NO_HDR=1
// forces the direct pass (no HDR chain), PJ_S3D_BENCH_NO_SHADOWS=1 turns
// shadows off and PJ_S3D_BENCH_NO_SSAO=1 / PJ_S3D_BENCH_NO_EDL=1 those stages,
// all for attributing a GPU-bound frame, not for Gate A.
//
// stages_ms (with PJ_RHI_TEST_HOOKS): per-frame medians of step (the
// case's own work before update()), sched (update() to render()), targets (HDR
// chain), admit (layers' prepareForRender(): synchronous decode and pack,
// placement), prepare (streams, upload enqueue, uniforms), submit (pass
// recording) and present (render() end to frameSubmitted: on OpenGL the command
// execution with its buffer uploads, then the swap). The dock and first-show
// cases split their first frame the same way, with init (initialize()'s base
// build: every family's pipelines). A dock move or first show can run several
// callbacks (QRhiWidget renders synchronously on Show and resize): its split
// sums the stages of every full render() in it, sched is the rest of its wall
// outside render() and present (window and QRhi creation, initialize(), the
// other callbacks' offscreen frames), and <kind>_paints / _renders / _skips
// count its paint events, render() calls and the paints the view skipped
// (SceneViewWidget::paintEvent: no offscreen frame).
//
// Sizes default to a ctest smoke; PJ_S3D_BENCH_FULL=1 selects the budget sizes
// and PJ_S3D_BENCH_<NAME> overrides one (CLOUD_POINTS, STREAM_POINTS, CUBES,
// LINE_VERTICES, MARKERS, LINKS, FRAMES, MOVES); PJ_S3D_BENCH_DUMP_DIR=<dir>
// saves each case's final frame there, PJ_S3D_BENCH_CSV_DIR=<dir> its
// per-frame samples with their spans (<case>_<build>.csv). Needs a real display; skips on
// the offscreen QPA. It is registered per backend (PJ_GRAPHICS_API).
//
// As a ctest smoke it asserts no timing, only that each case did its work: the
// cloud, markers or model drawn in its last frame (the view's draw counters)
// and, for the animated cases, that the data
// advanced (a new cloud uploaded every frame, the followed frame moved, the
// robot's pose changed the image).

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QLayout>
#include <QTemporaryDir>
#include <QTextStream>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../rhi/passes/marker_rhi_pass.h"
#include "../rhi/passes/model_rhi_pass.h"
#include "../rhi/passes/point_rhi_pass.h"
#include "../rhi/scene_view_widget_rhi_quality_p.h"
#include "pj_base/builtin/frame_transforms.hpp"
#include "pj_base/builtin/frame_transforms_codec.hpp"
#include "pj_base/builtin/scene_entities.hpp"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_bench_probes.h"
#include "pj_bench_scene_work.h"
#include "scene3d_render_test_support.h"
#include "scene_view_test_access.h"

namespace {

using BT = PJ::sdk::BuiltinObjectType;
using pj::scene3d::SceneViewTestAccess;
using pj::scene3d::SceneViewWidget;

constexpr char kFrame[] = "map";
constexpr PJ::Timestamp kStepNs = 10'000'000;  // data time per driven frame

constexpr char kBuild[] = "rhi";

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

// A frame's stages (see the header comment); init is initialize()'s base build,
// set on a dock move or first show only.
enum Stage : std::uint8_t { kStep, kSched, kTargets, kAdmit, kPrepare, kSubmit, kPresent, kInit, kStageCount };
constexpr std::array<const char*, kStageCount> kStageNames{"step",    "sched",  "targets", "admit",
                                                           "prepare", "submit", "present", "init"};
using Stages = std::array<double, kStageCount>;

// One measured frame: wall and GUI-thread CPU ms, the process's minor page
// faults, and its stages in ms (-1 where the build has no stamps).
struct Sample {
  double wall_ms = 0.0;
  double cpu_ms = 0.0;
  double faults = 0.0;
  double uploads = 0.0;     // point-cloud uploads + marker layer rebuilds + model mesh builds
  double upload_mib = 0.0;  // point-cloud bytes uploaded
  double inits = 0.0;       // initialize() base builds
  double paints = 0.0;      // paint events, render() calls and skipped paints (lifecycle operations)
  double renders = 0.0;
  double skips = 0.0;
  double gpu_ms = -1.0;       // the view's last completed GPU frame (QSG_RHI_PROFILE=1), -1 without
  double gpu_mhz = -1.0;      // the GPU clock at the frame's end (pj_bench::gpuMhz()), -1 unreadable
  std::int64_t begin_ns = 0;  // the sample's span (steadyNs()), for cutting a profile to it
  std::int64_t end_ns = 0;
  Stages stages{};
};

using pj_bench::SceneViewWork;

// One Scene3D dock on a real session, the numbers of one case, and its report.
class SceneBench {
 public:
  // Binds the session TF service first, as the app does.
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
  // Frames the view drew: its full render() calls where the build counts them,
  // else its finished frames.
  [[nodiscard]] std::uint64_t renders() {
    return counter_ == nullptr ? 0 : pj_bench::viewRenders(view(), *counter_);
  }
  // The fewest uploads of any sample of the last drive(); 0 without the
  // counters (no PJ_RHI_TEST_HOOKS).
  [[nodiscard]] double minUploads() const {
    double least = samples_.empty() ? 0.0 : samples_.front().uploads;
    for (const Sample& sample : samples_) {
      least = std::min(least, sample.uploads);
    }
    return least;
  }
  // Per-frame wall times of the last drive().
  [[nodiscard]] std::vector<double> lastWallMs() const {
    std::vector<double> wall;
    for (const Sample& sample : samples_) {
      wall.push_back(sample.wall_ms);
    }
    return wall;
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
    if (qEnvironmentVariableIntValue("PJ_S3D_BENCH_NO_HDR") == 1) {
      SceneViewTestAccess::forceStageFallback(
          view(), SceneViewTestAccess::Stage::kHdr, true);  // attribution runs: the direct pass, no HDR chain
    }
    if (qEnvironmentVariableIntValue("PJ_S3D_BENCH_NO_SSAO") == 1) {
      view().compositeParams().ssao_enabled = false;
    }
    if (qEnvironmentVariableIntValue("PJ_S3D_BENCH_NO_EDL") == 1) {
      view().compositeParams().edl_enabled = false;
    }
    if (qEnvironmentVariableIntValue("PJ_S3D_BENCH_NO_SHADOWS") == 1) {
      pj::scene3d::MeshShadingParams params = view().meshShadingParams();
      params.shadows_enabled = false;
      view().setMeshShadingParams(params);
    }
    dock().setFixedFrame(QString::fromLatin1(kFrame));
    dock().onTrackerTime(100.0e-9);  // every scene starts at t = 100 ns
    pj_bench::resetPeakRss();
    first_show_sample_ = measure([this] { fixture_.window().show(); }, lifecycle());
    if (first_show_sample_.wall_ms < 0.0) {
      return false;
    }
    window_begin_ns_ = first_show_sample_.begin_ns;
    window_end_ns_ = first_show_sample_.end_ns;
    first_show_ms_ = first_show_sample_.wall_ms;
    settle();
    camera_.focal = {0.0F, 0.0F, 0.5F};
    camera_.radius = 22.0F;
    camera_.azimuth = glm::radians(-60.0F);
    camera_.elevation = glm::radians(35.0F);
    azimuth0_ = camera_.azimuth;
    view().camera().adoptState(camera_);
    return true;
  }
  // Lets queued decodes and uploads land: returns once no frame came for 1 s.
  void settle() {
    (void)pj_bench::idleFrames([this] { return finished(); }, 1000, 15000, 0);
  }

  // Puts the orbit camera at driven frame `k` (see drive()): k + 1 degrees of
  // azimuth, and -2 / -1 degrees alternately while warming up, so the last
  // frame does not depend on the warm-up's length.
  void orbit(int k) {
    camera_.azimuth = azimuth0_ + glm::radians(static_cast<float>(k >= 0 ? k + 1 : k % 2 - 1));
    view().camera().adoptState(camera_);
  }
  // Pans the camera focal by `dy` metres along +Y (from wherever follow left it).
  void pan(float dy) {
    camera_ = view().camera().state();
    camera_.focal.y += dy;
    view().camera().adoptState(camera_);
  }

  // How measure() splits a sample into stages, given the view's work before it.
  using StageSplit = std::function<Stages(const Sample&, const SceneViewWork&)>;
  // The split of a lifecycle operation (a dock move, the first show).
  StageSplit lifecycle() {
    return [this](const Sample& sample, const SceneViewWork& before) { return lifecycleStages(sample, before); };
  }

  // Runs `action` and waits until the view finished a frame after it: the
  // sample of that span (wall ms negative on timeout), split by `split`.
  Sample measure(const std::function<void()>& action, const StageSplit& split) {
    Sample sample;
    const std::uint64_t before = finished();
    const long long faults = pj_bench::minorFaults();
    const SceneViewWork work = pj_bench::viewWork(view());
    const double cpu = pj_bench::threadCpuMs();
    sample.begin_ns = pj_bench::steadyNs();
    action();
    const bool done = pj_bench::waitUntil([&] { return finished() > before; });
    sample.end_ns = pj_bench::steadyNs();
    sample.wall_ms = done ? static_cast<double>(sample.end_ns - sample.begin_ns) / 1.0e6 : -1.0;
    sample.cpu_ms = pj_bench::threadCpuMs() - cpu;
    sample.faults = static_cast<double>(pj_bench::minorFaults() - faults);
    noteWork(sample, work);
    sample.stages = split(sample, work);
    return sample;
  }

  // Runs `step`, requests a frame and waits until one is finished: its sample
  // (wall ms from `step` to that frame, negative on timeout).
  Sample frame(const std::function<void()>& step) {
    std::int64_t requested_ns = 0;
    Sample sample = measure(
        [&] {
          step();
          requested_ns = pj_bench::steadyNs();
          view().invalidateContent();
        },
        [&](const Sample& measured, const SceneViewWork& /*before*/) {
          return stagesSince(measured.begin_ns, requested_ns);
        });
    sample.gpu_ms = lastGpuMs();
    sample.gpu_mhz = pj_bench::gpuMhz();
    return sample;
  }

  // A warm-up (pj_bench::WarmUp), then `frames` measured frames of
  // `step(k)`, k = 0, 1, ...; the warm-up frames run step(-1), step(-2), ...,
  // which must request a frame that differs from the previous one and from
  // step(0) without consuming data meant for the measured frames (their
  // number varies). `ingest(k)` runs before each measured frame, outside its
  // wall, CPU and faults, and is reported apart (ingest_ms, ingest_cpu_ms).
  void drive(int frames, const std::function<void(int)>& step, const std::function<void(int)>& ingest = {}) {
    samples_.clear();
    ingest_wall_ms_.clear();
    ingest_cpu_ms_ = 0.0;
    pj_bench::WarmUp warm_up;
    for (int w = 0;; ++w) {
      const Sample sample = frame([&] { step(-1 - w); });
      ASSERT_GE(sample.wall_ms, 0.0) << "warm-up frame " << w << " never finished";
      if (warm_up.done(sample.wall_ms)) {
        break;
      }
    }
    cold_ms_ = warm_up.coldMs();
    warmup_frames_ = warm_up.frames();
    warmup_ms_ = warm_up.elapsedMs();
    warmup_gpu_mhz_ = warm_up.gpuMhzAtEnd();
    window_begin_ns_ = pj_bench::steadyNs();
    for (int k = 0; k < frames; ++k) {
      if (ingest) {
        const double cpu = pj_bench::threadCpuMs();
        const std::int64_t begin_ns = pj_bench::steadyNs();
        ingest(k);
        ingest_wall_ms_.push_back(static_cast<double>(pj_bench::steadyNs() - begin_ns) / 1.0e6);
        ingest_cpu_ms_ += (pj_bench::threadCpuMs() - cpu) / frames;
      }
      const Sample sample = frame([&] { step(k); });
      ASSERT_GE(sample.wall_ms, 0.0) << "frame " << k << " never finished";
      samples_.push_back(sample);
    }
    window_end_ns_ = pj_bench::steadyNs();
  }

  // Float (reparent to a new top-level), redock and tab-switch (hide, then
  // show) the dock `moves` times, each timed to its first finished frame; the
  // medians as "float_ms=.. redock_ms=.. tab_ms=..", then each move's first
  // frame split into stages ("float_stages_ms=..").
  std::string dockMoves(int moves) {
    std::array<std::vector<Sample>, 3> kinds;  // float, redock, tab
    PJ::Scene3DDockWidget* moved = &dock();
    QLayout* layout = fixture_.window().layout();
    window_begin_ns_ = pj_bench::steadyNs();
    const auto timed = [this](const std::function<void()>& action) {
      const Sample sample = measure(action, lifecycle());
      settle();
      return sample;
    };
    for (int i = 0; i < moves; ++i) {
      kinds[0].push_back(timed([&] {
        moved->setParent(nullptr);
        moved->resize(fixture_.window().size());
        moved->show();
      }));
      kinds[1].push_back(timed([&] {
        layout->addWidget(moved);
        moved->show();
      }));
      moved->hide();
      QCoreApplication::processEvents();
      kinds[2].push_back(timed([&] { moved->show(); }));
    }
    window_end_ns_ = pj_bench::steadyNs();
    std::string out;
    const char* names[] = {"float", "redock", "tab"};
    for (std::size_t kind = 0; kind < kinds.size(); ++kind) {
      out += std::string(out.empty() ? "" : " ") + names[kind] + "_ms=" + fmtMs(median(kinds[kind], &Sample::wall_ms));
    }
    for (std::size_t kind = 0; kind < kinds.size(); ++kind) {
      out += std::string(" ") + names[kind] + "_faults=" + fmtMs(median(kinds[kind], &Sample::faults), 0);
      out += std::string(" ") + names[kind] + "_inits=" + fmtMs(median(kinds[kind], &Sample::inits), 0);
      out += std::string(" ") + names[kind] + "_uploads=" + fmtMs(median(kinds[kind], &Sample::uploads), 0);
      out += std::string(" ") + names[kind] + "_upload_mib=" + fmtMs(median(kinds[kind], &Sample::upload_mib), 1);
      out += std::string(" ") + names[kind] + "_paints=" + fmtMs(median(kinds[kind], &Sample::paints), 0);
      out += std::string(" ") + names[kind] + "_renders=" + fmtMs(median(kinds[kind], &Sample::renders), 0);
      out += std::string(" ") + names[kind] + "_skips=" + fmtMs(median(kinds[kind], &Sample::skips), 0);
      out += stagesField((std::string(names[kind]) + "_stages_ms").c_str(), kinds[kind]);
    }
    samples_.clear();
    for (const auto& kind : kinds) {
      samples_.insert(samples_.end(), kind.begin(), kind.end());
    }
    return out;
  }

  // Splits a first frame (show() or a dock move) that began at `begin_ns`:
  // the extra of the first-show case.
  [[nodiscard]] std::string firstFrameStages() const {
    return stagesField("first_stages_ms", {first_show_sample_}) +
           " first_paints=" + fmtMs(first_show_sample_.paints, 0) +
           " first_renders=" + fmtMs(first_show_sample_.renders, 0) +
           " first_skips=" + fmtMs(first_show_sample_.skips, 0);
  }

  // Prints the case line: the last drive()'s numbers, the probes, then `extra`,
  // after counting the settled view's idle frames (asserted 0).
  void report(const char* name, const std::string& extra = {}) {
    if (const QString dir = qEnvironmentVariable("PJ_S3D_BENCH_DUMP_DIR"); !dir.isEmpty()) {
      pj::scene3d::test::grab(view()).save(
          dir + u'/' + QString::fromLatin1(name) + u'_' + QString::fromLatin1(kBuild) + u".png");
    }
    const std::uint64_t idle_frames = pj_bench::idleFrames([this] { return renders(); });
    const pj_bench::Stats wall = pj_bench::stats(column(samples_, &Sample::wall_ms));
    const pj_bench::Stats cpu = pj_bench::stats(column(samples_, &Sample::cpu_ms));
    double cpu_mean = 0.0;
    for (const Sample& sample : samples_) {
      cpu_mean += sample.cpu_ms / static_cast<double>(samples_.size());
    }
    writeCsv(name);
    QRhi* rhi = pj_bench::rhiOf(view());
    const double vma = pj_bench::rhiAllocatorMib(rhi);
    const QString backend = view().rhiBackendName();
    const QString device = rhi != nullptr ? QString::fromUtf8(rhi->driverInfo().deviceName) : QString();
    std::string stages = stagesField("stages_ms", samples_);
    if (!samples_.empty() && samples_.front().gpu_mhz >= 0.0) {
      const pj_bench::Stats mhz = pj_bench::stats(column(samples_, &Sample::gpu_mhz));
      stages += " gpu_mhz_range=" + fmtMs(mhz.min, 0) + "-" + fmtMs(mhz.max, 0);
    }
    if (!samples_.empty() && samples_.front().gpu_ms > 0.0) {
      stages += " gpu_ms=" + fmtMs(median(samples_, &Sample::gpu_ms));
    }
    std::printf(
        "S3D_BENCH case=%s build=%s backend=%s device=\"%s\" frames=%zu wall_ms=%.2f wall_min=%.2f wall_p90=%.2f "
        "wall_max=%.2f cpu_ms=%.2f cpu_p50=%.2f cpu_p90=%.2f faults=%.0f uploads=%.0f upload_mib=%.1f ingest_ms=%.2f "
        "ingest_cpu_ms=%.2f rss_mib=%s "
        "hwm_mib=%s gpu_drm_mib=%s gpu_vma_mib=%s idle_frames=%llu first_show_ms=%.1f cpu_mhz=%s cold_ms=%.2f "
        "warmup_frames=%d warmup_ms=%.0f gpu_mhz=%s window_ns=%lld-%lld%s%s%s\n",
        name, kBuild, qPrintable(backend), qPrintable(device), samples_.size(), wall.median, wall.min, wall.p90,
        wall.max, cpu_mean, cpu.median, cpu.p90, median(samples_, &Sample::faults), median(samples_, &Sample::uploads),
        median(samples_, &Sample::upload_mib), pj_bench::stats(ingest_wall_ms_).median, ingest_cpu_ms_,
        pj_bench::fmt(pj_bench::rssMib()).constData(), pj_bench::fmt(pj_bench::peakRssMib()).constData(),
        pj_bench::fmt(pj_bench::drmTotalMib()).constData(), pj_bench::fmt(vma).constData(),
        static_cast<unsigned long long>(idle_frames), first_show_ms_, pj_bench::fmt(pj_bench::cpuMhz(), 0).constData(),
        cold_ms_, warmup_frames_, warmup_ms_, pj_bench::fmt(warmup_gpu_mhz_, 0).constData(),
        static_cast<long long>(window_begin_ns_), static_cast<long long>(window_end_ns_), stages.c_str(),
        extra.empty() ? "" : " ", extra.c_str());
    std::fflush(stdout);
    EXPECT_EQ(idle_frames, 0U) << "a settled, unchanged view kept rendering";
  }

 private:
  static double median(const std::vector<Sample>& samples, double Sample::* field) {
    return pj_bench::stats(column(samples, field)).median;
  }
  static std::vector<double> column(const std::vector<Sample>& samples, double Sample::* field) {
    std::vector<double> values;
    values.reserve(samples.size());
    for (const Sample& sample : samples) {
      values.push_back(sample.*field);
    }
    return values;
  }
  static std::string fmtMs(double value, int decimals = 2) {
    return pj_bench::fmt(value, decimals).toStdString();
  }
  // " <name>=step:..,sched:..,.." of the per-stage medians over `samples`, or
  // nothing where the view has no stage stamps (no PJ_RHI_TEST_HOOKS).
  static std::string stagesField(const char* name, const std::vector<Sample>& samples) {
    if (samples.empty() || samples.front().stages[kPresent] < 0.0) {
      return {};
    }
    std::string out = std::string(" ") + name + "=";
    for (std::size_t stage = 0; stage < kStageCount; ++stage) {
      std::vector<double> values;
      for (const Sample& sample : samples) {
        values.push_back(sample.stages[stage]);
      }
      out += std::string(stage == 0 ? "" : ",") + kStageNames[stage] + ":" +
             fmtMs(pj_bench::stats(std::move(values)).median);
    }
    return out;
  }
  // PJ_S3D_BENCH_CSV_DIR/<name>_<build>.csv: one row per measured sample (the
  // first-show sample when no frame was driven).
  void writeCsv(const char* name) const {
    const QString dir = qEnvironmentVariable("PJ_S3D_BENCH_CSV_DIR");
    if (dir.isEmpty()) {
      return;
    }
    QFile file(dir + u'/' + QString::fromLatin1(name) + u'_' + QString::fromLatin1(kBuild) + u".csv");
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      return;
    }
    QTextStream csv(&file);
    csv << "frame,begin_ns,end_ns,wall_ms,cpu_ms,faults,uploads,upload_mib,inits,gpu_ms,gpu_mhz";
    for (const char* stage : kStageNames) {
      csv << ',' << stage << "_ms";
    }
    csv << '\n';
    const std::vector<Sample> rows = samples_.empty() ? std::vector<Sample>{first_show_sample_} : samples_;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const Sample& sample = rows[i];
      csv << i << ',' << sample.begin_ns << ',' << sample.end_ns << ',' << sample.wall_ms << ',' << sample.cpu_ms << ','
          << sample.faults << ',' << sample.uploads << ',' << sample.upload_mib << ',' << sample.inits << ','
          << sample.gpu_ms << ',' << sample.gpu_mhz;
      for (const double stage : sample.stages) {
        csv << ',' << stage;
      }
      csv << '\n';
    }
  }

  // Adds the upload work done since `before` to `sample`.
  void noteWork(Sample& sample, const SceneViewWork& before) {
    const SceneViewWork now = pj_bench::viewWork(view());
    sample.uploads = now.uploads - before.uploads;
    sample.upload_mib = now.upload_mib - before.upload_mib;
    sample.inits = now.inits - before.inits;
    sample.paints = now.paints - before.paints;
    sample.renders = now.renders - before.renders;
    sample.skips = now.skips - before.skips;
  }

#ifdef PJ_RHI_TEST_HOOKS
  // The split of a lifecycle operation (`sample`, work counted from `before`):
  // the stage sums of its full renders, present from the last one to the
  // finished frame, init its base builds, sched the rest of its wall (init
  // included); every stage -1 when it drew no full frame.
  Stages lifecycleStages(const Sample& sample, const SceneViewWork& before) {
    Stages stages;
    stages.fill(-1.0);
    const SceneViewWork now = pj_bench::viewWork(view());
    const auto& t = SceneViewTestAccess::stageTimes(view());
    const std::int64_t finished_ns = counter_->lastFinishedNs();
    if (now.full_renders <= before.full_renders || sample.wall_ms < 0.0 || finished_ns < t.render_end_ns) {
      return stages;
    }
    stages[kStep] = 0.0;
    stages[kTargets] = now.targets_ms - before.targets_ms;
    stages[kAdmit] = now.admit_ms - before.admit_ms;
    stages[kPrepare] = now.prepare_ms - before.prepare_ms;
    stages[kSubmit] = now.submit_ms - before.submit_ms;
    stages[kPresent] = static_cast<double>(finished_ns - t.render_end_ns) / 1.0e6;
    stages[kInit] = now.init_ms - before.init_ms;
    stages[kSched] =
        sample.wall_ms - stages[kTargets] - stages[kAdmit] - stages[kPrepare] - stages[kSubmit] - stages[kPresent];
    return stages;
  }
  [[nodiscard]] double lastGpuMs() {
    const double ms = SceneViewTestAccess::stageTimes(view()).last_gpu_ms;
    return ms > 0.0 ? ms : -1.0;
  }
  // The stage split of the frame finished last, requested at `requested_ns`
  // after a step that began at `begin_ns`; every stage -1 without stamps.
  Stages stagesSince(std::int64_t begin_ns, std::int64_t requested_ns) {
    Stages stages;
    stages.fill(-1.0);
    const auto& t = SceneViewTestAccess::stageTimes(view());
    const std::int64_t finished_ns = counter_->lastFinishedNs();
    if (t.render_begin_ns < requested_ns || t.render_end_ns < t.render_begin_ns || finished_ns < t.render_end_ns) {
      return stages;  // no render() of this request reached the stamps (the Show skip, a failure)
    }
    const auto ms = [](std::int64_t from, std::int64_t to) { return static_cast<double>(to - from) / 1.0e6; };
    stages[kStep] = ms(begin_ns, requested_ns);
    stages[kSched] = ms(requested_ns, t.render_begin_ns);
    stages[kTargets] = ms(t.render_begin_ns, t.targets_ready_ns);
    stages[kAdmit] = ms(t.targets_ready_ns, t.admitted_ns);
    stages[kPrepare] = ms(t.admitted_ns, t.prepared_ns);
    stages[kSubmit] = ms(t.prepared_ns, t.render_end_ns);
    stages[kPresent] = ms(t.render_end_ns, finished_ns);
    stages[kInit] = 0.0;
    return stages;
  }
#else
  [[nodiscard]] double lastGpuMs() {
    return -1.0;
  }
  Stages lifecycleStages(const Sample& /*sample*/, const SceneViewWork& /*before*/) {
    Stages stages;
    stages.fill(-1.0);
    return stages;
  }
  Stages stagesSince(std::int64_t /*begin_ns*/, std::int64_t /*requested_ns*/) {
    Stages stages;
    stages.fill(-1.0);
    return stages;
  }
#endif

  pj::scene3d::test::Scene3DRenderFixture fixture_;
  QTemporaryDir urdf_dir_;
  std::unique_ptr<pj_bench::FrameCounter> counter_;
  pj::scene3d::CameraState camera_;
  std::vector<Sample> samples_;
  std::vector<double> ingest_wall_ms_;
  double ingest_cpu_ms_ = 0.0;
  double first_show_ms_ = 0.0;
  Sample first_show_sample_;
  float azimuth0_ = 0.0F;  // the camera's azimuth after show(); orbit() is relative to it
  double cold_ms_ = 0.0;   // the last drive()'s first warm-up frame
  int warmup_frames_ = 0;
  double warmup_ms_ = 0.0;
  double warmup_gpu_mhz_ = -1.0;
  // The measured span of the last drive(), dockMoves() or show() (steadyNs()):
  // the window of a `perf record -k CLOCK_MONOTONIC` profile.
  std::int64_t window_begin_ns_ = 0;
  std::int64_t window_end_ns_ = 0;
};

// What a case's last frame must have drawn, so a run whose cloud, markers or
// model were never admitted fails instead of timing a near-empty frame.
struct Workload {
  bool points = false;
  bool markers = false;  // instanced primitives and line strips
  bool models = false;
};

void expectDrawn(SceneViewWidget& view, Workload workload) {
  if (workload.points) {
    EXPECT_GT(SceneViewTestAccess::pointPass(view).lastPointVertexCount(), 0) << "no point cloud drawn";
  }
  if (workload.markers) {
    EXPECT_GT(SceneViewTestAccess::markerPass(view).lastInstanceCount(), 0) << "no marker drawn";
  }
  if (workload.models) {
    EXPECT_GT(SceneViewTestAccess::modelPass(view).lastDrawCount(), 0) << "no model drawn";
  }
}

// What the last frame drew, so a run that silently drew nothing shows up.
std::string drawn(SceneViewWidget& view) {
  return "points=" + std::to_string(SceneViewTestAccess::pointPass(view).lastPointVertexCount()) +
         " markers=" + std::to_string(SceneViewTestAccess::markerPass(view).lastInstanceCount()) +
         " models=" + std::to_string(SceneViewTestAccess::modelPass(view).lastDrawCount()) + " hdr=" +
         std::to_string(
             static_cast<int>(SceneViewTestAccess::stage(view, SceneViewTestAccess::Stage::kHdr).last_active)) +
         " shadow_draws=" + std::to_string(SceneViewTestAccess::modelPass(view).lastShadowDrawCount());
}

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
  ASSERT_NO_FATAL_FAILURE(bench.drive(benchFrames(), [&](int k) { bench.orbit(k); }));
  expectDrawn(bench.view(), {.points = true});
  bench.report("cloud_static", drawn(bench.view()));
}

// A new cloud every frame (live streaming): pushed untimed, then the tracker
// moves onto it. The warm-up alternates between two clouds already stored, so
// the store holds the same samples whatever the warm-up's length.
TEST_F(Scene3dFrameBenchmark, CloudStream) {
  SceneBench bench;
  const long long points = benchSize("STREAM_POINTS", 5'000, 300'000);
  const std::vector<std::vector<uint8_t>> clouds{randomCloud(points, 1), randomCloud(points, 2)};
  const PJ::ObjectTopicId id = bench.fixture().add("/points", BT::kPointCloud, clouds[0]);
  constexpr PJ::Timestamp kWarmSampleNs = 100 + kStepNs / 2;
  bench.fixture().push(id, kWarmSampleNs, clouds[1]);
  SHOW_OR_SKIP(bench);
  const auto time_of = [](int k) {
    return k < 0 ? (k % 2 != 0 ? kWarmSampleNs : 100) : 100 + static_cast<PJ::Timestamp>(k + 1) * kStepNs;
  };
  ASSERT_NO_FATAL_FAILURE(bench.drive(
      benchFrames(), [&](int k) { bench.dock().onTrackerTime(static_cast<double>(time_of(k)) / 1.0e9); },
      [&](int k) { bench.fixture().push(id, time_of(k), clouds[static_cast<std::size_t>(k % 2)]); }));
  expectDrawn(bench.view(), {.points = true});
  EXPECT_GE(bench.minUploads(), 1.0) << "a measured frame did not upload its new cloud";
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
  ASSERT_NO_FATAL_FAILURE(bench.drive(benchFrames(), [&](int k) { bench.orbit(k); }));
  expectDrawn(bench.view(), {.markers = true});
  EXPECT_GT(SceneViewTestAccess::markerPass(bench.view()).lastStreamVertexCount(), 0) << "no line strip drawn";
  bench.report("cubes_lines_orbit", drawn(bench.view()));
}

// Many sphere markers while the camera pans and follows a TF frame that moves
// every frame.
TEST_F(Scene3dFrameBenchmark, MarkersPanFollow) {
  SceneBench bench;
  const long long marker_count = benchSize("MARKERS", 100, 10'000);
  const int frames = benchFrames();
  const auto time_of = [](int k) { return 100 + static_cast<PJ::Timestamp>(k) * kStepNs; };
  // Warm-up frames alternate the tracker between the first TF sample and half
  // way to the second (the follow frame moves, the camera does not pan).
  const auto tracker_of = [&](int k) { return k < 0 ? time_of(0) + (k % 2 != 0 ? kStepNs / 2 : 0) : time_of(k + 1); };
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
  for (int k = 1; k <= frames; ++k) {
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
  float followed_x = 0.0F;
  ASSERT_NO_FATAL_FAILURE(bench.drive(frames, [&](int k) {
    bench.dock().onTrackerTime(static_cast<double>(tracker_of(k)) / 1.0e9);
    if (k == 0) {
      followed_x = bench.view().camera().state().focal.x;
    }
    if (k >= 0) {
      bench.pan(0.01F);
    }
  }));
  expectDrawn(bench.view(), {.markers = true});
  // base moves +0.02 m in X per TF sample; the pans move only Y.
  EXPECT_GT(bench.view().camera().state().focal.x, followed_x) << "the followed TF frame never advanced";
  bench.report("markers_pan_follow", drawn(bench.view()));
}

// A URDF robot whose every link TF moves each frame; the model never reloads.
TEST_F(Scene3dFrameBenchmark, RobotTfAnimated) {
  SceneBench bench;
  const int links = robotLinks();
  const int frames = benchFrames();
  bench.addRobotTf(links, frames + 1);
  ASSERT_NO_FATAL_FAILURE(bench.addRobot(links));
  SHOW_OR_SKIP(bench);
  const QImage first = pj::scene3d::test::grab(bench.view());
  // Warm-up frames alternate between the first TF sample and half way to the second.
  ASSERT_NO_FATAL_FAILURE(bench.drive(frames, [&](int k) {
    const PJ::Timestamp t =
        k < 0 ? 100 + (k % 2 != 0 ? kStepNs / 2 : 0) : 100 + static_cast<PJ::Timestamp>(k + 1) * kStepNs;
    bench.dock().onTrackerTime(static_cast<double>(t) / 1.0e9);
  }));
  expectDrawn(bench.view(), {.models = true});
  // The camera stays put: only the links' TF can change the frame.
  EXPECT_TRUE(pj::scene3d::test::grab(bench.view()) != first) << "the robot's TF never advanced";
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
  ASSERT_NO_FATAL_FAILURE(bench.drive(benchFrames(), [&](int k) { bench.orbit(k); }));
  expectDrawn(bench.view(), {.points = true});
  EXPECT_EQ(SceneViewTestAccess::pointPass(bench.view()).lastPointLayerCount(), count) << "a cloud layer was not drawn";
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
// a hide/show on the same QRhi (what hiding a workspace tab or, with our ADS
// fork, a dock-area tab does; mixed_layout_benchmark --tab-switch runs a real one).
TEST_F(Scene3dFrameBenchmark, DockMovesCloud) {
  SceneBench bench;
  bench.fixture().add("/points", BT::kPointCloud, randomCloud(cloudPoints(), 1));
  SHOW_OR_SKIP(bench);
  const std::string moves = bench.dockMoves(static_cast<int>(benchSize("MOVES", 2, 5)));
  expectDrawn(bench.view(), {.points = true});
  bench.report("dock_moves_cloud", moves + " " + drawn(bench.view()));
}
TEST_F(Scene3dFrameBenchmark, DockMovesRobot) {
  SceneBench bench;
  const int links = robotLinks();
  bench.addRobotTf(links, 1);
  ASSERT_NO_FATAL_FAILURE(bench.addRobot(links));
  SHOW_OR_SKIP(bench);
  const std::string moves = bench.dockMoves(static_cast<int>(benchSize("MOVES", 2, 5)));
  expectDrawn(bench.view(), {.models = true});
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
  expectDrawn(bench.view(), {.points = true, .models = true});
  std::string extra = drawn(bench.view()) + bench.firstFrameStages();
  if (QRhi* rhi = pj_bench::rhiOf(bench.view()); rhi != nullptr) {
    extra += " pipeline_ms=" + std::to_string(rhi->statistics().totalPipelineCreationTime);
  }
  bench.report("first_show", extra);
}

// The HDR chain's depth replay (every draw re-submitted into the single-sample
// depth target, forced by SceneViewTestAccess::setForceDepthReplay()) against the default
// chain (the multisample depth resolve where the backend has one; the replay
// elsewhere), in alternating blocks on one large cloud.
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
    SceneViewTestAccess::setForceDepthReplay(bench.view(), forced);
    ASSERT_NO_FATAL_FAILURE(bench.drive(frames, [&](int k) { bench.orbit(k); }));
    (forced ? replay : chain).push_back(pj_bench::stats(bench.lastWallMs()).median);
    resolves = resolves || SceneViewTestAccess::depthResolveActive(bench.view());
  }
  SceneViewTestAccess::setForceDepthReplay(bench.view(), false);
  expectDrawn(bench.view(), {.points = true});
  bench.report(
      "depth_replay", "replay_ms=" + std::to_string(pj_bench::stats(replay).median) +
                          " default_ms=" + std::to_string(pj_bench::stats(chain).median) +
                          " resolves=" + std::to_string(static_cast<int>(resolves)) + " " + drawn(bench.view()));
}

}  // namespace
