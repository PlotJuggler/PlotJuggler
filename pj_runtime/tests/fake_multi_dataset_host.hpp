// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace PJ::testing {

// A toolbox host that serves a catalog with SEVERAL data sources.
//
// The SDK's ToolboxTestStore cannot express this: it stamps every topic with
// source handle 1 and hands back a snapshot with `data_sources = nullptr,
// data_source_count = 0`. That is fine for the scalar read path it exists for,
// but it makes the multi-dataset case — two runs of the same robot, the reason
// any of this exists — untestable, and an untested branch that only fires when a
// user loads a second file is a branch that fires for the first time in front of
// the user.
//
// Deliberately catalog-only: `acquire_catalog_snapshot` is the single slot the
// catalog tools (list_topics, describe_topic) need. A read records which topic it asked for, so a test
// can check a same-named series resolved to the right dataset, and fails unless
// servedBy() gave that topic samples; creating a data source records the name and
// fails. Every other slot stays null so a test that wanders off fails loudly.
class FakeMultiDatasetHost {
 public:
  // Add a dataset and its topics. Topics must be added per source, in order:
  // the ABI lays them out contiguously (first_topic/topic_count) and this
  // mirrors that layout rather than pretending it does not exist.
  FakeMultiDatasetHost& addDataset(const std::string& name, const std::vector<std::string>& topics) {
    Source src;
    src.name = name;
    src.first_topic = static_cast<std::uint32_t>(topics_.size());
    src.topic_count = static_cast<std::uint32_t>(topics.size());
    sources_.push_back(src);
    for (const auto& t : topics) {
      topics_.push_back(t);
      fields_.emplace_back("value");
    }
    return *this;
  }

  void clearDatasets() {
    sources_.clear();
    topics_.clear();
    fields_.clear();
  }

  [[nodiscard]] PJ_toolbox_host_t makeHost() {
    static const PJ_toolbox_host_vtable_t vtable = [] {
      PJ_toolbox_host_vtable_t table{};
      table.abi_version = PJ_PLUGIN_DATA_API_VERSION;
      table.struct_size = sizeof(PJ_toolbox_host_vtable_t);
      table.acquire_catalog_snapshot = &FakeMultiDatasetHost::tAcquire;
      table.read_series_arrow = &FakeMultiDatasetHost::tRead;
      table.create_data_source = &FakeMultiDatasetHost::tCreateDataSource;
      return table;
    }();
    return PJ_toolbox_host_t{this, &vtable};
  }

  // The topic indices, in addDataset order, that reads asked for since the last call.
  [[nodiscard]] std::set<std::size_t> takeReadTopics() {
    return std::exchange(read_topics_, {});
  }

  // A read of topic i answers with field by_topic[i] of `store`.
  void servedBy(PJ_toolbox_host_t store, std::vector<PJ_field_handle_t> by_topic) {
    read_store_ = store;
    read_fields_ = std::move(by_topic);
  }

  [[nodiscard]] int catalogAcquires() const {
    return catalog_acquires_;
  }

  [[nodiscard]] const std::string& lastCreatedSource() const {
    return last_created_source_;
  }

 private:
  struct Source {
    std::string name;
    std::uint32_t first_topic = 0;
    std::uint32_t topic_count = 0;
  };

  static PJ_string_view_t sv(const std::string& s) {
    return PJ_string_view_t{s.data(), s.size()};
  }

  static bool tRead(
      void* ctx, PJ_field_handle_t field, struct ArrowSchema* schema, struct ArrowArray* array,
      PJ_error_t* err) noexcept {
    auto* self = static_cast<FakeMultiDatasetHost*>(ctx);
    const auto topic = static_cast<std::size_t>(field.topic.id) - 1;
    self->read_topics_.insert(topic);
    return topic < self->read_fields_.size() &&
           self->read_store_.vtable->read_series_arrow(
               self->read_store_.ctx, self->read_fields_[topic], schema, array, err);
  }

  static bool tCreateDataSource(
      void* ctx, PJ_string_view_t name, PJ_data_source_handle_t* /*out*/, PJ_error_t* /*err*/) noexcept {
    static_cast<FakeMultiDatasetHost*>(ctx)->last_created_source_.assign(name.data, name.size);
    return false;
  }

  static bool tAcquire(void* ctx, PJ_catalog_snapshot_t* out, PJ_error_t* /*err*/) noexcept {
    auto* self = static_cast<FakeMultiDatasetHost*>(ctx);
    ++self->catalog_acquires_;
    self->src_abi_.clear();
    self->topic_abi_.clear();
    self->field_abi_.clear();
    for (std::size_t i = 0; i < self->sources_.size(); ++i) {
      const auto& s = self->sources_[i];
      PJ_data_source_info_t info{};
      info.handle = PJ_data_source_handle_t{static_cast<std::uint32_t>(i + 1)};
      info.name = sv(s.name);
      info.first_topic = s.first_topic;
      info.topic_count = s.topic_count;
      self->src_abi_.push_back(info);
    }
    for (std::size_t i = 0; i < self->topics_.size(); ++i) {
      PJ_topic_info_t info{};
      info.handle = PJ_topic_handle_t{static_cast<std::uint32_t>(i + 1)};
      info.name = sv(self->topics_[i]);
      info.source = self->sourceOfTopic(i);
      info.first_field = static_cast<std::uint32_t>(i);
      info.field_count = 1;
      self->topic_abi_.push_back(info);
    }
    for (std::size_t i = 0; i < self->fields_.size(); ++i) {
      PJ_field_info_t info{};
      info.handle = PJ_field_handle_t{{static_cast<std::uint32_t>(i + 1)}, 0};
      info.name = sv(self->fields_[i]);
      info.type = PJ_PRIMITIVE_TYPE_FLOAT64;
      self->field_abi_.push_back(info);
    }
    out->data_sources = self->src_abi_.data();
    out->data_source_count = self->src_abi_.size();
    out->topics = self->topic_abi_.data();
    out->topic_count = self->topic_abi_.size();
    out->fields = self->field_abi_.data();
    out->field_count = self->field_abi_.size();
    out->release_ctx = nullptr;
    out->release = nullptr;
    return true;
  }

  [[nodiscard]] PJ_data_source_handle_t sourceOfTopic(std::size_t topic_index) const {
    for (std::size_t i = 0; i < sources_.size(); ++i) {
      if (topic_index >= sources_[i].first_topic && topic_index < sources_[i].first_topic + sources_[i].topic_count) {
        return PJ_data_source_handle_t{static_cast<std::uint32_t>(i + 1)};
      }
    }
    return PJ_data_source_handle_t{0};
  }

  std::set<std::size_t> read_topics_;
  int catalog_acquires_ = 0;
  PJ_toolbox_host_t read_store_{};
  std::vector<PJ_field_handle_t> read_fields_;
  std::string last_created_source_;
  std::vector<Source> sources_;
  std::vector<std::string> topics_;
  std::vector<std::string> fields_;
  // ABI mirrors, rebuilt per acquire and kept alive by this object — the views
  // above point into the strings, so neither may be reallocated while a
  // snapshot is live.
  std::vector<PJ_data_source_info_t> src_abi_;
  std::vector<PJ_topic_info_t> topic_abi_;
  std::vector<PJ_field_info_t> field_abi_;
};

}  // namespace PJ::testing
