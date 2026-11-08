// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// A data-processors host that records what it was asked to install instead of
// installing it, so the agent tool tests can assert on the exact script and
// inputs a tool synthesized.
//
// Note the limit this implies. It records INTENT — nothing here runs the script,
// so a transform recorded as correct can still yield an empty curve in the real
// application (e.g. inputs that do not share exact timestamps). Verifying the
// drawn result requires the GUI pass.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_plugins/testing/toolbox_test_store.hpp>
#include <string>
#include <vector>

namespace PJ::testing {

// Records the last create/validate call so tests can assert the synthesized
// Luau script + routed inputs/outputs. Returns success unless a fail flag is set.
struct RecordingDpHost {
  int create_calls = 0;
  // Persistent creates ever ATTEMPTED. Note what this is not: it never goes down,
  // so it answers "did it call create" and not "what is the user left with". Once
  // removal existed, those stopped being the same question — judge an outcome
  // with liveCount() instead, and keep this for asserting a call happened at all.
  int persistent_creates = 0;
  int validate_calls = 0;
  bool fail_validate = false;
  std::string validate_error = "compile error";  // what a failing validate_data_processor_script says
  // check_data_processor_script (0.38.0) answers what validate would (VALID / INVALID with validate_error),
  // or UNCHECKED with check_message when check_unchecked is set. Counted in validate_calls.
  bool check_unchecked = false;
  std::string check_message = "not checked: busy";
  bool fail_create = false;
  // Simulate a host that has not heard of any create_data_processor flag bit
  // besides EPHEMERAL (i.e. an older host, pre-HISTORY_EXEMPT): any other bit
  // is refused with an error message containing "reserved" -- the substring
  // derived_tools.cpp's createWithFlagFallback matches to retry once with flags=0.
  bool reject_unknown_flags = false;
  // Test-only bridge to a ToolboxTestStore: when set, an EPHEMERAL create
  // registers the resolved output's exact topic/field (split on the last
  // '/', matching joinSeriesPath's convention) into this store, holding
  // `ephemeral_series_ts`/`ephemeral_series_vals`. This is what lets a test
  // assert on the read that follows create() without predicting the
  // counter-generated ephemeral id (see evaluateSeries in derived_tools.cpp).
  PJ::testing::ToolboxTestStore* ephemeral_series_store = nullptr;
  std::vector<std::int64_t> ephemeral_series_ts;
  std::vector<double> ephemeral_series_vals;
  // Config echo for data_processor_config(id) (PJ::sdk::DataProcessorsHostView
  // ::recipeOf), what noteUndoProtection (derived_tools.cpp) reads back to confirm
  // HISTORY_EXEMPT actually took. std::nullopt -> the returned recipe JSON
  // omits "history_exempt" entirely, mirroring a host that does not know the
  // property yet even if it tolerated the flag bit; a value -> the recipe
  // carries "history_exempt": true|false; fail_config -> the read itself
  // fails.
  std::optional<bool> config_history_exempt;
  bool fail_config = false;
  std::string canned_config_json;
  // The `series{topic, rows, failed, first_error, complete}` object the config of an owner's own recipe carries
  // while the host replays its history. data_processor_config serves one entry per call, in order, and repeats the
  // last once they are used up; empty -> the recipe JSON has no "series" (a host that does not report it).
  struct SeriesStatus {
    std::string topic;
    std::uint64_t rows = 0;
    std::uint64_t failed = 0;
    std::string first_error;
    bool complete = false;
  };
  std::vector<SeriesStatus> config_series;
  std::size_t config_series_served = 0;
  int config_calls = 0;
  std::string last_config_id;
  std::string last_config_json;  // owned storage for the borrowed out_recipe_json view
  std::string last_kind;
  std::string last_id;
  std::string last_removed;
  std::uint32_t last_flags = 0;
  std::string last_script;
  std::string last_validate_script;
  std::vector<std::string> last_inputs;
  std::vector<std::string> last_outputs;
  std::vector<std::string> live_ids;  // what list() reports; the views point into this
  std::vector<std::string> resolved;  // storage the returned borrowed views point into

  // --- v2 (typed request) surface: create_data_processor_v2 / submit_evaluation /
  // poll_evaluation / release_evaluation, used by evaluate's object path and
  // create_derived_object. Simulates an older host (slots null, so
  // PJ_HAS_TAIL_SLOT fails) when supportsV2 is false.
  bool supports_v2 = true;
  bool fail_submit = false;
  // How many times poll_evaluation answers PENDING before COMPLETED, captured
  // per handle at submit time (see EvaluateWaitsThroughPendingPolls).
  int pending_polls = 0;
  std::uint32_t terminal_state = PJ_EVALUATION_STATE_COMPLETED;
  // Default report: one bundle with a "cropped" object summary and a
  // "count" scalar value, matching the crop_box shape fake_backend.hpp's
  // "crop <topic> at <t>" phrase asks for.
  std::string canned_report_json =
      R"({"coverage":{"start_ns":0,"end_ns":0,"evaluated_until_ns":null,"candidates":1,"evaluated":1,)"
      R"("cache_hits":0,"complete":true,"stopped":"complete"},"bundles":[{"requested_ns":0,"stamp_ns":0,)"
      R"("from_cache":false,"revision":1,"inputs":[{"alias":"/cloud","resolved_ns":0,"is_object":true}],)"
      R"("outputs":{"cropped":{"status":"ok","summary":{"count":42}},"count":{"status":"ok","value":42}}}]})";
  // Reports served before canned_report_json, one per COMPLETED poll (front first): lets a test script
  // a sequence of answers, e.g. "no sample" then a report.
  std::vector<std::string> report_queue;
  // When set, an evaluation requested at an instant before it is answered the way the host answers an input
  // that has no sample yet: a report whose coverage carries the host's raw error (with the instant in ns),
  // instead of the queued or canned report.
  std::optional<std::int64_t> first_sample_ns;
  std::string first_sample_input = "/cloud";
  // What every submit_evaluation asked for, in order.
  struct RecordedRequest {
    std::int64_t instant_ns = 0;  // raw ns
    std::uint32_t flags = 0;
    std::size_t output_count = 0;  // declared outputs
    std::string language;
    std::string script;
    std::size_t input_count = 0;
    std::string params_json;
    std::uint32_t dataset_id = 0;
  };
  std::vector<RecordedRequest> submits;
  int create_v2_calls = 0;
  std::vector<std::string> last_create_v2_inputs;
  std::string last_create_v2_script;
  int submit_calls = 0;
  int poll_calls = 0;
  std::vector<std::uint64_t> released_handles;
  std::uint32_t last_time_flags = 0;
  std::int64_t last_window_start_ns = 0;
  std::int64_t last_window_end_ns = 0;
  std::int64_t last_instant_ns = 0;
  std::string last_params_json;                // params_json of the last v2 request (create or submit)
  std::vector<std::string> last_output_types;  // parallel to last_outputs
  std::uint64_t last_budget_max_millis = 0;
  std::uint64_t last_budget_max_evaluations = 0;
  std::uint64_t last_budget_max_report_bytes = 0;
  std::map<std::uint64_t, int> poll_countdown;
  std::map<std::uint64_t, std::int64_t> handle_instant;  // the instant each submitted evaluation asked for
  std::uint64_t next_handle = 1;
  std::string last_poll_json;  // owned storage for the borrowed out_json view

  // create_data_processor_v2's OWN snapshot of flags/time_flags/instant,
  // taken separately from the shared last_* fields: create_derived_object
  // installs via createV2 and then immediately submitEvaluation()s the
  // installed node to read a finding back, and that second call's recordRequest
  // would otherwise overwrite what the create call actually carried before a
  // test gets to look at it.
  std::string last_create_v2_params_json;
  std::uint32_t last_create_v2_flags = 0;
  std::uint32_t last_create_v2_time_flags = 0;
  std::int64_t last_create_v2_instant_ns = 0;

  // One record per ACCEPTED persistent create, so a verdict can ask what a
  // surviving id actually is (kind, script, declared inputs) instead of judging
  // from last_* — which only remembers the final call and lumps three different
  // leftovers under one shape. Removal deliberately keeps the record: history
  // stays, and pairing `created` with `live_ids` answers "what is the user
  // left with, and what is each thing".
  struct CreatedNode {
    std::string id;
    std::string kind;
    std::string script;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
  };
  std::vector<CreatedNode> created;

  const CreatedNode* recordFor(const std::string& id) const {
    for (auto it = created.rbegin(); it != created.rend(); ++it) {
      if (it->id == id) {
        return &*it;
      }
    }
    return nullptr;
  }

  // The real host compiles a transform script in an environment where the
  // marker-rule vocabulary does not exist, so a `series(...)` reference fails to
  // resolve and the install is refused (verified in the application,
  // 2026-08-14). Mirror that refusal here: without it the harness records as a
  // success a runtime cross-read the product would reject, and the benchmark
  // measures an outcome that cannot happen.
  static bool rejectsCrossRead(const std::string& kind, const std::string& script, PJ_error_t* err) {
    if (kind == "transform" && script.find("series(") != std::string::npos) {
      PJ::sdk::fillError(
          err, 1, "test", "unknown global 'series' — the marker-rule vocabulary does not exist in a transform");
      return true;
    }
    return false;
  }

  static std::string toStr(PJ_string_view_t v) {
    return std::string(v.data == nullptr ? "" : v.data, v.size);
  }

  static bool tCreate(
      void* ctx, PJ_string_view_t id, PJ_string_view_t kind, PJ_string_view_t language, const PJ_string_view_t* inputs,
      uint64_t input_count, const PJ_string_view_t* outputs, uint64_t output_count, PJ_string_view_t script,
      PJ_string_view_t /*params*/, uint32_t flags, PJ_string_view_t* out_topics, uint64_t out_topics_capacity,
      uint64_t* out_topics_count, PJ_error_t* err) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    (void)language;
    ++self->create_calls;
    self->last_flags = flags;
    if ((flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) == 0) {
      ++self->persistent_creates;
    }
    self->last_id = toStr(id);
    self->last_kind = toStr(kind);
    self->last_script = toStr(script);
    self->last_inputs.clear();
    for (uint64_t i = 0; i < input_count; ++i) {
      self->last_inputs.push_back(toStr(inputs[i]));
    }
    self->last_outputs.clear();
    for (uint64_t i = 0; i < output_count; ++i) {
      self->last_outputs.push_back(toStr(outputs[i]));
    }
    if (self->fail_create) {
      PJ::sdk::fillError(err, 1, "test", "create rejected");
      return false;
    }
    if (self->reject_unknown_flags && (flags & ~static_cast<uint32_t>(PJ_DATA_PROCESSOR_FLAG_EPHEMERAL)) != 0) {
      PJ::sdk::fillError(err, 1, "test", "flags: unknown reserved bit set");
      return false;
    }
    if (rejectsCrossRead(self->last_kind, self->last_script, err)) {
      return false;
    }
    // The create succeeded, so from here on the host really is holding it. An
    // ephemeral one is a dry run the host drops by itself, so it never joins the
    // set the user is left with.
    if ((flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) == 0) {
      self->live_ids.push_back(self->last_id);
      self->created.push_back(
          {self->last_id, self->last_kind, self->last_script, self->last_inputs, self->last_outputs});
    }
    self->resolved = self->last_outputs.empty() ? std::vector<std::string>{"auto_topic"} : self->last_outputs;
    // Test-only: serve the ephemeral node's resolved output as a real series,
    // so a test can drive evaluate() end to end without knowing the
    // counter-generated id ahead of time.
    if ((flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) != 0 && self->ephemeral_series_store != nullptr &&
        !self->resolved.empty()) {
      const std::string& path = self->resolved.front();
      const auto slash = path.rfind('/');
      if (slash != std::string::npos) {
        self->ephemeral_series_store->addTopic(path.substr(0, slash))
            .addField(
                path.substr(0, slash), path.substr(slash + 1), self->ephemeral_series_ts, self->ephemeral_series_vals);
      }
    }
    if (out_topics_count != nullptr) {
      *out_topics_count = self->resolved.size();
    }
    for (uint64_t i = 0; i < self->resolved.size() && i < out_topics_capacity; ++i) {
      out_topics[i] = PJ_string_view_t{self->resolved[i].data(), self->resolved[i].size()};
    }
    return true;
  }

  static bool tValidate(
      void* ctx, PJ_string_view_t kind, PJ_string_view_t /*language*/, PJ_string_view_t script,
      PJ_string_view_t /*params*/, PJ_error_t* err) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    ++self->validate_calls;
    self->last_validate_script = toStr(script);
    if (self->fail_validate) {
      PJ::sdk::fillError(err, 1, "test", self->validate_error.c_str());
      return false;
    }
    if (rejectsCrossRead(toStr(kind), self->last_validate_script, err)) {
      return false;
    }
    return true;
  }

  static bool tCheck(
      void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script, PJ_string_view_t params,
      uint32_t* out_status, PJ_error_t* err) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    if (self->check_unchecked) {
      ++self->validate_calls;
      self->last_validate_script = toStr(script);
      PJ::sdk::fillError(err, 1, "test", self->check_message.c_str());
      *out_status = PJ_SCRIPT_CHECK_UNCHECKED;
      return true;
    }
    *out_status = tValidate(ctx, kind, language, script, params, err) ? PJ_SCRIPT_CHECK_VALID : PJ_SCRIPT_CHECK_INVALID;
    return true;
  }

  // Ids this fake claims to have live, so the list/remove tools can be driven.
  // Kept separate from what create() recorded: a test needs to say "the host
  // already has these" without pretending this plugin made them here.
  static bool tList(
      void* ctx, PJ_string_view_t* out_buffer, uint64_t capacity, uint64_t* out_count, PJ_error_t* /*err*/) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    if (out_count != nullptr) {
      *out_count = self->live_ids.size();
    }
    for (uint64_t i = 0; i < self->live_ids.size() && i < capacity; ++i) {
      out_buffer[i] = PJ_string_view_t{self->live_ids[i].data(), self->live_ids[i].size()};
    }
    return true;
  }

  static bool tRemove(void* ctx, PJ_string_view_t id, PJ_error_t* /*err*/) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    self->last_removed = toStr(id);
    auto it = std::find(self->live_ids.begin(), self->live_ids.end(), self->last_removed);
    if (it != self->live_ids.end()) {
      self->live_ids.erase(it);
    }
    return true;
  }

  static bool tConfig(void* ctx, PJ_string_view_t id, PJ_string_view_t* out_recipe_json, PJ_error_t* err) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    ++self->config_calls;
    self->last_config_id = toStr(id);
    if (self->fail_config) {
      PJ::sdk::fillError(err, 1, "test", "config unavailable");
      return false;
    }
    if (!self->canned_config_json.empty()) {
      *out_recipe_json = PJ::sdk::toAbiString(self->canned_config_json);
      return true;
    }
    self->last_config_json = "{\"kind\":\"" + self->last_kind + "\"";
    if (self->config_history_exempt.has_value()) {
      self->last_config_json += std::string(",\"history_exempt\":") + (*self->config_history_exempt ? "true" : "false");
    }
    if (!self->config_series.empty()) {
      const auto& series = self->config_series[std::min(self->config_series_served++, self->config_series.size() - 1)];
      self->last_config_json += ",\"series\":{\"topic\":\"" + series.topic +
                                "\",\"rows\":" + std::to_string(series.rows) +
                                ",\"failed\":" + std::to_string(series.failed) + ",\"first_error\":\"" +
                                series.first_error + "\",\"complete\":" + (series.complete ? "true" : "false") + "}";
    }
    self->last_config_json += "}";
    if (out_recipe_json != nullptr) {
      *out_recipe_json = PJ::sdk::toAbiString(self->last_config_json);
    }
    return true;
  }

  // What the user is actually left with. This is the number a benchmark verdict
  // wants: a model that probes, measures and cleans up leaves the panel as tidy
  // as one that got it right first time, and should score the same.
  int liveCount() const {
    return static_cast<int>(live_ids.size());
  }

  // Records the typed request the same way tCreate does, then upserts it —
  // outputs carry their declared TYPE too (last_output_types), and the time
  // fields (time_flags/window/instant) that createTransform/createMarkers
  // have no equivalent of.
  static bool tCreateV2(
      void* ctx, const PJ_data_processor_request_t* request, PJ_string_view_t* out_topics, uint64_t out_topics_capacity,
      uint64_t* out_topics_count, PJ_error_t* err) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    ++self->create_v2_calls;
    ++self->create_calls;
    self->recordRequest(*request);
    self->last_create_v2_inputs = self->last_inputs;
    self->last_create_v2_script = self->last_script;
    self->last_create_v2_flags = request->flags;
    self->last_create_v2_time_flags = request->time_flags;
    self->last_create_v2_instant_ns = request->time_ns;
    self->last_create_v2_params_json = self->last_params_json;
    if ((request->flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) == 0) {
      ++self->persistent_creates;
    }
    if (self->fail_create) {
      PJ::sdk::fillError(err, 1, "test", "create rejected");
      return false;
    }
    if (self->reject_unknown_flags &&
        (request->flags & ~static_cast<uint32_t>(PJ_DATA_PROCESSOR_FLAG_EPHEMERAL)) != 0) {
      PJ::sdk::fillError(err, 1, "test", "flags: unknown reserved bit set");
      return false;
    }
    if ((request->flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) == 0) {
      self->live_ids.push_back(self->last_id);
      self->created.push_back(
          {self->last_id, self->last_kind, self->last_script, self->last_inputs, self->last_outputs});
    }
    self->resolved = self->last_outputs.empty() ? std::vector<std::string>{"auto_topic"} : self->last_outputs;
    if (out_topics_count != nullptr) {
      *out_topics_count = self->resolved.size();
    }
    for (uint64_t i = 0; i < self->resolved.size() && i < out_topics_capacity; ++i) {
      out_topics[i] = PJ_string_view_t{self->resolved[i].data(), self->resolved[i].size()};
    }
    return true;
  }

  static bool tSubmit(
      void* ctx, const PJ_data_processor_request_t* request, const PJ_evaluation_budget_t* budget, uint64_t* out_handle,
      PJ_error_t* err) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    ++self->submit_calls;
    self->recordRequest(*request);
    self->submits.push_back(
        {request->time_ns, request->flags, static_cast<std::size_t>(request->output_count), toStr(request->language),
         toStr(request->script), static_cast<std::size_t>(request->input_count), toStr(request->params_json),
         request->dataset_id});
    if (budget != nullptr) {
      self->last_budget_max_millis = budget->max_millis;
      self->last_budget_max_evaluations = budget->max_evaluations;
      self->last_budget_max_report_bytes = budget->max_report_bytes;
    }
    if (self->fail_submit) {
      PJ::sdk::fillError(err, 1, "test", "submit rejected");
      return false;
    }
    const std::uint64_t handle = self->next_handle++;
    self->poll_countdown[handle] = self->pending_polls;
    self->handle_instant[handle] = request->time_ns;
    if (out_handle != nullptr) {
      *out_handle = handle;
    }
    return true;
  }

  static bool tPoll(
      void* ctx, uint64_t handle, uint32_t* out_state, PJ_string_view_t* out_json, PJ_error_t* err) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    ++self->poll_calls;
    auto it = self->poll_countdown.find(handle);
    if (it == self->poll_countdown.end()) {
      PJ::sdk::fillError(err, 1, "test", "unknown evaluation handle");
      return false;
    }
    if (it->second > 0) {
      --it->second;
      if (out_state != nullptr) {
        *out_state = PJ_EVALUATION_STATE_PENDING;
      }
      self->last_poll_json = "{}";
    } else {
      if (out_state != nullptr) {
        *out_state = self->terminal_state;
      }
      if (self->first_sample_ns && self->handle_instant[handle] < *self->first_sample_ns) {
        self->last_poll_json =
            R"({"coverage":{"complete":false,"stopped":"error","error":"on-demand: missing input ')" +
            self->first_sample_input + "' at t=" + std::to_string(self->handle_instant[handle]) + R"("},"bundles":[]})";
      } else if (self->report_queue.empty()) {
        self->last_poll_json = self->canned_report_json;
      } else {
        self->last_poll_json = self->report_queue.front();
        self->report_queue.erase(self->report_queue.begin());
      }
    }
    if (out_json != nullptr) {
      *out_json = PJ::sdk::toAbiString(self->last_poll_json);
    }
    return true;
  }

  static bool tRelease(void* ctx, uint64_t handle, PJ_error_t* err) noexcept {
    auto* self = static_cast<RecordingDpHost*>(ctx);
    auto it = self->poll_countdown.find(handle);
    if (it == self->poll_countdown.end()) {
      PJ::sdk::fillError(err, 1, "test", "unknown evaluation handle");
      return false;
    }
    self->poll_countdown.erase(it);
    self->released_handles.push_back(handle);
    return true;
  }

  // Shared by tCreateV2/tSubmit: both take the same PJ_data_processor_request_t
  // shape, so both record it into the same last_* fields tCreate does (plus
  // the v2-only ones: types, time flags, window/instant).
  void recordRequest(const PJ_data_processor_request_t& request) {
    last_id = toStr(request.id);
    last_kind = toStr(request.kind);
    last_script = toStr(request.script);
    last_flags = request.flags;
    last_time_flags = request.time_flags;
    last_window_start_ns = request.window_start_ns;
    last_window_end_ns = request.window_end_ns;
    last_instant_ns = request.time_ns;
    last_params_json = toStr(request.params_json);
    last_inputs.clear();
    for (uint64_t i = 0; i < request.input_count; ++i) {
      last_inputs.push_back(toStr(request.inputs[i]));
    }
    last_outputs.clear();
    last_output_types.clear();
    for (uint64_t i = 0; i < request.output_count; ++i) {
      last_outputs.push_back(toStr(request.outputs[i].name));
      last_output_types.push_back(toStr(request.outputs[i].type));
    }
  }

  PJ::sdk::DataProcessorsHostView view() {
    static const PJ_data_processors_host_vtable_t vtable_full = [] {
      PJ_data_processors_host_vtable_t v{};
      v.protocol_version = 1;
      v.struct_size = sizeof(PJ_data_processors_host_vtable_t);
      v.create_data_processor = &RecordingDpHost::tCreate;
      v.remove_data_processor = &RecordingDpHost::tRemove;
      v.list_data_processor_ids = &RecordingDpHost::tList;
      v.data_processor_config = &RecordingDpHost::tConfig;
      v.validate_data_processor_script = &RecordingDpHost::tValidate;
      v.create_data_processor_v2 = &RecordingDpHost::tCreateV2;
      v.submit_evaluation = &RecordingDpHost::tSubmit;
      v.poll_evaluation = &RecordingDpHost::tPoll;
      v.release_evaluation = &RecordingDpHost::tRelease;
      v.check_data_processor_script = &RecordingDpHost::tCheck;
      return v;
    }();
    // struct_size stops right before create_data_processor_v2, so
    // PJ_HAS_TAIL_SLOT fails for every v2 slot even though the pointers below
    // it are never installed either -- either alone already fails the check,
    // both together is what a host actually built before v2 existed reports.
    static const PJ_data_processors_host_vtable_t vtable_v1_only = [] {
      PJ_data_processors_host_vtable_t v{};
      v.protocol_version = 1;
      v.struct_size = offsetof(PJ_data_processors_host_vtable_t, create_data_processor_v2);
      v.create_data_processor = &RecordingDpHost::tCreate;
      v.remove_data_processor = &RecordingDpHost::tRemove;
      v.list_data_processor_ids = &RecordingDpHost::tList;
      v.data_processor_config = &RecordingDpHost::tConfig;
      v.validate_data_processor_script = &RecordingDpHost::tValidate;
      return v;
    }();
    return PJ::sdk::DataProcessorsHostView(
        PJ_data_processors_host_t{this, supports_v2 ? &vtable_full : &vtable_v1_only});
  }
};

}  // namespace PJ::testing
