// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/DataProcessorsKindRouter.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "RuntimeHostSlot.h"
#include "pj_base/sdk/data_processors_host.hpp"  // sdk::readDataProcessorRequest, sdk::readEvaluationBudget
#include "pj_base/sdk/plugin_data_api.hpp"       // sdk::toStringView
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataProcessorsRuntimeHost.h"
#include "pj_runtime/MarkersRuntimeHost.h"
#include "pj_runtime/OnDemandRuntimeHost.h"
#include "pj_runtime/ServiceRegistration.h"

namespace PJ {

namespace {

using host_slot::guarded;
using host_slot::reject;

constexpr std::string_view kDomain = "data_processors.router";

[[nodiscard]] bool isBound(const PJ_data_processors_host_t& backend) noexcept {
  return backend.vtable != nullptr;
}

/// `kind` for an error message, after its reason: at most 32 bytes, quoted, other
/// bytes than printable ASCII escaped as \xNN, so neither its length nor a NUL can push
/// the reason out of the bounded PJ_error_t message.
std::string kindLabel(PJ_string_view_t kind) {
  constexpr std::size_t kMaxBytes = 32;
  const std::string_view name = sdk::toStringView(kind);
  std::string label = "'";
  for (const char byte : name.substr(0, kMaxBytes)) {
    const auto code = static_cast<unsigned char>(byte);
    if (code >= 0x20 && code < 0x7f) {
      label += byte;
    } else {
      constexpr std::string_view kHex = "0123456789abcdef";
      label += "\\x";
      label += kHex[code >> 4];
      label += kHex[code & 0xf];
    }
  }
  label += name.size() > kMaxBytes ? "'..." : "'";
  return label;
}

bool rejectKind(PJ_error_t* out_error, PJ_string_view_t kind) {
  return reject(out_error, kDomain, "unknown data processor kind " + kindLabel(kind));
}

/// The next router handle, shared by every router in the process so that a handle one
/// router returned is never valid on another. GUI thread only, as every slot is.
uint64_t g_next_handle = 1;

/// Fetch one backend's plugin-namespaced id list into `out` (owned copies — the
/// backend's views die on its next vtable call). Returns false on backend error.
bool appendIds(const PJ_data_processors_host_t& backend, std::vector<std::string>& out, PJ_error_t* out_error) {
  uint64_t count = 0;
  if (!backend.vtable->list_data_processor_ids(backend.ctx, nullptr, 0, &count, out_error)) {
    return false;
  }
  if (count == 0) {
    return true;
  }
  std::vector<PJ_string_view_t> views(count);
  if (!backend.vtable->list_data_processor_ids(backend.ctx, views.data(), count, &count, out_error)) {
    return false;
  }
  for (uint64_t i = 0; i < count && i < views.size(); ++i) {
    out.emplace_back(views[i].data, views[i].size);
  }
  return true;
}

}  // namespace

DataProcessorsKindRouter::DataProcessorsKindRouter(
    MarkersRuntimeHost& markers, DataProcessorsRuntimeHost& transforms, OnDemandRuntimeHost& on_demand)
    : DataProcessorsKindRouter(
          markers.raw(), transforms.raw(), on_demand.raw(),
          OwnershipProbes{
              .transforms = [&transforms](std::string_view id) { return transforms.owns(id); },
              .markers = [&markers](std::string_view id) { return markers.owns(id); },
              .on_demand = [&on_demand](std::string_view id) { return on_demand.owns(id); },
          }) {}

DataProcessorsKindRouter::DataProcessorsKindRouter(
    PJ_data_processors_host_t markers, PJ_data_processors_host_t transforms, PJ_data_processors_host_t on_demand,
    OwnershipProbes owns)
    : markers_(markers),
      transforms_(transforms),
      on_demand_(on_demand),
      owns_(std::move(owns)),
      vtable_{
          .protocol_version = 1,
          .struct_size = sizeof(PJ_data_processors_host_vtable_t),
          .create_data_processor = &DataProcessorsKindRouter::onCreate,
          .remove_data_processor = &DataProcessorsKindRouter::onRemove,
          .list_data_processor_ids = &DataProcessorsKindRouter::onList,
          .data_processor_config = &DataProcessorsKindRouter::onConfig,
          .validate_data_processor_script = &DataProcessorsKindRouter::onValidate,
          .create_data_processor_v2 = &DataProcessorsKindRouter::onCreateV2,
          .submit_evaluation = &DataProcessorsKindRouter::onSubmit,
          .poll_evaluation = &DataProcessorsKindRouter::onPoll,
          .release_evaluation = &DataProcessorsKindRouter::onRelease,
          .check_data_processor_script = &DataProcessorsKindRouter::onCheck,
      },
      raw_{this, &vtable_} {
  assert(!isBound(markers_) || owns_.markers);
  assert(!isBound(transforms_) || owns_.transforms);
  assert(!isBound(on_demand_) || owns_.on_demand);
}

DataProcessorsKindRouter::~DataProcessorsKindRouter() {
  for (const auto& [handle, owner] : handles_) {
    PJ_error_t ignored{};
    owner.backend->vtable->release_evaluation(owner.backend->ctx, owner.backend_handle, &ignored);
  }
}

uint64_t DataProcessorsKindRouter::setNextHandleForTest(uint64_t next) noexcept {
  return std::exchange(g_next_handle, next);
}

Status DataProcessorsKindRouter::registerServices(ServiceRegistryBuilder& registry) {
  return registerRequiredService<sdk::DataProcessorsHostService>(registry, raw_);
}

const PJ_data_processors_host_t* DataProcessorsKindRouter::backendFor(PJ_string_view_t kind) const noexcept {
  const std::string_view name = sdk::toStringView(kind);
  const PJ_data_processors_host_t* backend = name == "markers"     ? &markers_
                                             : name == "transform" ? &transforms_
                                             : name == "on_demand" ? &on_demand_
                                                                   : nullptr;
  return backend != nullptr && isBound(*backend) ? backend : nullptr;
}

const PJ_data_processors_host_t* DataProcessorsKindRouter::ownerOf(PJ_string_view_t id) const {
  const std::string_view local_id = sdk::toStringView(id);
  for (const auto& [backend, probe] : {
           std::pair{&transforms_, &owns_.transforms},
           std::pair{&markers_, &owns_.markers},
           std::pair{&on_demand_, &owns_.on_demand},
       }) {
    if (isBound(*backend) && (*probe)(local_id)) {
      return backend;
    }
  }
  return nullptr;
}

bool DataProcessorsKindRouter::onCreate(
    void* ctx, PJ_string_view_t id, PJ_string_view_t kind, PJ_string_view_t language, const PJ_string_view_t* inputs,
    uint64_t input_count, const PJ_string_view_t* outputs, uint64_t output_count, PJ_string_view_t script,
    PJ_string_view_t params_json, uint32_t flags, PJ_string_view_t* out_topics, uint64_t out_topics_capacity,
    uint64_t* out_topics_count, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsKindRouter>(ctx, out_error, kDomain, [&](DataProcessorsKindRouter& self) {
    // The contract's v1 rule: on_demand outputs are typed, so only the v2 slot creates them.
    if (sdk::toStringView(kind) == "on_demand") {
      return reject(out_error, kDomain, "kind 'on_demand' is created only with create_data_processor_v2");
    }
    const PJ_data_processors_host_t* backend = self.backendFor(kind);
    if (backend == nullptr) {
      return rejectKind(out_error, kind);
    }
    // The backends share one id namespace here: reject an id already live in another
    // backend, or create would spawn a second node and make remove/config ambiguous.
    // Same-kind ids fall through to the backend's own upsert.
    if (const PJ_data_processors_host_t* owner = self.ownerOf(id); owner != nullptr && owner != backend) {
      return reject(out_error, kDomain, "data processor id already exists with a different kind");
    }
    return backend->vtable->create_data_processor(
        backend->ctx, id, kind, language, inputs, input_count, outputs, output_count, script, params_json, flags,
        out_topics, out_topics_capacity, out_topics_count, out_error);
  });
}

bool DataProcessorsKindRouter::onRemove(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsKindRouter>(ctx, out_error, kDomain, [&](DataProcessorsKindRouter& self) {
    const PJ_data_processors_host_t* owner = self.ownerOf(id);
    if (owner == nullptr) {
      return reject(out_error, kDomain, "unknown data processor id");
    }
    return owner->vtable->remove_data_processor(owner->ctx, id, out_error);
  });
}

bool DataProcessorsKindRouter::onList(
    void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsKindRouter>(ctx, out_error, kDomain, [&](DataProcessorsKindRouter& self) {
    std::vector<std::string> ids;
    for (const PJ_data_processors_host_t* backend : {&self.transforms_, &self.markers_, &self.on_demand_}) {
      if (isBound(*backend) && !appendIds(*backend, ids, out_error)) {
        return false;
      }
    }
    self.list_storage_ = std::move(ids);
    self.list_views_.clear();
    self.list_views_.reserve(self.list_storage_.size());
    for (const std::string& id : self.list_storage_) {
      self.list_views_.push_back(sdk::toAbiString(id));
    }
    if (out_count != nullptr) {
      *out_count = self.list_views_.size();
    }
    if (out_ids != nullptr) {
      const uint64_t filled = std::min<uint64_t>(capacity, self.list_views_.size());
      for (uint64_t index = 0; index < filled; ++index) {
        out_ids[index] = self.list_views_[index];
      }
    }
    return true;
  });
}

bool DataProcessorsKindRouter::onConfig(
    void* ctx, PJ_string_view_t id, PJ_string_view_t* out_recipe_json, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsKindRouter>(ctx, out_error, kDomain, [&](DataProcessorsKindRouter& self) {
    // A preview its backend's config hides reports "unknown" there, as through the backend alone.
    const PJ_data_processors_host_t* owner = self.ownerOf(id);
    if (owner == nullptr) {
      return reject(out_error, kDomain, "unknown data processor id");
    }
    return owner->vtable->data_processor_config(owner->ctx, id, out_recipe_json, out_error);
  });
}

bool DataProcessorsKindRouter::onValidate(
    void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script, PJ_string_view_t params_json,
    PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsKindRouter>(ctx, out_error, kDomain, [&](DataProcessorsKindRouter& self) {
    const PJ_data_processors_host_t* backend = self.backendFor(kind);
    if (backend == nullptr) {
      return rejectKind(out_error, kind);
    }
    return backend->vtable->validate_data_processor_script(
        backend->ctx, kind, language, script, params_json, out_error);
  });
}

bool DataProcessorsKindRouter::onCheck(
    void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script, PJ_string_view_t params_json,
    uint32_t* out_status, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsKindRouter>(ctx, out_error, kDomain, [&](DataProcessorsKindRouter& self) {
    const PJ_data_processors_host_t* backend = self.backendFor(kind);
    if (backend == nullptr) {
      return rejectKind(out_error, kind);
    }
    if (PJ_HAS_TAIL_SLOT(PJ_data_processors_host_vtable_t, backend->vtable, check_data_processor_script)) {
      return backend->vtable->check_data_processor_script(
          backend->ctx, kind, language, script, params_json, out_status, out_error);
    }
    return host_slot::checkByValidate(
        backend->vtable->validate_data_processor_script, backend->ctx, kind, language, script, params_json, out_status,
        out_error);
  });
}

bool DataProcessorsKindRouter::onCreateV2(
    void* ctx, const PJ_data_processor_request_t* request, PJ_string_view_t* out_topics, uint64_t out_topics_capacity,
    uint64_t* out_topics_count, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsKindRouter>(ctx, out_error, kDomain, [&](DataProcessorsKindRouter& self) {
    const auto decoded = sdk::readDataProcessorRequest(request, sdk::DataProcessorSlot::kCreateV2);
    if (!decoded) {
      return reject(out_error, kDomain, decoded.error());
    }
    const PJ_data_processors_host_t* backend = self.backendFor(decoded->kind);
    if (backend == nullptr ||
        !PJ_HAS_TAIL_SLOT(PJ_data_processors_host_vtable_t, backend->vtable, create_data_processor_v2)) {
      return rejectKind(out_error, decoded->kind);
    }
    // Same cross-kind id collision guard as onCreate.
    if (const PJ_data_processors_host_t* owner = self.ownerOf(decoded->id); owner != nullptr && owner != backend) {
      return reject(out_error, kDomain, "data processor id already exists with a different kind");
    }
    return backend->vtable->create_data_processor_v2(
        backend->ctx, &*decoded, out_topics, out_topics_capacity, out_topics_count, out_error);
  });
}

bool DataProcessorsKindRouter::onSubmit(
    void* ctx, const PJ_data_processor_request_t* request, const PJ_evaluation_budget_t* budget, uint64_t* out_handle,
    PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsKindRouter>(ctx, out_error, kDomain, [&](DataProcessorsKindRouter& self) {
    const auto decoded = sdk::readDataProcessorRequest(request, sdk::DataProcessorSlot::kSubmitEvaluation);
    if (!decoded) {
      return reject(out_error, kDomain, decoded.error());
    }
    const auto decoded_budget = sdk::readEvaluationBudget(budget);
    if (!decoded_budget) {
      return reject(out_error, kDomain, decoded_budget.error());
    }
    const PJ_data_processors_host_t* backend = self.backendFor(decoded->kind);
    // poll and release are checked too: a handle the router cannot read or free is never issued.
    if (backend == nullptr || !PJ_HAS_TAIL_SLOT(PJ_data_processors_host_vtable_t, backend->vtable, submit_evaluation) ||
        !PJ_HAS_TAIL_SLOT(PJ_data_processors_host_vtable_t, backend->vtable, poll_evaluation) ||
        !PJ_HAS_TAIL_SLOT(PJ_data_processors_host_vtable_t, backend->vtable, release_evaluation)) {
      return reject(out_error, kDomain, "no on-demand evaluation for kind " + kindLabel(decoded->kind));
    }
    if (out_handle == nullptr) {
      return reject(out_error, kDomain, "out_handle is NULL");
    }
    if (g_next_handle == std::numeric_limits<uint64_t>::max()) {
      return reject(out_error, kDomain, "evaluation handles exhausted");
    }
    uint64_t backend_handle = 0;
    if (!backend->vtable->submit_evaluation(backend->ctx, &*decoded, &*decoded_budget, &backend_handle, out_error)) {
      return false;
    }
    const uint64_t router_handle = g_next_handle++;
    self.handles_[router_handle] = HandleOwner{backend, backend_handle};
    *out_handle = router_handle;
    return true;
  });
}

bool DataProcessorsKindRouter::onPoll(
    void* ctx, uint64_t handle, uint32_t* out_state, PJ_string_view_t* out_json, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsKindRouter>(ctx, out_error, kDomain, [&](DataProcessorsKindRouter& self) {
    const auto it = self.handles_.find(handle);
    if (it == self.handles_.end()) {
      return reject(out_error, kDomain, "unknown evaluation handle");
    }
    const HandleOwner& owner = it->second;
    return owner.backend->vtable->poll_evaluation(
        owner.backend->ctx, owner.backend_handle, out_state, out_json, out_error);
  });
}

bool DataProcessorsKindRouter::onRelease(void* ctx, uint64_t handle, PJ_error_t* out_error) noexcept {
  return guarded<DataProcessorsKindRouter>(ctx, out_error, kDomain, [&](DataProcessorsKindRouter& self) {
    const auto it = self.handles_.find(handle);
    if (it == self.handles_.end()) {
      return reject(out_error, kDomain, "unknown or already released evaluation handle");
    }
    const HandleOwner owner = it->second;
    if (!owner.backend->vtable->release_evaluation(owner.backend->ctx, owner.backend_handle, out_error)) {
      return false;
    }
    self.handles_.erase(it);
    return true;
  });
}

}  // namespace PJ
