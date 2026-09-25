// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "point_aabb_reducer.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "pj_scene3d_core/aabb_gpu_key.h"
#include "pj_scene3d_widgets/point_renderable.h"

namespace pj::scene3d {
namespace {

// Result words: min keys xyz, max keys xyz, finite-point count, pad.
constexpr quint32 kResultBytes = 8U * sizeof(std::uint32_t);
// The Params block of point_aabb.comp (the point count), padded to one std140 vec4.
constexpr quint32 kParamsBytes = 16U;
// Matches kGroups in point_aabb.comp: every invocation strides over the cloud.
constexpr int kWorkGroups = 1024;
// min slots seed to the largest key so atomicMin reduces down; max and count to 0.
constexpr std::array<std::uint32_t, 8> kResultSeed = {0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0U, 0U, 0U, 0U, 0U};

}  // namespace

struct PointAabbReducer::Slot {
  enum class State { kIdle, kRequested, kInFlight };
  const void* key = nullptr;
  std::uint64_t revision = 0;
  QRhiBuffer* points = nullptr;
  quint32 count = 0;
  QRhiBuffer* bound_points = nullptr;  ///< what `bindings` reads
  quint32 bound_count = 0;
  QRhiBuffer* result = nullptr;
  QRhiBuffer* params = nullptr;
  QRhiShaderResourceBindings* bindings = nullptr;
  QRhiReadbackResult readback;
  State state = State::kIdle;
  bool forgotten = false;

  ~Slot() {
    delete bindings;
    delete params;
    delete result;
  }
};

PointAabbReducer::PointAabbReducer() = default;

PointAabbReducer::~PointAabbReducer() {
  release();
}

bool PointAabbReducer::initialize(QRhi* rhi) {
  if (rhi == nullptr || !rhi->isFeatureSupported(QRhi::Compute)) {
    return false;
  }
  const QShader shader = loadRhiShader(QStringLiteral(":/scene3d_rhi/point_aabb.comp.qsb"));
  if (!shader.isValid()) {
    return false;
  }
  rhi_ = rhi;
  // A QRhi destroyed first never completes the readbacks: forget it, so release()
  // neither waits on it nor touches it.
  rhi->addCleanupCallback(this, [this](QRhi*) { rhi_ = nullptr; });
  layout_points_ = rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, sizeof(PointVertex));
  layout_result_ = rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, kResultBytes);
  layout_params_ = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kParamsBytes);
  if (!layout_points_->create() || !layout_result_->create() || !layout_params_->create()) {
    release();
    return false;
  }
  layout_bindings_ = rhi->newShaderResourceBindings();
  layout_bindings_->setBindings({
      QRhiShaderResourceBinding::bufferLoad(0, QRhiShaderResourceBinding::ComputeStage, layout_points_),
      QRhiShaderResourceBinding::bufferLoadStore(1, QRhiShaderResourceBinding::ComputeStage, layout_result_),
      QRhiShaderResourceBinding::uniformBuffer(2, QRhiShaderResourceBinding::ComputeStage, layout_params_),
  });
  pipeline_ = rhi->newComputePipeline();
  pipeline_->setShaderStage({QRhiShaderStage::Compute, shader});
  pipeline_->setShaderResourceBindings(layout_bindings_);
  if (!layout_bindings_->create() || !pipeline_->create()) {
    release();
    return false;
  }
  return true;
}

void PointAabbReducer::request(const void* key, std::uint64_t revision, QRhiBuffer* points, quint32 count) {
  if (!available() || points == nullptr || count == 0U) {
    return;
  }
  auto found = std::find_if(
      slots_.begin(), slots_.end(), [key](const auto& slot) { return slot->key == key && !slot->forgotten; });
  if (found == slots_.end()) {
    slots_.push_back(std::make_unique<Slot>());
    found = std::prev(slots_.end());
    (*found)->key = key;
  }
  Slot& slot = **found;
  if (slot.state == Slot::State::kInFlight) {
    return;  // latest wins once it lands: the caller asks again for a newer revision
  }
  slot.revision = revision;
  slot.points = points;
  slot.count = count;
  slot.state = Slot::State::kRequested;
}

bool PointAabbReducer::record(QRhiCommandBuffer* command_buffer, QRhiResourceUpdateBatch* updates) {
  std::vector<Slot*> ready;
  for (const auto& slot : slots_) {
    if (slot->state != Slot::State::kRequested || slot->forgotten) {
      continue;
    }
    if (slot->result == nullptr) {
      slot->result = rhi_->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, kResultBytes);
      slot->params = rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kParamsBytes);
      if (!slot->result->create() || !slot->params->create()) {
        delete slot->result;
        delete slot->params;
        slot->result = nullptr;
        slot->params = nullptr;
        continue;
      }
    }
    if (slot->bindings == nullptr || slot->bound_points != slot->points || slot->bound_count != slot->count) {
      delete slot->bindings;
      slot->bindings = rhi_->newShaderResourceBindings();
      slot->bindings->setBindings({
          QRhiShaderResourceBinding::bufferLoad(
              0, QRhiShaderResourceBinding::ComputeStage, slot->points, 0,
              slot->count * static_cast<quint32>(sizeof(PointVertex))),
          QRhiShaderResourceBinding::bufferLoadStore(1, QRhiShaderResourceBinding::ComputeStage, slot->result),
          QRhiShaderResourceBinding::uniformBuffer(2, QRhiShaderResourceBinding::ComputeStage, slot->params),
      });
      if (!slot->bindings->create()) {
        delete slot->bindings;
        slot->bindings = nullptr;
        continue;
      }
      slot->bound_points = slot->points;
      slot->bound_count = slot->count;
    }
    ready.push_back(slot.get());
  }
  if (ready.empty()) {
    return false;
  }
  for (Slot* slot : ready) {
    updates->uploadStaticBuffer(slot->result, kResultSeed.data());
    const std::array<std::uint32_t, 4> params = {slot->count, 0U, 0U, 0U};
    updates->updateDynamicBuffer(slot->params, 0, kParamsBytes, params.data());
  }
  QRhiResourceUpdateBatch* readbacks = rhi_->nextResourceUpdateBatch();
  command_buffer->beginComputePass(updates);
  command_buffer->setComputePipeline(pipeline_);
  for (Slot* slot : ready) {
    command_buffer->setShaderResources(slot->bindings);
    command_buffer->dispatch(kWorkGroups, 1, 1);
    slot->state = Slot::State::kInFlight;
    slot->readback.completed = [this, slot] {
      slot->state = Slot::State::kIdle;
      if (slot->forgotten || slot->readback.data.size() < static_cast<qsizetype>(kResultBytes)) {
        return;
      }
      std::array<std::uint32_t, 8> words{};
      std::memcpy(words.data(), slot->readback.data.constData(), kResultBytes);
      Result result{slot->key, slot->revision, {}};
      if (words[6] != 0U) {
        result.bounds.min = {orderedKeyToFloat(words[0]), orderedKeyToFloat(words[1]), orderedKeyToFloat(words[2])};
        result.bounds.max = {orderedKeyToFloat(words[3]), orderedKeyToFloat(words[4]), orderedKeyToFloat(words[5])};
        result.bounds.valid = true;
      }
      results_.push_back(result);
    };
    readbacks->readBackBuffer(slot->result, 0, kResultBytes, &slot->readback);
  }
  command_buffer->endComputePass(readbacks);
  return true;
}

std::vector<PointAabbReducer::Result> PointAabbReducer::takeResults() {
  std::vector<Result> taken;
  taken.swap(results_);
  // A forgotten slot is reclaimed once its readback can no longer land.
  slots_.erase(
      std::remove_if(
          slots_.begin(), slots_.end(),
          [](const auto& slot) { return slot->forgotten && slot->state != Slot::State::kInFlight; }),
      slots_.end());
  return taken;
}

void PointAabbReducer::forget(const void* key) {
  for (const auto& slot : slots_) {
    if (slot->key == key) {
      slot->forgotten = true;
    }
  }
}

void PointAabbReducer::release() {
  const bool in_flight = std::any_of(
      slots_.cbegin(), slots_.cend(), [](const auto& slot) { return slot->state == Slot::State::kInFlight; });
  if (rhi_ != nullptr) {
    if (in_flight) {
      rhi_->finish();  // completes the readbacks, which point into the slots
    }
    rhi_->removeCleanupCallback(this);
  }
  slots_.clear();
  results_.clear();
  delete pipeline_;
  delete layout_bindings_;
  delete layout_params_;
  delete layout_result_;
  delete layout_points_;
  pipeline_ = nullptr;
  layout_bindings_ = nullptr;
  layout_params_ = nullptr;
  layout_result_ = nullptr;
  layout_points_ = nullptr;
  rhi_ = nullptr;
}

}  // namespace pj::scene3d
