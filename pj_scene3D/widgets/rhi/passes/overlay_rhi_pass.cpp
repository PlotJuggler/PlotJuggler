// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "overlay_rhi_pass.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

#include "pj_scene3d_core/poses_in_frame_render.h"
#include "pj_scene3d_widgets/gizmos/arrow_mesh.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"
#include "pose_rhi_pass.h"

namespace pj::scene3d {
namespace {

constexpr quint32 kGizmoUniformBytes = 192U;
constexpr quint32 kImageUniformBytes = 16U;
constexpr int kGizmoArmCount = 3;
constexpr int kGizmoSampleCount = 4;

struct alignas(16) GizmoUniforms {
  std::array<float, 16> view_projection{};
  std::array<float, 16> fixed_from_source{};
  std::array<float, 16> view{};
};
static_assert(sizeof(GizmoUniforms) == kGizmoUniformBytes);

// The corner gizmo's arrow, in a [-1.3, 1.3] orthographic box: its own mesh
// rather than the scene's thinner unit arrow scaled, so its silhouette and
// shading keep the desktop look (24 segments, the head 0.30 long).
const ArrowMeshData& gizmoArrowMesh() {
  static const ArrowMeshData mesh = buildArrowMesh(
      ArrowMeshParams{
          .length = 0.95F, .shaft_radius = 0.05F, .head_length = 0.30F, .head_radius = 0.13F, .segments = 24});
  return mesh;
}

// The corner gizmo's arms, constant: the scene's triad arms at unit length,
// recoloured with the saturated HUD triplets.
const std::vector<PoseTriadInstance>& gizmoArms() {
  static const std::vector<PoseTriadInstance> arms = [] {
    std::vector<PoseTriadInstance> triad;
    appendTriadArms(glm::mat4(1.0F), PoseTriadStyle{.axis_length = 1.0F}, triad);
    const std::array<glm::vec3, kGizmoArmCount> colors = {look::kHudTriadX, look::kHudTriadY, look::kHudTriadZ};
    for (std::size_t arm = 0; arm < triad.size(); ++arm) {
      triad[arm].color = glm::vec4(colors[arm], 1.0F);
    }
    return triad;
  }();
  return arms;
}

// Maps a logical pixel rectangle (top-left origin) to clip-space x/y.
std::array<float, 4> clipRect(const QRectF& pixels, const QSize& target, bool y_up) {
  const auto x = [&](double px) { return static_cast<float>(2.0 * px / target.width() - 1.0); };
  const auto y = [&](double py) {
    const double down = 2.0 * py / target.height() - 1.0;
    return static_cast<float>(y_up ? -down : down);
  };
  return {x(pixels.left()), y(pixels.top()), x(pixels.right()), y(pixels.bottom())};
}

}  // namespace

bool OverlayRhiPass::initialize(const RhiPassSetup& setup, qreal dpr) {
  QRhi* rhi = setup.rhi;
  if (rhi == nullptr) {
    return false;
  }
  rhi_ = rhi;

  gizmo_instance_buffer_ = rhi->newBuffer(
      QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
      kGizmoArmCount * static_cast<quint32>(sizeof(PoseTriadInstance)));
  if (!ensureUniformBuffer(rhi, gizmo_uniform_buffer_, kGizmoUniformBytes) || !gizmo_instance_buffer_->create() ||
      !createStaticMesh(rhi, setup.updates, gizmoArrowMesh(), gizmo_arrow_vertices_, gizmo_arrow_indices_)) {
    return false;
  }
  setup.updates->uploadStaticBuffer(gizmo_instance_buffer_, gizmoArms().data());
  gizmo_shader_resources_ = createBindings(
      rhi,
      {QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, gizmo_uniform_buffer_)});

  image_sampler_ = rhi->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
      QRhiSampler::ClampToEdge);
  image_layout_texture_ = rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1));
  if (gizmo_shader_resources_ == nullptr || !image_sampler_->create() ||
      !ensureUniformBuffer(rhi, image_layout_uniform_buffer_, kImageUniformBytes) || !image_layout_texture_->create()) {
    return false;
  }
  image_layout_shader_resources_ = createImageBindings(image_layout_uniform_buffer_, image_layout_texture_);
  // Premultiplied source-over that keeps the output's alpha (Wayland treats a
  // non-opaque surface as translucent).
  QRhiGraphicsPipeline::TargetBlend premultiplied = PJ::straightAlphaBlend();
  premultiplied.srcColor = QRhiGraphicsPipeline::One;
  premultiplied.srcAlpha = QRhiGraphicsPipeline::Zero;
  premultiplied.dstAlpha = QRhiGraphicsPipeline::One;
  image_pipeline_ = createPipeline(
      rhi,
      {
          .topology = QRhiGraphicsPipeline::TriangleStrip,
          .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/overlay.vert.qsb")),
          .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/overlay.frag.qsb")),
          .bindings = image_layout_shader_resources_,
          .blend = premultiplied,
          .depth_test = false,
          .depth_write = false,
      },
      setup.pipelines->directRenderPass(), setup.pipelines->directSampleCount());
  if (image_pipeline_ == nullptr) {
    return false;
  }
  const PipelineDesc gizmo_desc{
      .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/pose.vert.qsb")),
      .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/gizmo.frag.qsb")),
      .input_layout = poseTriadInputLayout(),
      .bindings = gizmo_shader_resources_,
      // No blend: opaque arms over a transparent clear resolve to premultiplied coverage.
      .blend = QRhiGraphicsPipeline::TargetBlend{},
  };
  const int size = gizmoPixels(dpr);
  if (!createGizmoTarget(setup, QSize(size, size), gizmo_desc)) {
    qWarning("SceneViewWidget QRhi: the corner gizmo's target is unavailable; the gizmo is not drawn");
  }
  return true;
}

bool OverlayRhiPass::createGizmoTarget(const RhiPassSetup& setup, const QSize& size, const PipelineDesc& gizmo_desc) {
  QRhi* rhi = setup.rhi;
  const int samples =
      rhi->isFeatureSupported(QRhi::MultisampleRenderBuffer) && rhi->supportedSampleCounts().contains(kGizmoSampleCount)
          ? kGizmoSampleCount
          : 1;
  GizmoTarget& gizmo = gizmo_target_;
  gizmo.depth = rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, samples);
  gizmo.quad.texture = rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget);
  bool created = gizmo.depth->create() && gizmo.quad.texture->create();
  QRhiColorAttachment color(gizmo.quad.texture);
  if (created && samples > 1) {
    gizmo.color = rhi->newRenderBuffer(QRhiRenderBuffer::Color, size, samples);
    created = gizmo.color->create();
    color = QRhiColorAttachment(gizmo.color);
    color.setResolveTexture(gizmo.quad.texture);
  }
  if (created) {
    QRhiTextureRenderTargetDescription description(color);
    description.setDepthStencilBuffer(gizmo.depth);
    gizmo.target = rhi->newTextureRenderTarget(description, QRhiTextureRenderTarget::DoNotStoreDepthStencilContents);
    gizmo.render_pass = gizmo.target->newCompatibleRenderPassDescriptor();
    gizmo.target->setRenderPassDescriptor(gizmo.render_pass);
    created = gizmo.target->create();
  }
  if (created) {
    gizmo.pipeline = createPipeline(rhi, gizmo_desc, gizmo.render_pass, samples);
    created = gizmo.pipeline != nullptr && ensureUniformBuffer(rhi, gizmo.quad.uniform_buffer, kImageUniformBytes);
  }
  if (created) {
    gizmo.quad.shader_resources = createImageBindings(gizmo.quad.uniform_buffer, gizmo.quad.texture);
    created = gizmo.quad.shader_resources != nullptr;
  }
  if (!created) {
    releaseGizmoTarget();
  }
  return created;
}

bool OverlayRhiPass::resizeGizmoTarget(const QSize& size) {
  GizmoTarget& gizmo = gizmo_target_;
  if (gizmo.quad.texture->pixelSize() == size) {
    return true;
  }
  gizmo.drawn.reset();
  gizmo.quad.rect_written = false;
  gizmo.depth->setPixelSize(size);
  gizmo.quad.texture->setPixelSize(size);
  bool created = gizmo.depth->create() && gizmo.quad.texture->create();
  if (created && gizmo.color != nullptr) {
    gizmo.color->setPixelSize(size);
    created = gizmo.color->create();
  }
  if (created && gizmo.target->create() && gizmo.quad.shader_resources->create()) {
    return true;
  }
  // ponytail: not retried until the next initialize() (new QRhi or output target).
  qWarning("SceneViewWidget QRhi: the corner gizmo's target could not be resized; the gizmo is not drawn");
  releaseGizmoTarget();
  return false;
}

void OverlayRhiPass::releaseGizmoTarget() {
  GizmoTarget& gizmo = gizmo_target_;
  releaseImage(gizmo.quad);
  gizmo.pipeline.reset();
  delete gizmo.target;
  delete gizmo.render_pass;
  delete gizmo.depth;
  delete gizmo.color;
  gizmo = {};
}

QRhiShaderResourceBindings* OverlayRhiPass::createImageBindings(QRhiBuffer* uniforms, QRhiTexture* texture) const {
  return createBindings(
      rhi_, {
                QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, uniforms),
                QRhiShaderResourceBinding::sampledTexture(
                    1, QRhiShaderResourceBinding::FragmentStage, texture, image_sampler_),
            });
}

bool OverlayRhiPass::ensureImage(QRhiResourceUpdateBatch* updates, ImageGpu& gpu, const QImage& image) {
  if (gpu.texture != nullptr && gpu.cache_key == image.cacheKey()) {
    return true;
  }
  if (gpu.texture == nullptr || gpu.texture->pixelSize() != image.size()) {
    delete gpu.shader_resources;
    delete gpu.texture;
    gpu.shader_resources = nullptr;
    gpu.texture = rhi_->newTexture(QRhiTexture::RGBA8, image.size());
    if (!gpu.texture->create()) {
      releaseImage(gpu);
      return false;
    }
  }
  if (!ensureUniformBuffer(rhi_, gpu.uniform_buffer, kImageUniformBytes)) {
    releaseImage(gpu);
    return false;
  }
  if (gpu.shader_resources == nullptr) {
    gpu.shader_resources = createImageBindings(gpu.uniform_buffer, gpu.texture);
    if (gpu.shader_resources == nullptr) {
      releaseImage(gpu);
      return false;
    }
  }
  // The texture is RGBA8: the producers paint straight into this format.
  Q_ASSERT(image.format() == QImage::Format_RGBA8888_Premultiplied);
  updates->uploadTexture(
      gpu.texture, QRhiTextureUploadDescription({0, 0, QRhiTextureSubresourceUploadDescription(image)}));
  gpu.cache_key = image.cacheKey();
  return true;
}

void OverlayRhiPass::prepare(
    QRhiResourceUpdateBatch* updates, const glm::mat4& view, const QSize& output_px, qreal dpr,
    const std::array<Image, kSlotCount>& images) {
  if (!ready() || updates == nullptr || output_px.isEmpty()) {
    return;
  }
  output_viewport_ =
      QRhiViewport(0.0F, 0.0F, static_cast<float>(output_px.width()), static_cast<float>(output_px.height()));
  const RhiConventions conventions = rhiConventions(*rhi_);

  // Gizmo: the camera's rotation applied to a fixed orthographic triad, drawn
  // into its own target and composited in the bottom-right corner.
  const int size = gizmoPixels(dpr);
  GizmoTarget& gizmo = gizmo_target_;
  gizmo.queued.reset();
  if (gizmo.target != nullptr && resizeGizmoTarget(QSize(size, size))) {
    std::array<float, 4> rect = clipRect(
        QRectF(output_px.width() - size, output_px.height() - size, size, size), output_px, conventions.y_up_in_ndc);
    if (renderTargetIsBottomUp(conventions)) {
      std::swap(rect[1], rect[3]);  // the texture's row 0 is the gizmo's bottom row
    }
    ImageGpu& quad = gizmo.quad;
    if (!quad.rect_written || rect != quad.rect) {
      updates->updateDynamicBuffer(quad.uniform_buffer, 0, kImageUniformBytes, rect.data());
      quad.rect = rect;
      quad.rect_written = true;
    }
    const GizmoKey key{glm::mat3(view), size};
    if (gizmo.drawn != key) {
      const glm::mat4 rotation{key.rotation};
      const glm::mat4 projection = glm::ortho(-1.3F, 1.3F, -1.3F, 1.3F, -10.0F, 10.0F);
      GizmoUniforms uniforms;
      copyMatrix(rhi_->clipSpaceCorrMatrix() * toQMatrix(projection * rotation), uniforms.view_projection);
      copyMatrix(glm::mat4(1.0F), uniforms.fixed_from_source);
      copyMatrix(rotation, uniforms.view);
      updates->updateDynamicBuffer(gizmo_uniform_buffer_, 0, kGizmoUniformBytes, &uniforms);
      gizmo.queued = key;
    }
  }

  for (std::size_t slot = 0; slot < kSlotCount; ++slot) {
    ImageGpu& gpu = images_[slot];
    const Image& source = images[slot];
    gpu.visible = !source.image.isNull() && ensureImage(updates, gpu, source.image);
    if (!gpu.visible) {
      continue;
    }
    // Snap to output pixels so the 1:1 texel mapping stays crisp.
    const QPointF top_left(std::round(source.top_left.x() * dpr), std::round(source.top_left.y() * dpr));
    const std::array<float, 4> rect =
        clipRect(QRectF(top_left, QSizeF(source.image.size())), output_px, conventions.y_up_in_ndc);
    if (!gpu.rect_written || rect != gpu.rect) {
      updates->updateDynamicBuffer(gpu.uniform_buffer, 0, kImageUniformBytes, rect.data());
      gpu.rect = rect;
      gpu.rect_written = true;
    }
  }
}

QRhiResourceUpdateBatch* OverlayRhiPass::submitGizmoTarget(
    QRhiCommandBuffer* command_buffer, QRhiResourceUpdateBatch* updates) {
  GizmoTarget& gizmo = gizmo_target_;
  if (!ready() || !gizmo.queued.has_value()) {
    return updates;
  }
  const auto size = static_cast<float>(gizmo.queued->size);
  command_buffer->beginPass(gizmo.target, Qt::transparent, {1.0F, 0}, updates);
  command_buffer->setGraphicsPipeline(gizmo.pipeline.get());
  command_buffer->setViewport(QRhiViewport(0.0F, 0.0F, size, size));
  command_buffer->setShaderResources(gizmo_shader_resources_);
  const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
      QRhiCommandBuffer::VertexInput(gizmo_arrow_vertices_, 0),
      QRhiCommandBuffer::VertexInput(gizmo_instance_buffer_, 0),
  };
  command_buffer->setVertexInput(
      0, static_cast<int>(inputs.size()), inputs.data(), gizmo_arrow_indices_, 0, QRhiCommandBuffer::IndexUInt32);
  command_buffer->drawIndexed(static_cast<quint32>(gizmoArrowMesh().indices.size()), kGizmoArmCount);
  command_buffer->endPass();
  gizmo.drawn = gizmo.queued;
  gizmo.queued.reset();
  ++gizmo_pass_count_;
  return nullptr;
}

void OverlayRhiPass::submit(QRhiCommandBuffer* command_buffer) const {
  if (!ready()) {
    return;
  }
  const auto draw = [&](const ImageGpu& gpu) {
    command_buffer->setGraphicsPipeline(image_pipeline_.get());
    command_buffer->setViewport(output_viewport_);
    command_buffer->setShaderResources(gpu.shader_resources);
    command_buffer->draw(4U);
  };
  if (gizmo_target_.drawn.has_value()) {
    draw(gizmo_target_.quad);
  }
  for (const ImageGpu& gpu : images_) {
    if (gpu.visible) {
      draw(gpu);
    }
  }
}

void OverlayRhiPass::releaseImage(ImageGpu& gpu) {
  delete gpu.shader_resources;
  delete gpu.texture;
  delete gpu.uniform_buffer;
  gpu = {};
}

void OverlayRhiPass::release() {
  releaseGizmoTarget();
  for (ImageGpu& gpu : images_) {
    releaseImage(gpu);
  }
  image_pipeline_.reset();
  delete image_layout_shader_resources_;
  delete image_layout_texture_;
  delete image_layout_uniform_buffer_;
  delete image_sampler_;
  delete gizmo_shader_resources_;
  delete gizmo_instance_buffer_;
  delete gizmo_arrow_indices_;
  delete gizmo_arrow_vertices_;
  delete gizmo_uniform_buffer_;
  image_layout_shader_resources_ = nullptr;
  image_layout_texture_ = nullptr;
  image_layout_uniform_buffer_ = nullptr;
  image_sampler_ = nullptr;
  gizmo_shader_resources_ = nullptr;
  gizmo_instance_buffer_ = nullptr;
  gizmo_arrow_indices_ = nullptr;
  gizmo_arrow_vertices_ = nullptr;
  gizmo_uniform_buffer_ = nullptr;
  gizmo_pass_count_ = 0;
  rhi_ = nullptr;
}

}  // namespace pj::scene3d
