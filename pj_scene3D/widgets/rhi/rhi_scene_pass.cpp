// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "rhi_scene_pass.h"

#include <rhi/qshader.h>

#include <QElapsedTimer>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "pj_scene3d_widgets/cube_mesh.h"
#include "pj_scene3d_widgets/gizmos/arrow_mesh.h"

namespace pj::scene3d {
namespace {

#ifdef PJ_RHI_TEST_HOOKS
bool injectedPipelineFailure(const QRhiGraphicsPipeline& pipeline) {
  const QString shader = qEnvironmentVariable("PJ_SCENE3D_TEST_FAIL_SHADER");
  return !shader.isEmpty() &&
         std::any_of(pipeline.cbeginShaderStages(), pipeline.cendShaderStages(), [&](const QRhiShaderStage& stage) {
           return PJ::rhiShaderPathForTest(stage.shader()).endsWith(shader);
         });
}
#endif

const ArrowMeshData& unitPoseArrowMesh() {
  static const ArrowMeshData mesh = buildArrowMesh(
      ArrowMeshParams{
          .length = 1.0F, .shaft_radius = 0.04F, .head_length = 0.30F, .head_radius = 0.10F, .segments = 16});
  return mesh;
}

}  // namespace

bool createGraphicsPipeline(QRhiGraphicsPipeline* pipeline) {
#ifdef PJ_RHI_TEST_HOOKS
  if (injectedPipelineFailure(*pipeline)) {
    return false;
  }
  const bool log = qEnvironmentVariableIntValue("PJ_SCENE3D_PIPELINE_LOG") == 1;
  QElapsedTimer timer;
  if (log) {
    timer.start();
  }
#endif
  const bool created = pipeline->create();
#ifdef PJ_RHI_TEST_HOOKS
  if (log) {
    QStringList shaders;
    for (auto stage = pipeline->cbeginShaderStages(); stage != pipeline->cendShaderStages(); ++stage) {
      shaders << PJ::rhiShaderPathForTest(stage->shader()).section(u'/', -1);
    }
    qInfo(
        "PJ_PIPELINE %s samples=%d ms=%.3f", qPrintable(shaders.join(u'+')), pipeline->sampleCount(),
        static_cast<double>(timer.nsecsElapsed()) / 1.0e6);
  }
#endif
  return created;
}

QRhiGraphicsPipeline* clonePipelineForRenderPass(
    QRhi* owner, const QRhiGraphicsPipeline* source, QRhiRenderPassDescriptor* render_pass, int sample_count,
    bool coverage_alpha_only) {
  if (owner == nullptr || source == nullptr || render_pass == nullptr) {
    return nullptr;
  }
  QRhiGraphicsPipeline* pipeline = owner->newGraphicsPipeline();
  pipeline->setFlags(source->flags());
  pipeline->setTopology(source->topology());
  pipeline->setShaderStages(source->cbeginShaderStages(), source->cendShaderStages());
  pipeline->setVertexInputLayout(source->vertexInputLayout());
  pipeline->setShaderResourceBindings(source->shaderResourceBindings());
  std::vector<QRhiGraphicsPipeline::TargetBlend> target_blends(
      source->cbeginTargetBlends(), source->cendTargetBlends());
  if (target_blends.empty()) {
    target_blends.emplace_back();
  }
  if (coverage_alpha_only) {
    target_blends.front().colorWrite = QRhiGraphicsPipeline::A;
  }
  pipeline->setTargetBlends(target_blends.cbegin(), target_blends.cend());
  pipeline->setCullMode(source->cullMode());
  pipeline->setFrontFace(source->frontFace());
  pipeline->setDepthTest(source->hasDepthTest());
  pipeline->setDepthWrite(source->hasDepthWrite());
  pipeline->setDepthOp(source->depthOp());
  pipeline->setDepthBias(source->depthBias());
  pipeline->setSlopeScaledDepthBias(source->slopeScaledDepthBias());
  pipeline->setSampleCount(sample_count);
  pipeline->setRenderPassDescriptor(render_pass);
  if (!createGraphicsPipeline(pipeline)) {
    delete pipeline;
    return nullptr;
  }
  return pipeline;
}

void ScenePipelineRegistry::add(QRhiGraphicsPipeline* direct) {
  if (direct == nullptr || std::any_of(entries_.cbegin(), entries_.cend(), [direct](const Entry& entry) {
        return entry.direct == direct;
      })) {
    return;
  }
  entries_.push_back(Entry{direct});
}

QRhiGraphicsPipeline* ScenePipelineRegistry::addTemplate(QRhiGraphicsPipeline* direct, const char* part) {
  if (direct != nullptr) {
    entries_.push_back(Entry{direct, nullptr, nullptr, part != nullptr ? part : "scene pipelines"});
  }
  return direct;
}

bool ScenePipelineRegistry::createTemplates() {
  if (deferred_failure_ != nullptr) {
    return false;  // not retried: the view fails on deferredFailure()
  }
  for (Entry& entry : entries_) {
    if (entry.uncreated_part == nullptr) {
      continue;
    }
    if (!createGraphicsPipeline(entry.direct)) {
      deferred_failure_ = entry.uncreated_part;
      return false;
    }
    entry.uncreated_part = nullptr;
  }
  return true;
}

void ScenePipelineRegistry::remove(QRhiGraphicsPipeline* direct) {
  const auto existing =
      std::find_if(entries_.begin(), entries_.end(), [direct](const Entry& entry) { return entry.direct == direct; });
  if (existing == entries_.end()) {
    return;
  }
  delete existing->depth;
  delete existing->hdr;
  entries_.erase(existing);
}

void ScenePipelineRegistry::setCloneTargets(
    QRhi* rhi, QRhiRenderPassDescriptor* hdr, int hdr_samples, QRhiRenderPassDescriptor* depth) {
  if (rhi != rhi_ || hdr != hdr_ || hdr_samples != hdr_samples_ || depth != depth_) {
    releaseClones();
    rhi_ = rhi;
    hdr_ = hdr;
    hdr_samples_ = hdr_samples;
    depth_ = depth;
  }
}

void ScenePipelineRegistry::releaseClones() {
  for (Entry& entry : entries_) {
    delete entry.depth;
    delete entry.hdr;
    entry.depth = nullptr;
    entry.hdr = nullptr;
  }
  clone_failed_ = false;
  rhi_ = nullptr;
  hdr_ = nullptr;
  depth_ = nullptr;
}

void ScenePipelineRegistry::clear() {
  releaseClones();
  entries_.clear();
  deferred_failure_ = nullptr;
}

bool ScenePipelineRegistry::directsBuiltFor(const QRhiRenderPassDescriptor* render_pass, int sample_count) const {
  return std::all_of(entries_.cbegin(), entries_.cend(), [&](const Entry& entry) {
    return pipelineBuiltFor(entry.direct, render_pass, sample_count);
  });
}

QRhiGraphicsPipeline* ScenePipelineRegistry::resolve(QRhiGraphicsPipeline* direct, ScenePass pass) const {
  if (pass == ScenePass::kDirect) {
    return direct;
  }
  if (pass == ScenePass::kDepthReplay && direct != nullptr && !direct->hasDepthWrite()) {
    // The composite finds the background by depth, so a draw that writes no
    // depth leaves no coverage there: the pixels it tints over the background
    // stay ungraded background, and SSAO/EDL never see it.
    return nullptr;
  }
  const auto existing =
      std::find_if(entries_.begin(), entries_.end(), [direct](const Entry& entry) { return entry.direct == direct; });
  if (existing == entries_.end() || rhi_ == nullptr) {
    return nullptr;
  }
  const bool depth_replay = pass == ScenePass::kDepthReplay;
  QRhiGraphicsPipeline*& clone = depth_replay ? existing->depth : existing->hdr;
  if (clone == nullptr) {
    clone = depth_replay ? clonePipelineForRenderPass(rhi_, direct, depth_, 1, true)
                         : clonePipelineForRenderPass(rhi_, direct, hdr_, hdr_samples_);
    clone_failed_ = clone_failed_ || clone == nullptr;
  }
  return clone;
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

QRhiGraphicsPipeline* createScenePipeline(const RhiPassSetup& setup, const PipelineDesc& desc) {
  QRhiGraphicsPipeline* pipeline = newScenePipeline(setup, desc);
  if (setup.templates != nullptr) {
    return setup.templates->addTemplate(pipeline, setup.part);
  }
  if (pipeline != nullptr && !createGraphicsPipeline(pipeline)) {
    delete pipeline;
    return nullptr;
  }
  return pipeline;
}

QRhiGraphicsPipeline* newScenePipeline(const RhiPassSetup& setup, const PipelineDesc& desc) {
  if (setup.rhi == nullptr || !desc.vertex.isValid() || !desc.fragment.isValid() || desc.bindings == nullptr) {
    return nullptr;
  }
  QRhiGraphicsPipeline* pipeline = setup.rhi->newGraphicsPipeline();
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
  pipeline->setSampleCount(setup.sample_count);
  pipeline->setRenderPassDescriptor(setup.render_pass);
  return pipeline;
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
