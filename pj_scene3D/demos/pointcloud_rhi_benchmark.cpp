// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Point-cloud GPU/CPU benchmark for the QRhi point pass. It
// drives the production PointRhiPass on an offscreen QRhi, the way the view does
// with its quality chain on: the colour pass into a 4x RGBA16F + D32F target, then
// the single-sample RGBA8 + D32F depth replay SSAO/EDL read (their full-screen
// passes cost the same for every topology, so they are left out). It compares the
// instanced quads against the point-list fast path, for three clouds: a static
// one; a changing one, the same array re-uploaded every frame; and a streaming one,
// a new sample every frame the layer's way: a wire PointCloud (x/y/z of
// --field-type, a float32 intensity) packed the layer's way: packPointVertices()
// on the thread pool, into the retained PointVertex array (rewritten in place once
// the upload released it) after the frame began, which the pass uploads shared.
//
//   pointcloud_rhi_benchmark [--api opengl|vulkan] [--points N] [--frames F]
//                            [--shape point|sphere|cube] [--size WxH]
//                            [--replay on|off] [--field-type u16|u32|f32]
//
// Prints the backend + adapter, then per mode, after a warm-up (pj_bench::WarmUp),
// the medians of: GPU frame time (QRhi timestamps); the CPU wall of prepare
// (admit + prepare), of enqueue
// (recording the passes) and of endFrame (submission, the backend's uploads and,
// offscreen, the wait for the GPU) with endFrame's own-thread CPU time; whole-frame
// wall time; and, for a streaming cloud, the conversion, a strided min/max scan of
// the wire x/y/z (what bounds would cost a path uploading the wire bytes as-is)
// and the decode-to-visible latency (conversion start to the frame completed).
// Then the backend's GPU memory in use where it reports one (Vulkan), and the
// process's peak RSS.

#include <rhi/qrhi.h>

#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QOffscreenSurface>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#include <QVulkanInstance>
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/point_cloud.hpp"
#include "pj_bench_probes.h"
#include "pj_scene3d_core/pointcloud_convert.h"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"  // parallelForOnPool
#include "pj_scene3d_widgets/point_renderable.h"
#include "pj_scene3d_widgets/retained_upload.h"
#include "rhi/passes/point_rhi_pass.h"
#include "rhi/rhi_scene_pass.h"

namespace {

using pj::scene3d::AABB;
using pj::scene3d::PointRenderable;
using pj::scene3d::PointVertex;

using DT = PJ::sdk::PointField::Datatype;

// The wire cloud: x/y/z of one datatype, then a float32 intensity. Integer clouds
// hold the float cloud scaled by kIntegerScale and shifted positive, under a camera
// scaled alike. They exist for the conversion and scan columns: their GPU times are
// not comparable with the float cloud's (an integer lattice rasterizes differently).
struct WireCloud {
  static constexpr float kIntegerScale = 100.0F;
  static constexpr float kIntegerShift = 500.0F;
  std::vector<std::uint8_t> bytes;
  PJ::sdk::PointCloud cloud;
  float scale = 1.0F;
  float shift = 0.0F;

  WireCloud(std::size_t count, DT type) {
    const std::uint32_t component = PJ::sdk::bytesPerElement(type);
    const std::uint32_t intensity_offset = std::max(3U * component, 8U);
    const std::uint32_t step = intensity_offset + 4U;
    if (type != DT::kFloat32) {
      scale = kIntegerScale;
      shift = kIntegerShift;
    }
    bytes.resize(count * step);
    std::mt19937 random(7);
    std::uniform_real_distribution<float> xy(-5.0F, 5.0F);
    std::uniform_real_distribution<float> z(0.0F, 2.0F);
    for (std::size_t index = 0; index < count; ++index) {
      std::uint8_t* point = bytes.data() + index * step;
      const float position[3] = {xy(random), xy(random), z(random)};
      for (std::uint32_t axis = 0; axis < 3; ++axis) {
        const float value = position[axis] * scale + (axis < 2 ? shift : 0.0F);
        if (type == DT::kUint16) {
          const auto stored = static_cast<std::uint16_t>(value);
          std::memcpy(point + axis * component, &stored, component);
        } else if (type == DT::kUint32) {
          const auto stored = static_cast<std::uint32_t>(value);
          std::memcpy(point + axis * component, &stored, component);
        } else {
          std::memcpy(point + axis * component, &value, component);
        }
      }
      const float intensity = position[2] * 0.5F;
      std::memcpy(point + intensity_offset, &intensity, 4);
    }
    cloud.width = static_cast<std::uint32_t>(count);
    cloud.point_step = step;
    cloud.row_step = step * cloud.width;
    cloud.frame_id = "map";
    cloud.fields = {
        {"x", 0, type, 1},
        {"y", component, type, 1},
        {"z", 2 * component, type, 1},
        {"intensity", intensity_offset, DT::kFloat32, 1}};
    cloud.data = PJ::Span<const std::uint8_t>(bytes.data(), bytes.size());
  }
};

// Finite min/max of the wire x/y/z, read strided in their own datatype.
template <typename T>
AABB scanXyz(const PJ::sdk::PointCloud& cloud) {
  const std::uint32_t component = sizeof(T);
  AABB bounds;
  const std::uint8_t* point = cloud.data.data();
  for (std::uint32_t index = 0; index < cloud.width; ++index, point += cloud.point_step) {
    T xyz[3];
    std::memcpy(xyz, point, 3 * component);
    const glm::vec3 position(static_cast<float>(xyz[0]), static_cast<float>(xyz[1]), static_cast<float>(xyz[2]));
    if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)) {
      pj::scene3d::expandAABB(bounds, position);
    }
  }
  return bounds;
}

AABB scanXyz(const PJ::sdk::PointCloud& cloud) {
  switch (cloud.fields[0].datatype) {
    case DT::kUint16:
      return scanXyz<std::uint16_t>(cloud);
    case DT::kUint32:
      return scanXyz<std::uint32_t>(cloud);
    default:
      return scanXyz<float>(cloud);
  }
}

// A cloud the pass reads like a layer's: its vertex array and conversion bounds.
class BenchCloud : public PointRenderable {
 public:
  explicit BenchCloud(pj::scene3d::PointShape shape) : shape_(shape) {}
  // The next prepare() uploads the same array again.
  void touch() {
    ++revision_;
  }
  // Converts `wire` the layer's way; the next prepare() uploads it.
  void convert(const PJ::sdk::PointCloud& wire) {
    // kField by "intensity": the layer stores the scalar (16-byte records).
    const std::optional<pj::scene3d::PointVertexPlan> plan =
        pj::scene3d::planPointVertices(wire, "intensity", false, true);
    if (!plan.has_value()) {
      return;
    }
    format_ = plan->format;
    pj::scene3d::PackedPointVertices packed;
    pj::scene3d::packIntoRetained(
        bytes_, static_cast<qsizetype>(plan->maxBytes()), [&](std::span<std::byte> out, bool reused) {
          packed = pj::scene3d::packPointVertices(
              wire, *plan, out, pj::scene3d::parallelForOnPool,
              reused ? pj::scene3d::kMaxPackChunks : pj::scene3d::kMaxFreshPackChunks);
          return static_cast<qsizetype>(packed.count * pj::scene3d::pointVertexStride(packed.format));
        });
    count_ = packed.count;
    bounds_ = packed.bounds;
    ++revision_;
  }
  bool prepareForRender(std::uint64_t /*remaining*/) override {
    return true;
  }
  [[nodiscard]] const QByteArray& vertexBytes() const override {
    return bytes_;
  }
  [[nodiscard]] pj::scene3d::PointVertexFormat vertexFormat() const override {
    return format_;
  }
  [[nodiscard]] std::size_t pointCount() const override {
    return count_;
  }
  [[nodiscard]] std::uint64_t geometryRevision() const override {
    return revision_;
  }
  [[nodiscard]] const std::string& renderFrameId() const override {
    return frame_;
  }
  [[nodiscard]] std::optional<AABB> sourceBounds() const override {
    return bounds_;
  }
  [[nodiscard]] bool visible() const override {
    return true;
  }
  [[nodiscard]] pj::scene3d::PointShape shape() const override {
    return shape_;
  }
  [[nodiscard]] pj::scene3d::PointColorType colorType() const override {
    return pj::scene3d::PointColorType::kField;
  }
  [[nodiscard]] float sizeMeters() const override {
    return 0.02F;
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
    return -1;
  }
  [[nodiscard]] std::pair<float, float> scalarRange(const glm::mat4& /*fixed_from_source*/) const override {
    return {0.0F, 1.0F};
  }
  void noteRenderedScalarRange(float /*minimum*/, float /*maximum*/) override {}
  [[nodiscard]] bool isDepthCloud() const override {
    return false;
  }

 private:
  QByteArray bytes_;
  std::size_t count_ = 0;
  pj::scene3d::PointVertexFormat format_ = pj::scene3d::PointVertexFormat::kXyzScalar;
  AABB bounds_;
  std::uint64_t revision_ = 0;
  std::string frame_ = "map";
  pj::scene3d::PointShape shape_;
};

struct Targets {
  QRhiRenderBuffer* hdr_color = nullptr;
  QRhiRenderBuffer* hdr_depth = nullptr;
  QRhiTexture* hdr_resolve = nullptr;
  QRhiTextureRenderTarget* hdr = nullptr;
  QRhiRenderPassDescriptor* hdr_pass = nullptr;
  QRhiTexture* replay_color = nullptr;
  QRhiTexture* replay_depth = nullptr;
  QRhiTextureRenderTarget* replay = nullptr;
  QRhiRenderPassDescriptor* replay_pass = nullptr;

  bool create(QRhi* rhi, QSize size) {
    hdr_color = rhi->newRenderBuffer(QRhiRenderBuffer::Color, size, 4, {}, QRhiTexture::RGBA16F);
    hdr_depth = rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, 4);
    hdr_resolve = rhi->newTexture(QRhiTexture::RGBA16F, size, 1, QRhiTexture::RenderTarget);
    replay_color = rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget);
    replay_depth = rhi->newTexture(QRhiTexture::D32F, size, 1, QRhiTexture::RenderTarget);
    if (!hdr_color->create() || !hdr_depth->create() || !hdr_resolve->create() || !replay_color->create() ||
        !replay_depth->create()) {
      return false;
    }
    QRhiColorAttachment color(hdr_color);
    color.setResolveTexture(hdr_resolve);
    QRhiTextureRenderTargetDescription hdr_description(color);
    hdr_description.setDepthStencilBuffer(hdr_depth);
    hdr = rhi->newTextureRenderTarget(hdr_description);
    hdr_pass = hdr->newCompatibleRenderPassDescriptor();
    hdr->setRenderPassDescriptor(hdr_pass);
    QRhiTextureRenderTargetDescription replay_description{QRhiColorAttachment(replay_color)};
    replay_description.setDepthTexture(replay_depth);
    replay = rhi->newTextureRenderTarget(replay_description);
    replay_pass = replay->newCompatibleRenderPassDescriptor();
    replay->setRenderPassDescriptor(replay_pass);
    return hdr->create() && replay->create();
  }
  void release() {
    delete replay;
    delete replay_pass;
    delete replay_depth;
    delete replay_color;
    delete hdr;
    delete hdr_pass;
    delete hdr_resolve;
    delete hdr_depth;
    delete hdr_color;
  }
};

double median(std::vector<double> values) {
  return pj_bench::stats(std::move(values)).median;
}

double elapsedMs(const QElapsedTimer& timer) {
  return static_cast<double>(timer.nsecsElapsed()) / 1e6;
}

struct Result {
  double gpu_ms = 0.0;
  double prepare_ms = 0.0;
  double enqueue_ms = 0.0;
  double end_frame_ms = 0.0;
  double end_frame_cpu_ms = 0.0;
  double frame_ms = 0.0;
  double convert_ms = 0.0;
  double scan_ms = 0.0;
  double latency_ms = 0.0;
  double used_mib = 0.0;
};

struct Camera {
  glm::vec3 focal{0.0F, 0.0F, 1.0F};
  float radius = 14.0F;
};

enum class Mode : std::uint8_t { kStatic, kChanging, kStreaming };

Result run(
    QRhi* rhi, const Targets& targets, BenchCloud& cloud, const WireCloud& wire, const Camera& framing, bool point_list,
    Mode mode, bool replay, int frames) {
  QRhiBuffer* render_mode = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16);
  render_mode->create();
  // The HDR target stands in for the widget's: the pass draws its direct pipelines into it.
  pj::scene3d::ScenePipelineRegistry registry;
  registry.setDirectTarget(rhi, targets.hdr_pass, 4);
  registry.setVariantTargets(targets.hdr_pass, 4, targets.replay_pass);
  pj::scene3d::PointRhiPass pass;
  Result result;
  if (!pass.initialize(
          {.rhi = rhi, .render_mode_uniforms = render_mode, .pipelines = &registry, .part = "point pipelines"})) {
    std::fprintf(stderr, "pass initialization failed\n");
    return result;
  }
  pass.setPointListEnabled(point_list);
  pass.setLayers({&cloud});

  const QSize size = targets.hdr->pixelSize();
  pj::scene3d::CameraState state;
  state.focal = framing.focal;
  state.radius = framing.radius;
  state.azimuth = 0.6F;
  state.elevation = 0.6F;
  pj::scene3d::OrbitCamera camera;
  camera.adoptState(state);
  const float aspect = static_cast<float>(size.width()) / static_cast<float>(size.height());
  const glm::mat4 projection = camera.projMatrix(aspect);
  const glm::mat4 view = camera.viewMatrixRelativeTo(glm::dvec3(0.0));
  const QMatrix4x4 correction = rhi->clipSpaceCorrMatrix();
  const QMatrix4x4 view_projection = correction * pj::scene3d::toQMatrix(projection * view);
  const QRhiViewport viewport(0.0F, 0.0F, static_cast<float>(size.width()), static_cast<float>(size.height()));

  std::vector<double> gpu;
  std::vector<double> prepare;
  std::vector<double> enqueue;
  std::vector<double> end_frame;
  std::vector<double> end_frame_cpu;
  std::vector<double> frame_times;
  std::vector<double> convert;
  std::vector<double> scan;
  std::vector<double> latency;
  cloud.convert(wire.cloud);
  // Warm-up (pipeline compilation, the first upload, the GPU clock ramp) until
  // pj_bench::WarmUp settles, then `frames` measured frames.
  pj_bench::WarmUp warm_up;
  bool warming = true;
  for (int measured = 0; measured < frames;) {
    QElapsedTimer latency_timer;
    latency_timer.start();
    QElapsedTimer frame_timer;
    frame_timer.start();
    QRhiCommandBuffer* command_buffer = nullptr;
    if (rhi->beginOffscreenFrame(&command_buffer) != QRhi::FrameOpSuccess) {
      break;
    }
    // Converted inside the frame, as the view's layers are (the previous frame's
    // upload has released the array by now), and timed apart from it.
    double convert_ms = 0.0;
    if (mode == Mode::kStreaming) {
      QElapsedTimer convert_timer;
      convert_timer.start();
      cloud.convert(wire.cloud);
      convert_ms = elapsedMs(convert_timer);
    } else if (mode == Mode::kChanging) {
      cloud.touch();
    }
    QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
    const std::array<std::int32_t, 4> render_mode_flags{1, 0, 0, 0};
    updates->updateDynamicBuffer(render_mode, 0, 16, render_mode_flags.data());
    QElapsedTimer prepare_timer;
    prepare_timer.start();
    pass.admit([](const std::string&, std::optional<PJ::Timepoint>, pj::scene3d::PlacementHold*) {
      return std::optional<pj::tf::Transform>(pj::tf::Transform{});
    });
    pass.prepare({rhi, updates, glm::dvec3(0.0), view_projection, view}, {projection, size, 1.0F});
    const double prepare_ms = elapsedMs(prepare_timer);
    if (!registry.createDirects()) {
      std::fprintf(stderr, "pipeline creation failed\n");
      break;
    }
    QElapsedTimer enqueue_timer;
    enqueue_timer.start();
    const auto draws = pass.drawsFor(&cloud);
    command_buffer->beginPass(targets.hdr, Qt::black, {1.0F, 0}, updates);
    for (const auto& draw : draws) {
      pass.submit(command_buffer, draw, pj::scene3d::ScenePass::kDirect, registry, viewport);
    }
    command_buffer->endPass();
    if (replay) {
      command_buffer->beginPass(targets.replay, Qt::transparent, {1.0F, 0});
      for (const auto& draw : draws) {
        pass.submit(command_buffer, draw, pj::scene3d::ScenePass::kDepthReplay, registry, viewport);
      }
      command_buffer->endPass();
    }
    const double enqueue_ms = elapsedMs(enqueue_timer);
    QElapsedTimer end_frame_timer;
    end_frame_timer.start();
    // CPU apart: endFrame's wall includes the offscreen wait for the GPU.
    const double end_frame_cpu_start = pj_bench::threadCpuMs();
    rhi->endOffscreenFrame();
    const double end_frame_cpu_ms = pj_bench::threadCpuMs() - end_frame_cpu_start;
    const double end_frame_ms = elapsedMs(end_frame_timer);
    const double frame_ms = elapsedMs(frame_timer);
    const double latency_ms = elapsedMs(latency_timer);
    double scan_ms = 0.0;
    if (mode == Mode::kStreaming) {
      QElapsedTimer scan_timer;
      scan_timer.start();
      const AABB scanned = scanXyz(wire.cloud);
      scan_ms = elapsedMs(scan_timer);
      if (!scanned.valid) {
        std::fprintf(stderr, "empty scan\n");
      }
    }
    if (warming) {
      warming = !warm_up.done(frame_ms);
    } else {
      ++measured;
      gpu.push_back(command_buffer->lastCompletedGpuTime() * 1000.0);
      prepare.push_back(prepare_ms);
      enqueue.push_back(enqueue_ms);
      end_frame.push_back(end_frame_ms);
      end_frame_cpu.push_back(end_frame_cpu_ms);
      frame_times.push_back(frame_ms - convert_ms);  // the frame without the conversion
      convert.push_back(convert_ms);
      scan.push_back(scan_ms);
      latency.push_back(latency_ms);
    }
  }
  result.gpu_ms = median(gpu);
  result.prepare_ms = median(prepare);
  result.enqueue_ms = median(enqueue);
  result.end_frame_ms = median(end_frame);
  result.end_frame_cpu_ms = median(end_frame_cpu);
  result.frame_ms = median(frame_times);
  result.convert_ms = median(convert);
  result.scan_ms = median(scan);
  result.latency_ms = median(latency);
  result.used_mib = static_cast<double>(rhi->statistics().usedBytes) / (1024.0 * 1024.0);
  registry.clear();
  pass.release();
  delete render_mode;
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  Q_INIT_RESOURCE(scene3d_rhi_shaders);  // lives in the pj_scene3d_widgets static archive
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addOption({"api", "opengl or vulkan", "api", "opengl"});
  parser.addOption({"points", "point count", "n", "10000000"});
  parser.addOption({"frames", "timed frames per mode", "f", "20"});
  parser.addOption({"shape", "point, sphere or cube", "shape", "point"});
  parser.addOption({"size", "target size WxH", "size", "1920x1080"});
  parser.addOption({"replay", "on or off: the single-sample depth replay pass", "replay", "on"});
  parser.addOption({"field-type", "u16, u32 or f32: the wire x/y/z datatype", "type", "f32"});
  parser.process(app);
  const bool replay = parser.value("replay") != "off";
  const DT field_type = parser.value("field-type") == "u16"   ? DT::kUint16
                        : parser.value("field-type") == "u32" ? DT::kUint32
                                                              : DT::kFloat32;

  const QStringList size_text = parser.value("size").split('x');
  const QSize size(size_text.value(0).toInt(), size_text.value(1).toInt());
  const auto shape = parser.value("shape") == "sphere" ? pj::scene3d::PointShape::kSphere
                     : parser.value("shape") == "cube" ? pj::scene3d::PointShape::kCube
                                                       : pj::scene3d::PointShape::kPoint;

  // Declared before the QRhi, which uses them until it is destroyed.
  std::unique_ptr<QOffscreenSurface> surface;
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
  QVulkanInstance vulkan;
#endif
  std::unique_ptr<QRhi> rhi;
  if (parser.value("api") == "vulkan") {
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
    vulkan.setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
    if (!vulkan.create()) {
      std::fprintf(stderr, "no Vulkan instance\n");
      return 1;
    }
    QRhiVulkanInitParams params;
    params.inst = &vulkan;
    rhi.reset(QRhi::create(QRhi::Vulkan, &params, QRhi::EnableTimestamps));
#endif
  } else {
    surface.reset(QRhiGles2InitParams::newFallbackSurface());
    QRhiGles2InitParams params;
    params.fallbackSurface = surface.get();
    rhi.reset(QRhi::create(QRhi::OpenGLES2, &params, QRhi::EnableTimestamps));
  }
  if (!rhi) {
    std::fprintf(stderr, "QRhi creation failed\n");
    return 1;
  }
  std::printf(
      "backend %s, device %s, point-list capable %d\n", rhi->backendName(), rhi->driverInfo().deviceName.constData(),
      rhi->isFeatureSupported(QRhi::VertexShaderPointSize) ? 1 : 0);

  Targets targets;
  if (!targets.create(rhi.get(), size)) {
    std::fprintf(stderr, "render targets failed\n");
    return 1;
  }
  const auto points = static_cast<std::size_t>(parser.value("points").toLongLong());
  const int frames = parser.value("frames").toInt();
  const WireCloud wire(points, field_type);
  BenchCloud cloud(shape);
  Camera framing;
  framing.focal = {wire.shift, wire.shift, wire.scale};
  framing.radius *= wire.scale;
  const double mib = 1024.0 * 1024.0;
  std::printf(
      "%zu points, shape %s, %dx%d, x/y/z %s, depth replay %s; per sample: wire %.0f MiB, decoded %.0f MiB, "
      "vertices %.0f MiB; times in ms\n",
      points, qPrintable(parser.value("shape")), size.width(), size.height(), qPrintable(parser.value("field-type")),
      replay ? "on" : "off", static_cast<double>(wire.bytes.size()) / mib,
      static_cast<double>(points * (sizeof(glm::vec3) + sizeof(float))) / mib,
      static_cast<double>(points * sizeof(PointVertex)) / mib);
  for (const Mode mode : {Mode::kStatic, Mode::kChanging, Mode::kStreaming}) {
    for (const bool point_list : {false, true}) {
      if (point_list && !rhi->isFeatureSupported(QRhi::VertexShaderPointSize)) {
        continue;
      }
      const Result result = run(rhi.get(), targets, cloud, wire, framing, point_list, mode, replay, frames);
      const char* mode_name = mode == Mode::kStatic ? "static" : mode == Mode::kChanging ? "changing" : "streaming";
      std::printf(
          "%-9s %-10s gpu %8.2f  prepare %7.2f  enqueue %6.2f  endframe %8.2f (cpu %7.2f)  frame %8.2f", mode_name,
          point_list ? "point-list" : "quads", result.gpu_ms, result.prepare_ms, result.enqueue_ms, result.end_frame_ms,
          result.end_frame_cpu_ms, result.frame_ms);
      if (mode == Mode::kStreaming) {
        std::printf("  convert %7.2f  scan %6.2f  latency %8.2f", result.convert_ms, result.scan_ms, result.latency_ms);
      }
      std::printf("  gpu-mem %6.1f MiB\n", result.used_mib);
      std::fflush(stdout);
    }
  }
  std::printf("peak RSS %s MiB\n", pj_bench::fmt(pj_bench::peakRssMib(), 0).constData());
  targets.release();
  return 0;
}
