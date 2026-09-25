// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Point-cloud GPU/CPU benchmark for the QRhi point pass (QRHI_MIGRATION P3). It
// drives the production PointRhiPass on an offscreen QRhi, the way the view does
// with its quality chain on: the colour pass into a 4x RGBA16F + D32F target, then
// the single-sample RGBA8 + D32F depth replay SSAO/EDL read (their full-screen
// passes cost the same for every topology, so they are left out). It compares the
// instanced quads against the point-list fast path, for a static cloud and for one
// that re-uploads every frame.
//
//   pointcloud_rhi_benchmark [--api opengl|vulkan] [--points N] [--frames F]
//                            [--shape point|sphere|cube] [--size WxH]
//
// Prints the backend + adapter, then per mode: median GPU frame time (QRhi
// timestamps), CPU prepare time, whole-frame wall time and the backend's GPU
// memory in use where it reports one (Vulkan).

#include <rhi/qrhi.h>

#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QOffscreenSurface>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#include <QVulkanInstance>
#endif
#include <algorithm>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "pj_scene3d_widgets/point_renderable.h"
#include "rhi/passes/point_rhi_pass.h"
#include "rhi/rhi_scene_pass.h"

namespace {

using pj::scene3d::AABB;
using pj::scene3d::PointRenderable;
using pj::scene3d::PointVertex;

// A static in-memory cloud; `touch()` makes the next prepare() re-upload it.
class BenchCloud : public PointRenderable {
 public:
  BenchCloud(std::size_t count, pj::scene3d::PointShape shape) : shape_(shape) {
    std::mt19937 random(7);
    std::uniform_real_distribution<float> xy(-5.0F, 5.0F);
    std::uniform_real_distribution<float> z(0.0F, 2.0F);
    vertices_.resize(count);
    for (PointVertex& vertex : vertices_) {
      vertex = {xy(random), xy(random), z(random), 0.0F, 0xFFFFFFFFU};
      vertex.scalar = vertex.z * 0.5F;
      pj::scene3d::expandAABB(bounds_, glm::vec3(vertex.x, vertex.y, vertex.z));
    }
  }
  void touch() {
    ++revision_;
  }
  bool prepareForRender(std::uint64_t /*remaining*/) override {
    return true;
  }
  [[nodiscard]] const std::vector<PointVertex>& vertices() const override {
    return vertices_;
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
  std::vector<PointVertex> vertices_;
  AABB bounds_;
  std::uint64_t revision_ = 1;
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
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

struct Result {
  double gpu_ms = 0.0;
  double prepare_ms = 0.0;
  double frame_ms = 0.0;
  double used_mib = 0.0;
};

Result run(QRhi* rhi, const Targets& targets, BenchCloud& cloud, bool point_list, bool changing, int frames) {
  QRhiBuffer* render_mode = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16);
  render_mode->create();
  pj::scene3d::ScenePipelineRegistry registry;
  pj::scene3d::PointRhiPass pass;
  Result result;
  if (!pass.initialize({rhi, targets.hdr_pass, 4, render_mode})) {
    std::fprintf(stderr, "pass initialization failed\n");
    return result;
  }
  pass.setPointListEnabled(point_list);
  pass.registerPipelines(registry);
  registry.setCloneTargets(rhi, targets.hdr_pass, 4, targets.replay_pass);
  pass.setLayers({&cloud});

  const QSize size = targets.hdr->pixelSize();
  pj::scene3d::CameraState state;
  state.focal = {0.0F, 0.0F, 1.0F};
  state.radius = 14.0F;
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
  std::vector<double> frame_times;
  for (int frame = 0; frame < frames + 3; ++frame) {
    QElapsedTimer frame_timer;
    frame_timer.start();
    QRhiCommandBuffer* command_buffer = nullptr;
    if (rhi->beginOffscreenFrame(&command_buffer) != QRhi::FrameOpSuccess) {
      break;
    }
    if (changing) {
      cloud.touch();
    }
    QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
    const std::array<std::int32_t, 4> mode{1, 0, 0, 0};
    updates->updateDynamicBuffer(render_mode, 0, 16, mode.data());
    QElapsedTimer prepare_timer;
    prepare_timer.start();
    pj::scene3d::SceneBoundsCollector bounds;
    pass.admit(
        [](const std::string&) { return std::optional<pj::scene3d::Transform>(pj::scene3d::Transform{}); }, bounds);
    pass.prepare({rhi, updates, glm::dvec3(0.0), view_projection, view}, {projection, size, 1.0F});
    const double prepare_ms = static_cast<double>(prepare_timer.nsecsElapsed()) / 1e6;
    const auto draws = pass.drawsFor(&cloud);
    command_buffer->beginPass(targets.hdr, Qt::black, {1.0F, 0}, updates);
    for (const auto& draw : draws) {
      pass.submit(command_buffer, draw, pj::scene3d::ScenePass::kDirect, registry, viewport);
    }
    command_buffer->endPass();
    command_buffer->beginPass(targets.replay, Qt::transparent, {1.0F, 0});
    for (const auto& draw : draws) {
      pass.submit(command_buffer, draw, pj::scene3d::ScenePass::kDepthReplay, registry, viewport);
    }
    command_buffer->endPass();
    rhi->endOffscreenFrame();
    if (frame >= 3) {  // warm-up: pipeline compilation, first upload
      gpu.push_back(command_buffer->lastCompletedGpuTime() * 1000.0);
      prepare.push_back(prepare_ms);
      frame_times.push_back(static_cast<double>(frame_timer.nsecsElapsed()) / 1e6);
    }
  }
  result.gpu_ms = median(gpu);
  result.prepare_ms = median(prepare);
  result.frame_ms = median(frame_times);
  result.used_mib = static_cast<double>(rhi->statistics().usedBytes) / (1024.0 * 1024.0);
  pass.release();
  registry.clear();
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
  parser.process(app);

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
  BenchCloud cloud(points, shape);
  std::printf(
      "%zu points, shape %s, %dx%d, cloud %.0f MiB\n", points, qPrintable(parser.value("shape")), size.width(),
      size.height(), static_cast<double>(points * sizeof(PointVertex)) / (1024.0 * 1024.0));
  for (const bool changing : {false, true}) {
    for (const bool point_list : {false, true}) {
      if (point_list && !rhi->isFeatureSupported(QRhi::VertexShaderPointSize)) {
        continue;
      }
      const Result result = run(rhi.get(), targets, cloud, point_list, changing, frames);
      std::printf(
          "%-7s %-10s gpu %8.2f ms  prepare %8.2f ms  frame %8.2f ms  gpu-mem %7.1f MiB\n",
          changing ? "changing" : "static", point_list ? "point-list" : "quads", result.gpu_ms, result.prepare_ms,
          result.frame_ms, result.used_mib);
      std::fflush(stdout);
    }
  }
  targets.release();
  return 0;
}
