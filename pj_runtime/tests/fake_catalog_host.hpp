// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <vector>

namespace PJ::testing {

// A catalog-only toolbox host with optionally named object datasets and field COUNT
// and TYPE the test controls. Neither existing fake covers this:
// ToolboxTestStore stamps every field float64 with real timestamps/values
// (it exists for the read path), and FakeMultiDatasetHost fixes one float64
// "value" field per topic (it exists for the several-datasets path). Tests
// that need topics with many fields of assorted types, or object topics, use
// this one.
//
// Catalog-only like FakeMultiDatasetHost: every other slot stays null so a
// test that wanders into the read path fails loudly instead of reading
// zeros.
class FakeCatalogHost {
 public:
  FakeCatalogHost& addTopic(const std::string& name) {
    topics_.push_back({name, {}});
    return *this;
  }

  // Adds a field to the most recently added topic that matches `topic_name`.
  // No-op if that topic was never added.
  FakeCatalogHost& addField(
      const std::string& topic_name, const std::string& field_name,
      PJ_primitive_type_t type = PJ_PRIMITIVE_TYPE_FLOAT64) {
    for (auto& t : topics_) {
      if (t.name == topic_name) {
        t.fields.push_back({field_name, type});
        return *this;
      }
    }
    return *this;
  }

  // Adds an object topic (catalog snapshot v2 only -- see supportsV2()).
  // `metadata_json` defaults to a document already carrying the canonical
  // `builtin_object_type` key so a test that only needs a plausible object
  // topic can omit it; a test asserting `"derived"` should pass a document
  // with (or without) the `"pj_derived"` key explicitly.
  FakeCatalogHost& addObjectTopic(
      const std::string& name, const std::string& type_name, std::uint64_t entries, std::int64_t t_min_ns,
      std::int64_t t_max_ns, const std::string& metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      const std::string& dataset = {}) {
    object_topics_.push_back({name, type_name, entries, t_min_ns, t_max_ns, metadata_json, dataset});
    if (!dataset.empty() && std::find(datasets_.begin(), datasets_.end(), dataset) == datasets_.end()) {
      datasets_.push_back(dataset);
    }
    return *this;
  }

  // Opt-in, true by default: a test simulates a host that predates catalog
  // snapshot v2 by calling supportsV2(false), which leaves the vtable's
  // acquire_catalog_snapshot_v2 slot null AND shrinks struct_size to exclude
  // it -- either alone already fails PJ_HAS_TAIL_SLOT, both together is what
  // a host actually built before the slot existed would report.
  FakeCatalogHost& supportsV2(bool value) {
    supports_v2_ = value;
    return *this;
  }

  [[nodiscard]] PJ_toolbox_host_t makeHost() {
    static const PJ_toolbox_host_vtable_t vtable_v2 = [] {
      PJ_toolbox_host_vtable_t v{};
      v.abi_version = PJ_PLUGIN_DATA_API_VERSION;
      v.struct_size = sizeof(PJ_toolbox_host_vtable_t);
      v.acquire_catalog_snapshot = &FakeCatalogHost::tAcquire;
      v.acquire_catalog_snapshot_v2 = &FakeCatalogHost::tAcquireV2;
      return v;
    }();
    static const PJ_toolbox_host_vtable_t vtable_v1_only = [] {
      PJ_toolbox_host_vtable_t v{};
      v.abi_version = PJ_PLUGIN_DATA_API_VERSION;
      v.struct_size = offsetof(PJ_toolbox_host_vtable_t, acquire_catalog_snapshot_v2);
      v.acquire_catalog_snapshot = &FakeCatalogHost::tAcquire;
      return v;
    }();
    return PJ_toolbox_host_t{this, supports_v2_ ? &vtable_v2 : &vtable_v1_only};
  }

 private:
  struct FieldSpec {
    std::string name;
    PJ_primitive_type_t type;
  };
  struct TopicSpec {
    std::string name;
    std::vector<FieldSpec> fields;
  };
  struct ObjectTopicSpec {
    std::string name;
    std::string type_name;
    std::uint64_t entries;
    std::int64_t t_min_ns;
    std::int64_t t_max_ns;
    std::string metadata_json;
    std::string dataset;
  };

  static PJ_string_view_t sv(const std::string& s) {
    return PJ_string_view_t{s.data(), s.size()};
  }

  // Rebuilds topic_abi_/field_abi_ from topics_ -- shared by tAcquire and
  // tAcquireV2 so the scalar half of a v2 snapshot is byte-for-byte what v1
  // would have reported.
  void rebuildScalarAbi() {
    source_abi_.clear();
    for (std::size_t i = 0; i < datasets_.size(); ++i) {
      PJ_data_source_info_t info{};
      info.handle = PJ_data_source_handle_t{static_cast<std::uint32_t>(i + 1)};
      info.name = sv(datasets_[i]);
      source_abi_.push_back(info);
    }
    topic_abi_.clear();
    field_abi_.clear();
    std::uint32_t field_cursor = 0;
    for (std::size_t ti = 0; ti < topics_.size(); ++ti) {
      const auto& t = topics_[ti];
      PJ_topic_info_t tinfo{};
      tinfo.handle = PJ_topic_handle_t{static_cast<std::uint32_t>(ti + 1)};
      tinfo.name = sv(t.name);
      tinfo.first_field = field_cursor;
      tinfo.field_count = static_cast<std::uint32_t>(t.fields.size());
      topic_abi_.push_back(tinfo);
      for (const auto& f : t.fields) {
        PJ_field_info_t finfo{};
        finfo.handle = PJ_field_handle_t{tinfo.handle, field_cursor + 1};
        finfo.name = sv(f.name);
        finfo.type = f.type;
        field_abi_.push_back(finfo);
        ++field_cursor;
      }
    }
  }

  static bool tAcquire(void* ctx, PJ_catalog_snapshot_t* out, PJ_error_t* /*err*/) noexcept {
    auto* self = static_cast<FakeCatalogHost*>(ctx);
    self->rebuildScalarAbi();
    out->data_sources = self->source_abi_.data();
    out->data_source_count = self->source_abi_.size();
    out->topics = self->topic_abi_.data();
    out->topic_count = self->topic_abi_.size();
    out->fields = self->field_abi_.data();
    out->field_count = self->field_abi_.size();
    out->release_ctx = nullptr;
    out->release = nullptr;
    return true;
  }

  static bool tAcquireV2(void* ctx, PJ_catalog_snapshot_v2_t* out, PJ_error_t* /*err*/) noexcept {
    auto* self = static_cast<FakeCatalogHost*>(ctx);
    self->rebuildScalarAbi();
    self->object_topic_abi_.clear();
    for (const auto& o : self->object_topics_) {
      PJ_object_topic_info_t oinfo{};
      oinfo.handle = PJ_object_topic_handle_t{static_cast<std::uint32_t>(self->object_topic_abi_.size() + 1)};
      for (const auto& source : self->source_abi_) {
        if (PJ::sdk::toStringView(source.name) == o.dataset) {
          oinfo.source = source.handle;
          break;
        }
      }
      oinfo.name = sv(o.name);
      oinfo.builtin_object_type = sv(o.type_name);
      oinfo.metadata_json = sv(o.metadata_json);
      oinfo.entry_count = o.entries;
      oinfo.time_min_ns = o.t_min_ns;
      oinfo.time_max_ns = o.t_max_ns;
      self->object_topic_abi_.push_back(oinfo);
    }
    out->struct_size = sizeof(PJ_catalog_snapshot_v2_t);
    out->reserved = 0;
    out->data_sources = self->source_abi_.data();
    out->data_source_count = self->source_abi_.size();
    out->topics = self->topic_abi_.data();
    out->topic_count = self->topic_abi_.size();
    out->fields = self->field_abi_.data();
    out->field_count = self->field_abi_.size();
    out->object_topics = self->object_topic_abi_.data();
    out->object_topic_count = self->object_topic_abi_.size();
    out->release_ctx = nullptr;
    out->release = nullptr;
    return true;
  }

  std::vector<std::string> datasets_;
  std::vector<PJ_data_source_info_t> source_abi_;
  std::vector<TopicSpec> topics_;
  std::vector<ObjectTopicSpec> object_topics_;
  bool supports_v2_ = true;
  std::vector<PJ_topic_info_t> topic_abi_;
  std::vector<PJ_field_info_t> field_abi_;
  std::vector<PJ_object_topic_info_t> object_topic_abi_;
};

}  // namespace PJ::testing
