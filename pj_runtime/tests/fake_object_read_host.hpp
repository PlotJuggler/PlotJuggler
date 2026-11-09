// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <map>
#include <pj_base/builtin/plot_markers.hpp>
#include <pj_base/builtin/plot_markers_codec.hpp>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <vector>

namespace PJ::testing {

// A minimal stand-in for pj.toolbox_object_read.v1, holding one serialized
// PlotMarkers blob per topic name.
//
// It exists because the SDK's ToolboxTestStore covers the scalar read path only,
// and the marker count the model is handed after every create comes from the
// OBJECT path — untested, that number is exactly the kind of plausible-looking
// figure nobody notices is wrong.
//
// Deliberately serializes through the real codec rather than hand-rolling bytes:
// a fake that encodes its own format would pass while the production decoder
// failed on the host's actual output.
class FakeObjectReadHost {
 public:
  // Publish a marker set under `topic`, exactly as MarkerService does: one blob
  // carrying the whole set, pushed at timestamp 0.
  FakeObjectReadHost& publish(const std::string& topic, const PJ::sdk::PlotMarkers& markers) {
    blobs_[topic] = PJ::serializePlotMarkers(markers);
    order_.push_back(topic);
    return *this;
  }

  [[nodiscard]] PJ_object_read_host_t makeHost() {
    static const PJ_object_read_host_vtable_t vtable = {
        .abi_version = PJ_PLUGIN_DATA_API_VERSION,
        .struct_size = sizeof(PJ_object_read_host_vtable_t),
        .lookup_topic = &FakeObjectReadHost::tLookup,
        .list_topics = nullptr,
        .topic_metadata = nullptr,
        .read_latest_at = &FakeObjectReadHost::tReadLatest,
        .get_bytes = &FakeObjectReadHost::tGetBytes,
        .release_bytes = &FakeObjectReadHost::tReleaseBytes,
        .entry_count = &FakeObjectReadHost::tEntryCount,
        .time_range = nullptr,
        .lookup_topic_on_dataset = nullptr,
    };
    return PJ_object_read_host_t{this, &vtable};
  }

 private:
  // Handles are 1-based indices into `order_`; 0 is the documented miss value.
  static PJ_object_topic_handle_t tLookup(void* ctx, PJ_string_view_t name) noexcept {
    auto* self = static_cast<FakeObjectReadHost*>(ctx);
    const std::string want(name.data == nullptr ? "" : name.data, name.size);
    for (std::size_t i = 0; i < self->order_.size(); ++i) {
      if (self->order_[i] == want) {
        return PJ_object_topic_handle_t{static_cast<uint32_t>(i + 1)};
      }
    }
    return PJ_object_topic_handle_t{0};
  }

  static bool tReadLatest(
      void* ctx, PJ_object_topic_handle_t topic, int64_t /*ts*/, PJ_object_bytes_handle_t* out_handle,
      int64_t* out_timestamp, PJ_error_t* out_error) noexcept {
    auto* self = static_cast<FakeObjectReadHost*>(ctx);
    if (topic.id == 0 || topic.id > self->order_.size()) {
      PJ::sdk::fillError(out_error, 1, "test", "unknown object topic");
      return false;
    }
    // The blob is owned by this fake for its whole lifetime, so the "owning
    // handle" the ABI describes is just a borrowed pointer here; tReleaseBytes
    // is a no-op to match.
    auto& blob = self->blobs_[self->order_[topic.id - 1]];
    *out_handle = reinterpret_cast<PJ_object_bytes_handle_t>(&blob);
    if (out_timestamp != nullptr) {
      *out_timestamp = 0;
    }
    return true;
  }

  static void tGetBytes(PJ_object_bytes_handle_t handle, const uint8_t** out_data, uint64_t* out_size) noexcept {
    const auto* blob = reinterpret_cast<const std::vector<uint8_t>*>(handle);
    *out_data = blob->data();
    *out_size = blob->size();
  }

  static void tReleaseBytes(PJ_object_bytes_handle_t /*handle*/) noexcept {}

  static uint64_t tEntryCount(void* ctx, PJ_object_topic_handle_t topic) noexcept {
    auto* self = static_cast<FakeObjectReadHost*>(ctx);
    // One entry per topic, whatever the marker count — which is precisely why
    // the production code decodes the payload instead of trusting this.
    return (topic.id != 0 && topic.id <= self->order_.size()) ? 1 : 0;
  }

  std::map<std::string, std::vector<uint8_t>> blobs_;
  std::vector<std::string> order_;
};

}  // namespace PJ::testing
