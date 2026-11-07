// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "trail_rhi_pass.h"

#include <algorithm>
#include <array>
#include <limits>

#include "pj_scene3d_widgets/layers/poses_in_frame_layer.h"
#include "pj_scene3d_widgets/layers/trail_layer.h"

namespace pj::scene3d {
namespace {

struct alignas(16) TrailUniforms {
  std::array<float, 16> view_projection{};
  std::array<float, 4> color{};
  std::array<float, 4> params{};
};
constexpr quint32 kUniformBytes = sizeof(TrailUniforms);
// Position, tangent, side: 7 floats per strip vertex, two vertices per point.
constexpr std::size_t kFloatsPerVertex = 7;
constexpr quint32 kMinimumVertexBytes = 16U * 1024U;
constexpr auto kStages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;

glm::dvec3 safeNormalize(const glm::dvec3& vector) {
  const double length = glm::length(vector);
  return length > 1e-12 ? vector / length : glm::dvec3(0.0);
}

// The ribbon vertices of `points`, camera-relative. Joint tangent: the mean of
// the adjacent segment directions (endpoints take their lone segment).
void buildRibbon(const std::vector<glm::dvec3>& points, const glm::dvec3& origin, std::vector<float>& out) {
  const std::size_t total = points.size();
  out.clear();
  out.reserve(total * 2U * kFloatsPerVertex);
  for (std::size_t index = 0; index < total; ++index) {
    glm::dvec3 direction(0.0);
    if (index + 1 < total) {
      direction += safeNormalize(points[index + 1] - points[index]);
    }
    if (index > 0) {
      direction += safeNormalize(points[index] - points[index - 1]);
    }
    glm::dvec3 unit = safeNormalize(direction);
    if (unit == glm::dvec3(0.0)) {
      unit = glm::dvec3(1.0, 0.0, 0.0);
    }
    const glm::vec3 position(points[index] - origin);  // double subtract, then downcast
    const glm::vec3 tangent(unit);
    for (const float side : {1.0F, -1.0F}) {
      out.insert(out.end(), {position.x, position.y, position.z, tangent.x, tangent.y, tangent.z, side});
    }
  }
}

}  // namespace

bool TrailRhiPass::initialize(const RhiPassSetup& setup) {
  QRhi* rhi = setup.rhi;
  if (rhi == nullptr) {
    return false;
  }
  block_stride_ = static_cast<quint32>(rhi->ubufAligned(static_cast<int>(kUniformBytes)));
  if (!ensureUniformBuffer(rhi, layout_uniform_buffer_, kUniformBytes)) {
    return false;
  }
  layout_resources_ =
      createBindings(rhi, {QRhiShaderResourceBinding::uniformBuffer(0, kStages, layout_uniform_buffer_)});
  QRhiVertexInputLayout layout;
  layout.setBindings({QRhiVertexInputBinding(kFloatsPerVertex * sizeof(float))});
  layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0U),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, 3U * sizeof(float)),
      QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float, 6U * sizeof(float)),
  });
  pipeline_ = setup.pipelines->add(
      {
          .topology = QRhiGraphicsPipeline::TriangleStrip,
          .vertex = loadRhiShader(QStringLiteral(":/scene3d_rhi/trail.vert.qsb")),
          .fragment = loadRhiShader(QStringLiteral(":/scene3d_rhi/trail.frag.qsb")),
          .input_layout = layout,
          .bindings = layout_resources_,
      },
      setup.part);
  return pipeline_ != nullptr;
}

void TrailRhiPass::setLayers(const std::vector<Owner>& owners) {
  // adoptLayerList() for registered owners: an owner's GPU state goes when it
  // leaves (a hidden trail's is pruned in prepare()).
  std::erase_if(owner_gpu_, [&owners](auto& entry) {
    const bool gone = std::none_of(
        owners.cbegin(), owners.cend(), [&entry](const Owner& owner) { return owner.layer == entry.first; });
    if (gone) {
      releaseOwnerGpu(entry.second);
    }
    return gone;
  });
  owners_ = owners;
}

TrailLayer* TrailRhiPass::trailOf(const Owner& owner) {
  if (owner.trail != nullptr) {
    return owner.trail;
  }
  return owner.poses != nullptr ? owner.poses->trail() : nullptr;
}

void TrailRhiPass::prepare(
    const RhiFrameContext& frame, const std::string& fixed_frame, PJ::Timepoint time, const QSize& render_size,
    float render_scale) {
  if (!ready()) {
    return;  // never built: no layer of this family since the last release
  }
  draws_.clear();
  last_trail_count_ = 0;
  last_vertex_count_ = 0;
  if (render_size.isEmpty()) {
    return;
  }
  for (const Owner& entry : owners_) {
    Scene3DLayer* owner = entry.layer;
    TrailLayer* trail = trailOf(entry);
    if (trail == nullptr || !trail->visible()) {
      pruneLayerGpu(owner_gpu_, owner, releaseOwnerGpu);
      continue;
    }
    if (!trail->syncFixedFrame(fixed_frame) || trail->points().size() < 2 ||
        (!trail->pastVisible() && !trail->futureVisible())) {
      continue;
    }
    const std::vector<glm::dvec3>& points = trail->points();
    if (points.size() * 2U * kFloatsPerVertex * sizeof(float) > std::numeric_limits<quint32>::max()) {
      continue;
    }
    OwnerGpu& gpu = owner_gpu_[owner];
    const auto vertex_bytes = static_cast<quint32>(points.size() * 2U * kFloatsPerVertex * sizeof(float));
    if (!ensureOwnerGpu(frame.rhi, gpu, vertex_bytes)) {
      continue;
    }
    // Re-upload only when the polyline, the camera-relative origin or the trail
    // itself changed: scrubbing moves just the two draw ranges.
    if (gpu.trail != trail || gpu.uploaded_revision != trail->buildRevision() ||
        gpu.uploaded_origin != frame.render_origin) {
      buildRibbon(points, frame.render_origin, scratch_);
      PJ::uploadStaticBufferInPieces(frame.updates, gpu.vertex_buffer, 0, vertex_bytes, scratch_.data());
      gpu.trail = trail;
      gpu.uploaded_revision = trail->buildRevision();
      gpu.uploaded_origin = frame.render_origin;
    }

    const float half_width = 0.5F * std::max(trail->thickness(), 1.0F) * render_scale;
    const std::array<float, 4> params = {
        static_cast<float>(render_size.width()), static_cast<float>(render_size.height()), half_width, 0.0F};
    for (const auto& [offset, color] :
         {std::pair{0U, trail->pastColor()}, std::pair{block_stride_, trail->futureColor()}}) {
      TrailUniforms uniforms;
      copyMatrix(frame.view_projection, uniforms.view_projection);
      uniforms.color = {
          static_cast<float>(color.redF()), static_cast<float>(color.greenF()), static_cast<float>(color.blueF()),
          1.0F};
      uniforms.params = params;
      frame.updates->updateDynamicBuffer(gpu.uniform_buffer, offset, kUniformBytes, &uniforms);
    }

    const std::size_t total = points.size();
    const std::size_t past = std::min(trail->pastCount(time), total);
    const std::size_t future_first = past > 0 ? past - 1 : 0;
    const std::size_t future_points = total - future_first;
    Draw draw{owner, gpu.vertex_buffer, gpu.past_resources, gpu.future_resources};
    if (trail->pastVisible() && past >= 2) {
      draw.past_vertex_count = static_cast<quint32>(2U * past);
    }
    if (trail->futureVisible() && future_points >= 2) {
      draw.future_first_vertex = static_cast<quint32>(2U * future_first);
      draw.future_vertex_count = static_cast<quint32>(2U * future_points);
    }
    if (draw.past_vertex_count == 0U && draw.future_vertex_count == 0U) {
      continue;
    }
    draws_.push_back(draw);
    ++last_trail_count_;
    last_vertex_count_ += static_cast<int>(draw.past_vertex_count + draw.future_vertex_count);
  }
}

bool TrailRhiPass::submit(
    QRhiCommandBuffer* command_buffer, const Draw& draw, ScenePass pass, const ScenePipelineRegistry& registry,
    const QRhiViewport& viewport) const {
  QRhiGraphicsPipeline* pipeline = registry.resolve(pipeline_, pass);
  if (pipeline == nullptr) {
    return false;
  }
  const auto record = [&](QRhiShaderResourceBindings* resources, quint32 first, quint32 count) {
    if (count == 0U) {
      return;
    }
    command_buffer->setGraphicsPipeline(pipeline);
    command_buffer->setViewport(viewport);
    command_buffer->setShaderResources(resources);
    const QRhiCommandBuffer::VertexInput input(draw.vertex_buffer, 0);
    command_buffer->setVertexInput(0, 1, &input);
    command_buffer->draw(count, 1U, first, 0U);
  };
  record(draw.past_resources, 0U, draw.past_vertex_count);
  record(draw.future_resources, draw.future_first_vertex, draw.future_vertex_count);
  return true;
}

bool TrailRhiPass::ensureOwnerGpu(QRhi* rhi, OwnerGpu& gpu, quint32 vertex_bytes) {
  const quint32 previous_capacity = gpu.vertex_capacity;
  if (!ensureStaticVertexBuffer(rhi, gpu.vertex_buffer, gpu.vertex_capacity, vertex_bytes, kMinimumVertexBytes)) {
    releaseOwnerGpu(gpu);
    return false;
  }
  if (gpu.vertex_capacity != previous_capacity) {
    gpu.trail = nullptr;  // a new buffer holds nothing yet
  }
  if (gpu.uniform_buffer == nullptr) {
    if (!ensureUniformBuffer(rhi, gpu.uniform_buffer, block_stride_ + kUniformBytes)) {
      releaseOwnerGpu(gpu);
      return false;
    }
    gpu.past_resources = createBindings(
        rhi, {QRhiShaderResourceBinding::uniformBuffer(0, kStages, gpu.uniform_buffer, 0U, kUniformBytes)});
    gpu.future_resources = createBindings(
        rhi, {QRhiShaderResourceBinding::uniformBuffer(0, kStages, gpu.uniform_buffer, block_stride_, kUniformBytes)});
    if (gpu.past_resources == nullptr || gpu.future_resources == nullptr) {
      releaseOwnerGpu(gpu);
      return false;
    }
  }
  return true;
}

void TrailRhiPass::releaseOwnerGpu(OwnerGpu& gpu) {
  delete gpu.past_resources;
  delete gpu.future_resources;
  delete gpu.uniform_buffer;
  delete gpu.vertex_buffer;
  gpu = {};
}

void TrailRhiPass::release() {
  for (auto& [owner, gpu] : owner_gpu_) {
    (void)owner;
    releaseOwnerGpu(gpu);
  }
  owner_gpu_.clear();
  draws_.clear();
  delete layout_resources_;
  delete layout_uniform_buffer_;
  pipeline_ = nullptr;
  layout_resources_ = nullptr;
  layout_uniform_buffer_ = nullptr;
  block_stride_ = 0;
}

}  // namespace pj::scene3d
