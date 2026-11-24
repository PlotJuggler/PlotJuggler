// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// GPU cost of the clip behind the eye (TECHNICAL_NOTES "SwiftShader drops lines that cross behind the eye"): the same
// line buffer drawn as a vertex list (what a GPU draws) and as one instance per segment through the *_segment.vert
// shaders (what a software rasterizer draws), on an offscreen QRhi into the view's 4x RGBA16F + D32F colour target. The
// scene is a field of random two-colour segments in a 100 m square: marker stream lines (28-byte ColorVertex pairs,
// scene.vert) or mesh-map links (float positions and RGBA8 colours, meshmap.vert).
//
//   line_clip_rhi_benchmark [--vertices N] [--family markers|links] [--camera over|far] [--frames F] [--rounds R]
//                           [--size WxH]
//
// `over` puts the eye inside the field ~1 m up, so many segments cross the plane through the eye; `far` frames the
// whole field (sub-pixel segments: vertex-bound). Per mode, R rounds of (warm-up, F frames) in rotation: the median GPU
// frame time (QRhi timestamps) over every frame and the min/max of the round medians. Then the pixels the vertex list
// drew and how many differ between the two modes (only the clipped segments' ends may).

#include <rhi/qrhi.h>

#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "pj_bench_probes.h"
#include "pj_scene3d_core/camera/camera.h"
#include "rhi/rhi_scene_pass.h"

namespace {

using pj::scene3d::ColorVertex;

QShader load(const char* name) {
  QFile file(QStringLiteral(":/scene3d_rhi/") + QLatin1StringView(name));
  if (!file.open(QIODevice::ReadOnly)) {
    std::fprintf(stderr, "missing %s\n", name);
    std::exit(1);
  }
  return QShader::fromSerialized(file.readAll());
}

double elapsedMs(const QElapsedTimer& timer) {
  return static_cast<double>(timer.nsecsElapsed()) / 1e6;
}

struct Targets {
  QRhiRenderBuffer* color = nullptr;
  QRhiRenderBuffer* depth = nullptr;
  QRhiTexture* resolve = nullptr;
  QRhiTextureRenderTarget* target = nullptr;
  QRhiRenderPassDescriptor* pass = nullptr;
  bool create(QRhi* rhi, QSize size) {
    color = rhi->newRenderBuffer(QRhiRenderBuffer::Color, size, 4, {}, QRhiTexture::RGBA16F);
    depth = rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, 4);
    resolve = rhi->newTexture(QRhiTexture::RGBA16F, size, 1, QRhiTexture::RenderTarget);
    if (!color->create() || !depth->create() || !resolve->create()) {
      return false;
    }
    QRhiColorAttachment attachment(color);
    attachment.setResolveTexture(resolve);
    QRhiTextureRenderTargetDescription description(attachment);
    description.setDepthStencilBuffer(depth);
    target = rhi->newTextureRenderTarget(description);
    pass = target->newCompatibleRenderPassDescriptor();
    target->setRenderPassDescriptor(pass);
    return target->create();
  }
};

}  // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  Q_INIT_RESOURCE(scene3d_rhi_shaders);  // lives in the pj_scene3d_widgets static archive
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addOption({"vertices", "line vertices", "n", "10000000"});
  parser.addOption({"family", "markers or links", "family", "markers"});
  parser.addOption({"camera", "over or far", "camera", "far"});
  parser.addOption({"frames", "timed frames per round", "f", "30"});
  parser.addOption({"rounds", "rounds per mode", "r", "7"});
  parser.addOption({"size", "target WxH", "size", "1920x1080"});
  parser.process(app);
  const auto vertices = static_cast<std::size_t>(parser.value("vertices").toLongLong()) & ~std::size_t{1};
  const auto segments = static_cast<quint32>(vertices / 2);
  const bool links = parser.value("family") == "links";
  const bool over = parser.value("camera") == "over";
  const int frames = parser.value("frames").toInt();
  const int rounds = parser.value("rounds").toInt();
  const QStringList size_text = parser.value("size").split('x');
  const QSize size(size_text.value(0).toInt(), size_text.value(1).toInt());

  std::unique_ptr<QOffscreenSurface> surface(QRhiGles2InitParams::newFallbackSurface());
  QRhiGles2InitParams params;
  params.fallbackSurface = surface.get();
  std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::OpenGLES2, &params, QRhi::EnableTimestamps));
  Targets targets;
  if (!rhi || !targets.create(rhi.get(), size)) {
    std::fprintf(stderr, "QRhi or render target creation failed\n");
    return 1;
  }

  pj::scene3d::CameraState state;
  state.focal = over ? glm::vec3(0.0F, 0.0F, 0.5F) : glm::vec3(0.0F);
  state.radius = over ? 3.0F : 160.0F;
  state.azimuth = 0.6F;
  state.elevation = over ? 0.15F : 0.6F;
  pj::scene3d::OrbitCamera camera;
  camera.adoptState(state);
  pj::scene3d::AABB bounds;
  bounds.min = {-52.0F, -52.0F, -1.0F};
  bounds.max = {52.0F, 52.0F, 4.0F};
  bounds.valid = true;
  camera.setSceneBounds(bounds);
  const float aspect = static_cast<float>(size.width()) / static_cast<float>(size.height());
  const glm::mat4 view_projection = camera.projMatrix(aspect) * camera.viewMatrixRelativeTo(glm::dvec3(0.0));

  // The field in both families' formats; consecutive vertices are a segment.
  std::vector<ColorVertex> marker_vertices(vertices);
  std::vector<float> link_positions(3 * vertices);
  std::vector<std::uint32_t> link_colors(vertices);
  std::mt19937 random(11);
  std::uniform_real_distribution<float> xy(-50.0F, 50.0F);
  std::uniform_real_distribution<float> z(0.0F, 3.0F);
  std::uniform_real_distribution<float> length(0.2F, 4.0F);
  std::uniform_real_distribution<float> angle(0.0F, 6.2831853F);
  std::uniform_real_distribution<float> unit(0.0F, 1.0F);
  const auto byte = [](float value) { return static_cast<std::uint32_t>(std::lround(value * 255.0F)); };
  std::size_t crossing = 0;
  for (std::size_t index = 0; index < vertices; index += 2) {
    const glm::vec3 a(xy(random), xy(random), z(random));
    const float heading = angle(random);
    const float l = length(random);
    const glm::vec3 b = a + glm::vec3(l * std::cos(heading), l * std::sin(heading), 0.3F * (z(random) - 1.5F));
    for (const auto& [end, position] : {std::pair{std::size_t{0}, a}, std::pair{std::size_t{1}, b}}) {
      const glm::vec3 color(unit(random), unit(random), unit(random));
      marker_vertices[index + end] = {position.x, position.y, position.z, color.r, color.g, color.b, 1.0F};
      std::memcpy(&link_positions[3 * (index + end)], &position, sizeof(glm::vec3));
      link_colors[index + end] = byte(color.r) | (byte(color.g) << 8U) | (byte(color.b) << 16U) | (255U << 24U);
    }
    const bool a_behind = (view_projection * glm::vec4(a, 1.0F)).w < 0.0F;
    const bool b_behind = (view_projection * glm::vec4(b, 1.0F)).w < 0.0F;
    crossing += a_behind != b_behind ? 1U : 0U;
  }

  const auto upload = [&](const void* data, std::size_t bytes) {
    QRhiBuffer* buffer = rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::VertexBuffer, static_cast<quint32>(bytes));
    if (!buffer->create()) {
      std::fprintf(stderr, "buffer creation failed\n");
      std::exit(1);
    }
    QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
    updates->uploadStaticBuffer(buffer, 0, static_cast<quint32>(bytes), data);
    QRhiCommandBuffer* command_buffer = nullptr;
    rhi->beginOffscreenFrame(&command_buffer);
    command_buffer->resourceUpdate(updates);
    rhi->endOffscreenFrame();
    return buffer;
  };
  QRhiBuffer* first = links ? upload(link_positions.data(), link_positions.size() * sizeof(float))
                            : upload(marker_vertices.data(), marker_vertices.size() * sizeof(ColorVertex));
  QRhiBuffer* second = links ? upload(link_colors.data(), link_colors.size() * sizeof(std::uint32_t)) : nullptr;

  // Binding 0: the marker stream's mat4, or MeshMapUniforms (view_projection, model, then six vec4s); binding 8: the
  // render mode both fragment shaders read.
  constexpr quint32 kUniformFloats = 56;
  QRhiBuffer* uniforms = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformFloats * sizeof(float));
  QRhiBuffer* render_mode = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16);
  uniforms->create();
  render_mode->create();
  const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
  QRhiShaderResourceBindings* bindings = rhi->newShaderResourceBindings();
  bindings->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, stages, uniforms),
      QRhiShaderResourceBinding::uniformBuffer(8, QRhiShaderResourceBinding::FragmentStage, render_mode),
  });
  bindings->create();
  std::array<float, kUniformFloats> uniform_data{};
  const QMatrix4x4 corrected = rhi->clipSpaceCorrMatrix() * pj::scene3d::toQMatrix(view_projection);
  std::memcpy(uniform_data.data(), corrected.constData(), 16 * sizeof(float));
  const QMatrix4x4 identity;
  std::memcpy(uniform_data.data() + 16, identity.constData(), 16 * sizeof(float));
  uniform_data[32 + 4 * 3 + 3] = 1.0F;  // color_params.w: opacity
  uniform_data[32 + 4 * 5] = 3.0F;      // flags.x: the unlit line mode

  using A = QRhiVertexInputAttribute;
  using B = QRhiVertexInputBinding;
  QRhiVertexInputLayout list_layout = pj::scene3d::colorVertexLayout();
  QRhiVertexInputLayout segment_layout = pj::scene3d::lineSegmentLayout();
  if (links) {
    // meshmap_rhi_pass.cpp's layouts: the vertex list binds the colours to the ignored value slot too.
    list_layout.setBindings({B(12), B(4), B(4)});
    list_layout.setAttributes({A(0, 0, A::Float3, 0), A(1, 1, A::Float, 0), A(2, 2, A::UNormByte4, 0)});
    segment_layout.setBindings({B(24, B::PerInstance), B(8, B::PerInstance)});
    segment_layout.setAttributes(
        {A(0, 0, A::Float3, 0), A(0, 1, A::Float3, 12), A(1, 2, A::UNormByte4, 0), A(1, 3, A::UNormByte4, 4)});
  }
  const QShader fragment = load(links ? "meshmap.frag.qsb" : "scene.frag.qsb");
  const auto pipeline = [&](const char* vertex, const QRhiVertexInputLayout& layout) {
    QRhiGraphicsPipeline* created = rhi->newGraphicsPipeline();
    created->setTopology(QRhiGraphicsPipeline::Lines);
    created->setShaderStages({{QRhiShaderStage::Vertex, load(vertex)}, {QRhiShaderStage::Fragment, fragment}});
    created->setVertexInputLayout(layout);
    created->setShaderResourceBindings(bindings);
    created->setRenderPassDescriptor(targets.pass);
    created->setSampleCount(4);
    created->setDepthTest(true);
    created->setDepthWrite(true);
    created->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
    if (!created->create()) {
      std::fprintf(stderr, "pipeline creation failed\n");
      std::exit(1);
    }
    return created;
  };
  const std::array<QRhiGraphicsPipeline*, 2> pipelines{
      pipeline(links ? "meshmap.vert.qsb" : "scene.vert.qsb", list_layout),
      pipeline(links ? "meshmap_segment.vert.qsb" : "scene_segment.vert.qsb", segment_layout)};
  constexpr std::array<const char*, 2> kModes{"vertex list", "segments"};

  std::array<std::vector<double>, 2> gpu;
  std::array<std::vector<double>, 2> round_medians;
  std::array<QRhiReadbackResult, 2> images;
  for (int round = 0; round < rounds; ++round) {
    for (std::size_t mode = 0; mode < pipelines.size(); ++mode) {
      std::vector<double> times;
      pj_bench::WarmUp warm_up(150, 100, 60, 1500);
      bool warming = true;
      while (static_cast<int>(times.size()) < frames) {
        QElapsedTimer frame_timer;
        frame_timer.start();
        QRhiCommandBuffer* command_buffer = nullptr;
        if (rhi->beginOffscreenFrame(&command_buffer) != QRhi::FrameOpSuccess) {
          return 1;
        }
        QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
        updates->updateDynamicBuffer(uniforms, 0, kUniformFloats * sizeof(float), uniform_data.data());
        const std::array<std::int32_t, 4> linear_target{1, 0, 0, 0};
        updates->updateDynamicBuffer(render_mode, 0, 16, linear_target.data());
        command_buffer->beginPass(targets.target, Qt::black, {1.0F, 0}, updates);
        command_buffer->setGraphicsPipeline(pipelines[mode]);
        command_buffer->setViewport({0, 0, static_cast<float>(size.width()), static_cast<float>(size.height())});
        command_buffer->setShaderResources(bindings);
        const std::array<QRhiCommandBuffer::VertexInput, 3> inputs{{{first, 0}, {second, 0}, {second, 0}}};
        command_buffer->setVertexInput(0, links ? (mode == 0 ? 3 : 2) : 1, inputs.data());
        pj::scene3d::drawLines(*command_buffer, 2 * segments, mode == 1);
        command_buffer->endPass();
        if (!warming && static_cast<int>(times.size()) + 1 == frames && round + 1 == rounds) {
          QRhiResourceUpdateBatch* readback = rhi->nextResourceUpdateBatch();
          readback->readBackTexture({targets.resolve}, &images[mode]);
          command_buffer->resourceUpdate(readback);
        }
        rhi->endOffscreenFrame();
        if (warming) {
          warming = !warm_up.done(elapsedMs(frame_timer));
        } else {
          times.push_back(command_buffer->lastCompletedGpuTime() * 1000.0);
        }
      }
      gpu[mode].insert(gpu[mode].end(), times.begin(), times.end());
      round_medians[mode].push_back(pj_bench::stats(times).median);
    }
  }

  std::printf(
      "device %s | %s, camera %s, %zu vertices (%u segments, %zu crossing the eye plane), %dx%d, %d rounds x %d "
      "frames; GPU ms\n",
      rhi->driverInfo().deviceName.constData(), links ? "mesh-map links" : "marker lines",
      qPrintable(parser.value("camera")), vertices, segments, crossing, size.width(), size.height(), rounds, frames);
  for (std::size_t mode = 0; mode < pipelines.size(); ++mode) {
    const auto [lo, hi] = std::minmax_element(round_medians[mode].begin(), round_medians[mode].end());
    std::printf("%-12s gpu %8.3f  rounds %8.3f .. %8.3f\n", kModes[mode], pj_bench::stats(gpu[mode]).median, *lo, *hi);
  }
  // RGBA16F: 8 bytes a pixel.
  const QByteArray& list = images[0].data;
  const QByteArray& segment = images[1].data;
  std::size_t drawn = 0;
  std::size_t differ = 0;
  for (qsizetype offset = 0; offset + 8 <= std::min(list.size(), segment.size()); offset += 8) {
    const char* pixel = list.constData() + offset;
    drawn += std::any_of(pixel, pixel + 6, [](char value) { return value != 0; }) ? 1U : 0U;
    differ += std::memcmp(pixel, segment.constData() + offset, 8) != 0 ? 1U : 0U;
  }
  std::printf("pixels drawn %zu, differing between the modes %zu\n", drawn, differ);
  return 0;
}
