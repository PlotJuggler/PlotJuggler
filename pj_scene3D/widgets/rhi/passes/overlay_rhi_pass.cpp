// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "overlay_rhi_pass.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

#include "pj_scene3d_core/poses_in_frame_render.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"
#include "pose_rhi_pass.h"

namespace pj::scene3d {
namespace {

constexpr quint32 kGizmoUniformBytes = 192U;
constexpr quint32 kImageUniformBytes = 16U;
constexpr int kGizmoArmCount = 3;

struct alignas(16) GizmoUniforms {
  std::array<float, 16> view_projection{};
  std::array<float, 16> fixed_from_source{};
  std::array<float, 16> view{};
};
static_assert(sizeof(GizmoUniforms) == kGizmoUniformBytes);

// The GL AxisOverlayPass triad: arrow length 0.95, shaft radius 0.05, head
// radius 0.13, head length 0.30 in a [-1.3, 1.3] orthographic box. The view's
// unit arrow (length 1, shaft 0.04, head 0.10 x 0.30) is scaled to it: the
// head radius and length match exactly, the shaft is 4% thicker.
const glm::mat4& gizmoArrowScale() {
  static const glm::mat4 scale = glm::scale(glm::mat4(1.0F), glm::vec3(0.95F, 1.3F, 1.3F));
  return scale;
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

bool OverlayRhiPass::initialize(const RhiPassSetup& setup) {
  QRhi* rhi = setup.rhi;
  if (rhi == nullptr || setup.geometry == nullptr) {
    return false;
  }
  rhi_ = rhi;
  geometry_ = setup.geometry;

  gizmo_instance_buffer_ = rhi->newBuffer(
      QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
      kGizmoArmCount * static_cast<quint32>(sizeof(PoseTriadInstance)));
  if (!ensureUniformBuffer(rhi, gizmo_uniform_buffer_, kGizmoUniformBytes) || !gizmo_instance_buffer_->create()) {
    return false;
  }
  gizmo_arms_upload_pending_ = true;
  gizmo_shader_resources_ = createBindings(
      rhi,
      {QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, gizmo_uniform_buffer_)});
  // Keeps the output's alpha (Wayland treats a non-opaque surface as translucent).
  QRhiGraphicsPipeline::TargetBlend over = PJ::straightAlphaBlend();
  over.srcAlpha = QRhiGraphicsPipeline::Zero;
  over.dstAlpha = QRhiGraphicsPipeline::One;
  gizmo_pipeline_ = createScenePipeline(
      setup, {
                 .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/pose.vert.qsb")),
                 .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/gizmo.frag.qsb")),
                 .input_layout = poseTriadInputLayout(),
                 .bindings = gizmo_shader_resources_,
                 .blend = over,
                 .flags = QRhiGraphicsPipeline::UsesScissor,
             });

  image_sampler_ = rhi->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
      QRhiSampler::ClampToEdge);
  image_layout_texture_ = rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1));
  if (gizmo_pipeline_ == nullptr || !image_sampler_->create() ||
      !ensureUniformBuffer(rhi, image_layout_uniform_buffer_, kImageUniformBytes) || !image_layout_texture_->create()) {
    return false;
  }
  image_layout_shader_resources_ = createImageBindings(image_layout_uniform_buffer_, image_layout_texture_);
  // Premultiplied source-over; the output's alpha stays opaque.
  QRhiGraphicsPipeline::TargetBlend premultiplied = over;
  premultiplied.srcColor = QRhiGraphicsPipeline::One;
  image_pipeline_ = createScenePipeline(
      setup, {
                 .topology = QRhiGraphicsPipeline::TriangleStrip,
                 .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/overlay.vert.qsb")),
                 .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/overlay.frag.qsb")),
                 .bindings = image_layout_shader_resources_,
                 .blend = premultiplied,
                 .depth_test = false,
                 .depth_write = false,
             });
  return image_pipeline_ != nullptr;
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

  // Gizmo: the camera's rotation applied to a fixed orthographic triad in the
  // bottom-right corner. The viewport's depth range [0, 1e-4] keeps it in front
  // of whatever depth the output target holds (the direct pass leaves the
  // scene's) while its arms still occlude each other: no mid-pass depth clear.
  const int size = std::max(1, static_cast<int>(static_cast<double>(kGizmoSizePx) * dpr));
  const int x = output_px.width() - size;
  gizmo_viewport_ =
      QRhiViewport(static_cast<float>(x), 0.0F, static_cast<float>(size), static_cast<float>(size), 0.0F, 1.0e-4F);
  gizmo_scissor_ = QRhiScissor(x, 0, size, size);
  const glm::mat4 rotation{glm::mat3(view)};
  const glm::mat4 projection = glm::ortho(-1.3F, 1.3F, -1.3F, 1.3F, -10.0F, 10.0F);
  GizmoUniforms uniforms;
  copyMatrix(rhi_->clipSpaceCorrMatrix() * toQMatrix(projection * rotation), uniforms.view_projection);
  copyMatrix(glm::mat4(1.0F), uniforms.fixed_from_source);
  copyMatrix(rotation, uniforms.view);
  updates->updateDynamicBuffer(gizmo_uniform_buffer_, 0, kGizmoUniformBytes, &uniforms);
  if (gizmo_arms_upload_pending_) {
    // The scene's triad arms (unit length), rescaled to the GL gizmo's arrow and
    // recoloured with the saturated HUD triplets.
    std::vector<PoseTriadInstance> arms;
    appendTriadArms(glm::mat4(1.0F), PoseTriadStyle{.axis_length = 1.0F}, arms);
    const std::array<glm::vec3, kGizmoArmCount> colors = {look::kHudTriadX, look::kHudTriadY, look::kHudTriadZ};
    for (std::size_t arm = 0; arm < arms.size(); ++arm) {
      arms[arm].model = arms[arm].model * gizmoArrowScale();
      arms[arm].color = glm::vec4(colors[arm], 1.0F);
    }
    updates->uploadStaticBuffer(gizmo_instance_buffer_, arms.data());
    gizmo_arms_upload_pending_ = false;
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
        clipRect(QRectF(top_left, QSizeF(source.image.size())), output_px, rhiConventions(*rhi_).y_up_in_ndc);
    if (!gpu.rect_written || rect != gpu.rect) {
      updates->updateDynamicBuffer(gpu.uniform_buffer, 0, kImageUniformBytes, rect.data());
      gpu.rect = rect;
      gpu.rect_written = true;
    }
  }
}

void OverlayRhiPass::submit(QRhiCommandBuffer* command_buffer) const {
  if (!ready() || geometry_->arrow_vertices == nullptr || geometry_->arrow_indices == nullptr) {
    return;
  }
  command_buffer->setGraphicsPipeline(gizmo_pipeline_);
  command_buffer->setViewport(gizmo_viewport_);
  command_buffer->setScissor(gizmo_scissor_);
  command_buffer->setShaderResources(gizmo_shader_resources_);
  const std::array<QRhiCommandBuffer::VertexInput, 2> inputs = {
      QRhiCommandBuffer::VertexInput(geometry_->arrow_vertices, 0),
      QRhiCommandBuffer::VertexInput(gizmo_instance_buffer_, 0),
  };
  command_buffer->setVertexInput(
      0, static_cast<int>(inputs.size()), inputs.data(), geometry_->arrow_indices, 0, QRhiCommandBuffer::IndexUInt32);
  command_buffer->drawIndexed(geometry_->arrow_index_count, kGizmoArmCount);

  for (const ImageGpu& gpu : images_) {
    if (!gpu.visible) {
      continue;
    }
    command_buffer->setGraphicsPipeline(image_pipeline_);
    command_buffer->setViewport(output_viewport_);
    command_buffer->setShaderResources(gpu.shader_resources);
    command_buffer->draw(4U);
  }
}

void OverlayRhiPass::releaseImage(ImageGpu& gpu) {
  delete gpu.shader_resources;
  delete gpu.texture;
  delete gpu.uniform_buffer;
  gpu = {};
}

void OverlayRhiPass::release() {
  for (ImageGpu& gpu : images_) {
    releaseImage(gpu);
  }
  delete image_pipeline_;
  delete image_layout_shader_resources_;
  delete image_layout_texture_;
  delete image_layout_uniform_buffer_;
  delete image_sampler_;
  delete gizmo_pipeline_;
  delete gizmo_shader_resources_;
  delete gizmo_instance_buffer_;
  delete gizmo_uniform_buffer_;
  image_pipeline_ = nullptr;
  image_layout_shader_resources_ = nullptr;
  image_layout_texture_ = nullptr;
  image_layout_uniform_buffer_ = nullptr;
  image_sampler_ = nullptr;
  gizmo_pipeline_ = nullptr;
  gizmo_shader_resources_ = nullptr;
  gizmo_instance_buffer_ = nullptr;
  gizmo_uniform_buffer_ = nullptr;
  geometry_ = nullptr;
  rhi_ = nullptr;
  gizmo_arms_upload_pending_ = false;
}

}  // namespace pj::scene3d
