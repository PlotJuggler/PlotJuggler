// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "rhi_scene_pass.h"

#include <rhi/qshader.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "pj_scene3d_widgets/cube_mesh.h"
#include "pj_scene3d_widgets/gizmos/arrow_mesh.h"

namespace pj::scene3d {
namespace {

const ArrowMeshData& unitPoseArrowMesh() {
  static const ArrowMeshData mesh = buildArrowMesh(
      ArrowMeshParams{
          .length = 1.0F, .shaft_radius = 0.04F, .head_length = 0.30F, .head_radius = 0.10F, .segments = 16});
  return mesh;
}

}  // namespace

std::unique_ptr<QRhiGraphicsPipeline> createPipeline(
    QRhi* rhi, const PipelineDesc& desc, QRhiRenderPassDescriptor* render_pass, int sample_count) {
  if (rhi == nullptr || render_pass == nullptr || !desc.vertex.isValid() || !desc.fragment.isValid() ||
      desc.bindings == nullptr) {
    return nullptr;
  }
  std::unique_ptr<QRhiGraphicsPipeline> pipeline(rhi->newGraphicsPipeline());
  pipeline->setFlags(desc.flags);
  pipeline->setTopology(desc.topology);
  pipeline->setShaderStages({{QRhiShaderStage::Vertex, desc.vertex}, {QRhiShaderStage::Fragment, desc.fragment}});
  pipeline->setVertexInputLayout(desc.input_layout);
  pipeline->setShaderResourceBindings(desc.bindings);
  if (desc.blend.has_value()) {
    pipeline->setTargetBlends({*desc.blend});
  }
  pipeline->setCullMode(desc.cull);
  pipeline->setDepthTest(desc.depth_test);
  pipeline->setDepthWrite(desc.depth_write);
  pipeline->setDepthOp(desc.depth_op);
  pipeline->setDepthBias(desc.depth_bias);
  pipeline->setSlopeScaledDepthBias(desc.slope_scaled_depth_bias);
  pipeline->setSampleCount(sample_count);
  pipeline->setRenderPassDescriptor(render_pass);
  if (!PJ::createRhiPipeline(*pipeline)) {
    return nullptr;
  }
  return pipeline;
}

struct ScenePipeline {
  PipelineDesc desc;
  const char* part = nullptr;
  bool optional = false;
  mutable bool failed = false;  // optional only: a variant could not be created
  std::unique_ptr<QRhiGraphicsPipeline> direct;
  // The HDR variants are a cache resolve() fills, hence mutable.
  mutable std::unique_ptr<QRhiGraphicsPipeline> hdr;
  mutable std::unique_ptr<QRhiGraphicsPipeline> depth;
};

ScenePipelineRegistry::ScenePipelineRegistry() = default;
ScenePipelineRegistry::~ScenePipelineRegistry() = default;

void ScenePipelineRegistry::setDirectTarget(QRhi* rhi, QRhiRenderPassDescriptor* render_pass, int sample_count) {
  rhi_ = rhi;
  direct_ = render_pass;
  direct_samples_ = sample_count;
}

const ScenePipeline* ScenePipelineRegistry::add(const PipelineDesc& desc, const char* part, bool optional) {
  if (!desc.vertex.isValid() || !desc.fragment.isValid() || desc.bindings == nullptr) {
    return nullptr;
  }
  auto& pipeline = pipelines_.emplace_back(std::make_unique<ScenePipeline>());
  pipeline->desc = desc;
  pipeline->part = part != nullptr ? part : "scene pipelines";
  pipeline->optional = optional;
  return pipeline.get();
}

bool ScenePipelineRegistry::optionalFailed(const ScenePipeline* pipeline) const {
  return pipeline != nullptr && pipeline->failed;
}

void ScenePipelineRegistry::remove(const ScenePipeline* pipeline) {
  std::erase_if(pipelines_, [pipeline](const auto& entry) { return entry.get() == pipeline; });
}

bool ScenePipelineRegistry::createDirects() {
  if (failure_.has_value() && failure_->severity == PipelineFailure::Severity::kRequired) {
    return false;  // not retried: the view fails on failure()
  }
  for (const auto& pipeline : pipelines_) {
    if (pipeline->direct != nullptr || pipeline->failed) {
      continue;
    }
    pipeline->direct = createPipeline(rhi_, pipeline->desc, direct_, direct_samples_);
    if (pipeline->direct == nullptr && pipeline->optional) {
      pipeline->failed = true;
    } else if (pipeline->direct == nullptr) {
      failure_ = PipelineFailure{PipelineFailure::Severity::kRequired, pipeline->part};
      return false;
    }
  }
  return true;
}

void ScenePipelineRegistry::setVariantTargets(
    QRhiRenderPassDescriptor* hdr, int hdr_samples, QRhiRenderPassDescriptor* depth) {
  if (hdr != hdr_ || hdr_samples != hdr_samples_ || depth != depth_) {
    releaseVariants();
    hdr_ = hdr;
    hdr_samples_ = hdr_samples;
    depth_ = depth;
  }
}

void ScenePipelineRegistry::releaseVariants() {
  for (const auto& pipeline : pipelines_) {
    pipeline->depth.reset();
    pipeline->hdr.reset();
  }
  if (failure_.has_value() && failure_->severity == PipelineFailure::Severity::kHdrVariant) {
    failure_.reset();
  }
  hdr_ = nullptr;
  depth_ = nullptr;
}

void ScenePipelineRegistry::clear() {
  releaseVariants();
  pipelines_.clear();
  failure_.reset();
  rhi_ = nullptr;
  direct_ = nullptr;
  direct_samples_ = 1;
}

bool ScenePipelineRegistry::directsBuiltFor(const QRhiRenderPassDescriptor* render_pass, int sample_count) const {
  return std::all_of(pipelines_.cbegin(), pipelines_.cend(), [&](const auto& pipeline) {
    return pipelineBuiltFor(pipeline->direct.get(), render_pass, sample_count);
  });
}

ScenePipelineRegistry::Counts ScenePipelineRegistry::counts() const {
  Counts counts;
  for (const auto& pipeline : pipelines_) {
    ++counts.registered;
    counts.directs += pipeline->direct != nullptr ? 1 : 0;
    counts.hdr_variants += (pipeline->hdr != nullptr ? 1 : 0) + (pipeline->depth != nullptr ? 1 : 0);
  }
  return counts;
}

QRhiGraphicsPipeline* ScenePipelineRegistry::resolve(const ScenePipeline* pipeline, ScenePass pass) const {
  if (pipeline == nullptr || pipeline->failed) {
    return nullptr;
  }
  if (pass != ScenePass::kDirect && failure_.has_value()) {
    // The frame is withheld and the next one drops the HDR chain: no compile
    // is retried on every later bind of this frame.
    return nullptr;
  }
  switch (pass) {
    case ScenePass::kDirect:
      return pipeline->direct.get();
    case ScenePass::kHdrColor:
      if (pipeline->hdr == nullptr) {
        pipeline->hdr = createVariant(*pipeline, pass);
      }
      return pipeline->hdr.get();
    case ScenePass::kDepthReplay:
      // The composite finds the background by depth, so a draw that writes no
      // depth leaves no coverage there: the pixels it tints over the background
      // stay ungraded background, and SSAO/EDL never see it.
      if (!pipeline->desc.depth_write) {
        return nullptr;
      }
      if (pipeline->depth == nullptr) {
        pipeline->depth = createVariant(*pipeline, pass);
      }
      return pipeline->depth.get();
  }
  return nullptr;
}

std::unique_ptr<QRhiGraphicsPipeline> ScenePipelineRegistry::createVariant(
    const ScenePipeline& pipeline, ScenePass pass) const {
  if (hdr_ == nullptr) {
    return nullptr;
  }
  std::unique_ptr<QRhiGraphicsPipeline> variant;
  if (pass == ScenePass::kDepthReplay) {
    // The replay's coverage: the colour target takes alpha only.
    PipelineDesc desc = pipeline.desc;
    QRhiGraphicsPipeline::TargetBlend blend = desc.blend.value_or(QRhiGraphicsPipeline::TargetBlend{});
    blend.colorWrite = QRhiGraphicsPipeline::A;
    desc.blend = blend;
    variant = createPipeline(rhi_, desc, depth_, 1);
  } else {
    variant = createPipeline(rhi_, pipeline.desc, hdr_, hdr_samples_);
  }
  if (variant == nullptr && pipeline.optional) {
    pipeline.failed = true;
  } else if (variant == nullptr) {
    failure_ = PipelineFailure{PipelineFailure::Severity::kHdrVariant, pipeline.part};
  }
  return variant;
}

namespace {

// The box around `source`'s eight corners mapped by `place` (double in, double
// out) minus `origin`, skipping non-finite corners. The origin is subtracted in
// double: casting world coordinates to float first would collapse a far-away
// box before it is made camera-relative.
template <typename Place>
AABB placedCorners(const AABB& source, const glm::dvec3& origin, Place place) {
  if (!source.valid) {
    return {};
  }
  AABB output;
  for (int corner = 0; corner < 8; ++corner) {
    const glm::dvec3 point{
        (corner & 1) != 0 ? source.max.x : source.min.x,
        (corner & 2) != 0 ? source.max.y : source.min.y,
        (corner & 4) != 0 ? source.max.z : source.min.z,
    };
    const glm::dvec3 transformed = place(point) - origin;
    if (std::isfinite(transformed.x) && std::isfinite(transformed.y) && std::isfinite(transformed.z)) {
      expandAABB(output, glm::vec3(transformed));
    }
  }
  return output;
}

}  // namespace

AABB transformedBounds(const AABB& source, const Transform& fixed_from_source, const glm::dvec3& origin) {
  return placedCorners(source, origin, [&](const glm::dvec3& point) { return fixed_from_source * point; });
}

AABB transformedBounds(const AABB& source, const glm::dmat4& fixed_from_source, const glm::dvec3& origin) {
  return placedCorners(
      source, origin, [&](const glm::dvec3& point) { return glm::dvec3(fixed_from_source * glm::dvec4(point, 1.0)); });
}

bool ensureStaticVertexBuffer(
    QRhi* owner, QRhiBuffer*& buffer, quint32& capacity, quint32 required_bytes, quint32 minimum_capacity,
    QRhiBuffer::Type type) {
  if (required_bytes == 0) {
    return true;
  }
  if (buffer != nullptr && buffer->type() == type && capacity >= required_bytes &&
      capacity <= std::max<quint32>(required_bytes, minimum_capacity / 2U) * 2U) {
    return true;
  }
  delete buffer;
  capacity = std::max(required_bytes, minimum_capacity);
  buffer = owner->newBuffer(type, QRhiBuffer::VertexBuffer, capacity);
  if (!buffer->create()) {
    delete buffer;
    buffer = nullptr;
    capacity = 0;
    return false;
  }
  return true;
}

bool ensureVertexBuffer(
    QRhi* owner, QRhiBuffer*& buffer, quint32& capacity, quint32 required_bytes, quint32 minimum_capacity) {
  if (required_bytes == 0) {
    return true;
  }
  if (buffer != nullptr && capacity >= required_bytes) {
    return true;
  }
  quint32 next_capacity = std::max(capacity, minimum_capacity);
  while (next_capacity < required_bytes && next_capacity <= std::numeric_limits<quint32>::max() / 2U) {
    next_capacity *= 2U;
  }
  if (next_capacity < required_bytes) {
    return false;
  }
  delete buffer;
  buffer = owner->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, next_capacity);
  if (!buffer->create()) {
    delete buffer;
    buffer = nullptr;
    capacity = 0;
    return false;
  }
  capacity = next_capacity;
  return true;
}

bool ensureUniformBuffer(QRhi* owner, QRhiBuffer*& buffer, quint32 bytes) {
  if (buffer != nullptr) {
    return true;
  }
  buffer = owner->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, bytes);
  if (!buffer->create()) {
    delete buffer;
    buffer = nullptr;
    return false;
  }
  return true;
}

QRhiShaderResourceBindings* createBindings(QRhi* owner, std::initializer_list<QRhiShaderResourceBinding> bindings) {
  QRhiShaderResourceBindings* resources = owner->newShaderResourceBindings();
  resources->setBindings(bindings.begin(), bindings.end());
  if (!resources->create()) {
    delete resources;
    return nullptr;
  }
  return resources;
}

bool createStaticMesh(
    QRhi* rhi, QRhiResourceUpdateBatch* updates, const ArrowMeshData& mesh, QRhiBuffer*& vertices,
    QRhiBuffer*& indices) {
  const auto bytes = [](const auto& values) { return static_cast<quint32>(values.size() * sizeof(values.front())); };
  vertices = rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, bytes(mesh.vertices));
  indices = rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, bytes(mesh.indices));
  if (!vertices->create() || !indices->create()) {
    return false;
  }
  updates->uploadStaticBuffer(vertices, mesh.vertices.data());
  updates->uploadStaticBuffer(indices, mesh.indices.data());
  return true;
}

bool RhiSharedGeometry::create(QRhi* rhi, QRhiResourceUpdateBatch* updates) {
  cube_vertices = rhi->newBuffer(
      QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, static_cast<quint32>(kCubeVertices.size() * sizeof(CubeVertex)));
  cube_indices = rhi->newBuffer(
      QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer,
      static_cast<quint32>(kCubeIndices.size() * sizeof(std::uint16_t)));
  cube_fan_indices = rhi->newBuffer(
      QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer,
      static_cast<quint32>(kCubeFanIndices.size() * sizeof(std::uint16_t)));
  if (!cube_vertices->create() || !cube_indices->create() || !cube_fan_indices->create() ||
      !createStaticMesh(rhi, updates, unitPoseArrowMesh(), arrow_vertices, arrow_indices)) {
    return false;
  }
  arrow_index_count = static_cast<quint32>(unitPoseArrowMesh().indices.size());
  updates->uploadStaticBuffer(cube_vertices, kCubeVertices.data());
  updates->uploadStaticBuffer(cube_indices, kCubeIndices.data());  // already uint16, as QRhi wants
  updates->uploadStaticBuffer(cube_fan_indices, kCubeFanIndices.data());
  return true;
}

void RhiSharedGeometry::release() {
  delete arrow_indices;
  delete arrow_vertices;
  delete cube_fan_indices;
  delete cube_indices;
  delete cube_vertices;
  *this = {};
}

}  // namespace pj::scene3d
