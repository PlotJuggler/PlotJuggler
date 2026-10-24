// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/OnDemandRuntimeHost.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "RuntimeHostSlot.h"
#include "pj_base/sdk/data_processors_host.hpp"  // sdk::readDataProcessorRequest, sdk::readEvaluationBudget
#include "pj_base/sdk/plugin_data_api.hpp"       // sdk::toStringView, sdk::toAbiString
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/OnDemandService.h"
#include "pj_scripting/object_script.h"
#include "pj_scripting/on_demand_script.h"

namespace PJ {
namespace {

using host_slot::fillViews;
using host_slot::guarded;
using host_slot::reject;

constexpr std::string_view kDomain = "data_processors.on_demand";
constexpr std::string_view kKind = "on_demand";
/// A transient evaluation's id namespace: no installed node holds it.
constexpr std::string_view kEvaluationIdPrefix = "~evaluation/";

/// The id namespace of one bridge's previews, `~preview/<n>/`: `n` is unique in the process, so two instances of one
/// plugin never share a preview.
std::string previewNamespace() {
  static std::atomic<uint64_t> next_instance{1};
  return std::string(RecipeGraph::kPreviewIdPrefix) + std::to_string(next_instance.fetch_add(1)) + "/";
}

bool rejectKind(PJ_error_t* out_error, PJ_string_view_t kind) {
  return reject(
      out_error, kDomain, "unknown data processor kind '" + std::string(sdk::toStringView(kind)).substr(0, 32) + "'");
}

std::string_view stateName(NodeScheduler::NodeState::Status status) {
  using Status = NodeScheduler::NodeState::Status;
  switch (status) {
    case Status::kIdle:
      return "idle";
    case Status::kPending:
      return "pending";
    case Status::kReady:
      return "ready";
    case Status::kMissingInput:
      return "missing_input";
    case Status::kError:
      return "error";
  }
  return "idle";
}

/// `GridRunner::Budget`'s defaults, overridden by each nonzero field (the ABI's 0 is "host default"); the runner
/// clamps them to its host caps.
GridRunner::Budget budgetOf(const PJ_evaluation_budget_t& abi) {
  GridRunner::Budget budget;
  if (abi.max_millis != 0) {
    budget.max_millis = abi.max_millis;
  }
  if (abi.max_bytes != 0) {
    budget.max_bytes = abi.max_bytes;
  }
  if (abi.max_evaluations != 0) {
    budget.max_evaluations = abi.max_evaluations;
  }
  if (abi.max_report_bytes != 0) {
    budget.max_report_bytes = abi.max_report_bytes;
  }
  return budget;
}

}  // namespace

void OnDemandRuntimeHost::writeReport(EnginePool& pool, std::weak_ptr<Evaluation> slot, GridRunner::Report report) {
  const bool failed = report.stopped == sdk::EvaluationStop::kError && report.bundles.empty();
  auto json = std::make_shared<std::string>();
  EnginePool::Task task;
  task.compute = [json, report = std::move(report)](const EnginePool::CancelToken&) mutable {
    const GridRunner::Report written = std::move(report);  // freed here, not on the GUI thread
    *json = GridRunner::toJson(written);
  };
  task.publish = [slot = std::move(slot), json, failed](const EnginePool::Outcome& outcome) {
    const std::shared_ptr<Evaluation> current = slot.lock();
    if (!current) {
      return;  // released meanwhile
    }
    if (outcome.status != EnginePool::Outcome::Status::kCompleted) {
      GridRunner::Report lost;
      lost.stopped = sdk::EvaluationStop::kError;
      lost.error = ("the report could not be written: " + outcome.error).substr(0, GridRunner::kMaxErrorBytes);
      current->state = PJ_EVALUATION_STATE_FAILED;
      current->json = GridRunner::toJson(lost);
      return;
    }
    current->state = failed ? PJ_EVALUATION_STATE_FAILED : PJ_EVALUATION_STATE_COMPLETED;
    current->json = std::move(*json);
  };
  pool.submit(std::move(task));
}

OnDemandRuntimeHost::OnDemandRuntimeHost(
    OnDemandService& service, const ObjectStore& store, std::string plugin_id, std::function<void()> on_topics_changed)
    : service_(service),
      store_(store),
      plugin_id_(std::move(plugin_id)),
      preview_namespace_(previewNamespace()),
      on_topics_changed_(std::move(on_topics_changed)),
      vtable_{
          .protocol_version = 1,
          .struct_size = sizeof(PJ_data_processors_host_vtable_t),
          .create_data_processor = &OnDemandRuntimeHost::onCreate,
          .remove_data_processor = &OnDemandRuntimeHost::onRemove,
          .list_data_processor_ids = &OnDemandRuntimeHost::onList,
          .data_processor_config = &OnDemandRuntimeHost::onConfig,
          .validate_data_processor_script = &OnDemandRuntimeHost::onValidate,
          .create_data_processor_v2 = &OnDemandRuntimeHost::onCreateV2,
          .submit_evaluation = &OnDemandRuntimeHost::onSubmit,
          .poll_evaluation = &OnDemandRuntimeHost::onPoll,
          .release_evaluation = &OnDemandRuntimeHost::onRelease,
          .check_data_processor_script = &OnDemandRuntimeHost::onCheck,
      },
      raw_{this, &vtable_} {}

OnDemandRuntimeHost::~OnDemandRuntimeHost() {
  for (const auto& entry : evaluations_) {
    service_.evaluations().cancel(entry.second->job);
  }
}

std::string OnDemandRuntimeHost::keyOf(std::string_view local_id, bool preview) const {
  std::string key = plugin_id_ + "/";
  if (preview) {
    key += preview_namespace_;
  }
  key += local_id;
  return key;
}

const RecipeGraph::Node* OnDemandRuntimeHost::ownNode(const std::string& key) const {
  const RecipeGraph::Node* node = service_.graph().find(key);
  return node != nullptr && node->recipe.owner_plugin == plugin_id_ ? node : nullptr;
}

const RecipeGraph::Node* OnDemandRuntimeHost::nodeOf(std::string_view local_id) const {
  const RecipeGraph::Node* preview = ownNode(keyOf(local_id, true));
  return preview != nullptr ? preview : ownNode(keyOf(local_id, false));
}

bool OnDemandRuntimeHost::owns(std::string_view local_id) const {
  return nodeOf(local_id) != nullptr;
}

Expected<RecipeGraph::Recipe> OnDemandRuntimeHost::recipeOf(const PJ_data_processor_request_t& request) const {
  RecipeGraph::Recipe recipe;
  recipe.owner_plugin = plugin_id_;
  recipe.user_id = std::string(sdk::toStringView(request.id));
  recipe.language = std::string(sdk::toStringView(request.language));
  recipe.script = std::string(sdk::toStringView(request.script));  // binary-safe
  if (request.params_json.size > kMaxParamsBytes) {
    return unexpected("params_json is larger than " + std::to_string(kMaxParamsBytes) + " bytes");
  }
  recipe.params_json = std::string(sdk::toStringView(request.params_json));
  recipe.inputs.reserve(request.input_count);
  for (uint64_t index = 0; index < request.input_count; ++index) {
    recipe.inputs.emplace_back(sdk::toStringView(request.inputs[index]));
  }
  recipe.outputs.reserve(request.output_count);
  for (uint64_t index = 0; index < request.output_count; ++index) {
    const std::string_view name = sdk::toStringView(request.outputs[index].name);
    const std::string_view type = sdk::toStringView(request.outputs[index].type);
    if (type.empty()) {
      return unexpected("output '" + std::string(name) + "' needs a type");
    }
    Expected<scripting::OutputSpec> spec = scripting::parseOutputSpec(std::string(name) + ":" + std::string(type));
    if (!spec) {
      return unexpected(spec.error());
    }
    recipe.outputs.push_back(std::move(*spec));
  }
  if (request.dataset_id != 0) {
    recipe.dataset = request.dataset_id;
  }
  return recipe;
}

std::string OnDemandRuntimeHost::recipeJson(const RecipeGraph::Node& node) const {
  const RecipeGraph::Recipe& recipe = node.recipe;
  nlohmann::json outputs = nlohmann::json::array();
  for (const scripting::OutputSpec& output : recipe.outputs) {
    outputs.push_back({{"name", output.name}, {"type", scripting::outputTypeName(output)}});
  }
  // Installed params nest at most `scripting::kMaxParamsDepth` levels (`RecipeGraph::upsert`): copy and dump recurse.
  nlohmann::json params = nlohmann::json::parse(recipe.params_json, nullptr, /*allow_exceptions=*/false);
  if (params.is_discarded()) {
    params = recipe.params_json;  // verbatim when it is not JSON
  }
  std::vector<std::string> upstream;
  for (const RecipeGraph::Binding& binding : node.bindings) {
    if (!binding.upstream.empty() && std::ranges::find(upstream, binding.upstream) == upstream.end()) {
      upstream.push_back(binding.upstream);
    }
  }
  const std::optional<NodeScheduler::NodeState> state = service_.state(recipe.key());
  const bool requested = state && state->status != NodeScheduler::NodeState::Status::kIdle;
  nlohmann::json json = {
      {"kind", kKind},
      {"language", recipe.language},
      {"inputs", recipe.inputs},
      {"dataset_id", node.dataset},
      {"outputs", std::move(outputs)},
      {"params", std::move(params)},
      {"history_exempt", recipe.history_exempt},
      {"ephemeral", recipe.ephemeral},
      {"state", stateName(state ? state->status : NodeScheduler::NodeState::Status::kIdle)},
      {"detail", state ? state->detail : std::string()},
      {"requested_ns", requested ? nlohmann::json(state->requested_t) : nlohmann::json(nullptr)},
      {"lag_ns", state && state->lag ? nlohmann::json(*state->lag) : nlohmann::json(nullptr)},
      {"upstream", upstream},
  };
  if (const std::optional<SeriesRunner::State> series = service_.series().state(recipe.key())) {
    json["series"] = {{"topic", recipe.key()},        {"rows", series->rows},
                      {"failed", series->failed},     {"first_error", series->first_error},
                      {"complete", series->complete}, {"done", series->done},
                      {"total", series->total},       {"stopped", series->stopped}};
  }
  if (recipe.pinned_t) {
    json["pinned_ns"] = *recipe.pinned_t;
    std::size_t pinned = 0;
    std::size_t bytes = 0;
    for (const std::string& key : service_.graph().keys()) {
      const RecipeGraph::Recipe& other = service_.graph().find(key)->recipe;
      if (other.owner_plugin == recipe.owner_plugin && other.pinned_t) {
        ++pinned;
        bytes += service_.history().ringBytesOf(key);
      }
    }
    json["findings"] = {{"pinned", pinned}, {"bytes", bytes}};
  }
  return json.dump();
}

bool OnDemandRuntimeHost::onCreate(
    void* ctx, PJ_string_view_t /*id*/, PJ_string_view_t kind, PJ_string_view_t /*language*/,
    const PJ_string_view_t* /*inputs*/, uint64_t /*input_count*/, const PJ_string_view_t* /*outputs*/,
    uint64_t /*output_count*/, PJ_string_view_t /*script*/, PJ_string_view_t /*params_json*/, uint32_t /*flags*/,
    PJ_string_view_t* /*out_topics*/, uint64_t /*out_topics_capacity*/, uint64_t* /*out_topics_count*/,
    PJ_error_t* out_error) noexcept {
  return guarded<OnDemandRuntimeHost>(ctx, out_error, kDomain, [&](OnDemandRuntimeHost&) {
    if (sdk::toStringView(kind) != kKind) {
      return rejectKind(out_error, kind);
    }
    return reject(out_error, kDomain, "kind 'on_demand' is created only with create_data_processor_v2");
  });
}

bool OnDemandRuntimeHost::onCreateV2(
    void* ctx, const PJ_data_processor_request_t* request, PJ_string_view_t* out_topics, uint64_t out_topics_capacity,
    uint64_t* out_topics_count, PJ_error_t* out_error) noexcept {
  return guarded<OnDemandRuntimeHost>(ctx, out_error, kDomain, [&](OnDemandRuntimeHost& self) {
    const auto decoded = sdk::readDataProcessorRequest(request, sdk::DataProcessorSlot::kCreateV2);
    if (!decoded) {
      return reject(out_error, kDomain, decoded.error());
    }
    if (sdk::toStringView(decoded->kind) != kKind) {
      return rejectKind(out_error, decoded->kind);
    }
    if ((decoded->time_flags & PJ_DATA_PROCESSOR_TIME_FLAG_WINDOW) != 0) {
      return reject(out_error, kDomain, "kind 'on_demand' takes no WINDOW on create: use submit_evaluation");
    }
    if (decoded->output_count == 0) {
      return reject(out_error, kDomain, "kind 'on_demand' needs typed outputs");
    }
    Expected<RecipeGraph::Recipe> recipe = self.recipeOf(*decoded);
    if (!recipe) {
      return reject(out_error, kDomain, recipe.error());
    }
    const bool preview = (decoded->flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) != 0;
    const std::string_view local_id = sdk::toStringView(decoded->id);
    // One id names one recipe: a lifetime change needs a remove first, or one id would hold two nodes.
    if (const RecipeGraph::Node* held = self.nodeOf(local_id); held != nullptr && held->recipe.ephemeral != preview) {
      return reject(
          out_error, kDomain,
          std::string("'") + std::string(local_id).substr(0, 64) + "' is a " +
              (preview ? "persistent recipe" : "preview") + ": remove it before creating it as a " +
              (preview ? "preview" : "persistent recipe"));
    }
    if (preview) {
      recipe->user_id = self.preview_namespace_ + recipe->user_id;
    }
    recipe->ephemeral = preview;
    recipe->history_exempt = (decoded->flags & PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT) != 0;
    if ((decoded->time_flags & PJ_DATA_PROCESSOR_TIME_FLAG_INSTANT) != 0) {
      recipe->pinned_t = decoded->time_ns;
    }
    const std::string key = recipe->key();
    if (auto installed = self.service_.install(std::move(*recipe)); !installed) {
      return reject(out_error, kDomain, installed.error());
    }
    if (preview) {
      self.previews_.insert(key);
    }
    const RecipeGraph::Node& node = *self.service_.graph().find(key);
    const std::vector<ObjectTopicId> topics = self.service_.history().topicsOf(key);
    self.create_topics_storage_.clear();
    std::size_t object = 0;
    for (const scripting::OutputSpec& output : node.recipe.outputs) {
      self.create_topics_storage_.push_back(
          output.type == scripting::OutputType::kObject ? self.store_.descriptor(topics.at(object++)).topic_name
                                                        : output.name);
    }
    fillViews(self.create_topics_storage_, out_topics, out_topics_capacity, out_topics_count);
    if (self.on_topics_changed_) {
      self.on_topics_changed_();
    }
    return true;
  });
}

bool OnDemandRuntimeHost::onRemove(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept {
  return guarded<OnDemandRuntimeHost>(ctx, out_error, kDomain, [&](OnDemandRuntimeHost& self) {
    const RecipeGraph::Node* node = self.nodeOf(sdk::toStringView(id));
    if (node == nullptr) {
      return reject(out_error, kDomain, "unknown data processor id");
    }
    const std::string key = node->recipe.key();
    (void)self.service_.remove(key);
    self.previews_.erase(key);
    if (self.on_topics_changed_) {
      self.on_topics_changed_();
    }
    return true;
  });
}

bool OnDemandRuntimeHost::onList(
    void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t* out_error) noexcept {
  return guarded<OnDemandRuntimeHost>(ctx, out_error, kDomain, [&](OnDemandRuntimeHost& self) {
    self.list_storage_.clear();
    const RecipeGraph& graph = self.service_.graph();
    for (const std::string& key : graph.keys()) {
      const RecipeGraph::Recipe& recipe = graph.find(key)->recipe;
      if (recipe.owner_plugin == self.plugin_id_ && !recipe.ephemeral) {
        self.list_storage_.push_back(recipe.user_id);
      }
    }
    fillViews(self.list_storage_, out_ids, capacity, out_count);
    return true;
  });
}

bool OnDemandRuntimeHost::onConfig(
    void* ctx, PJ_string_view_t id, PJ_string_view_t* out_recipe_json, PJ_error_t* out_error) noexcept {
  return guarded<OnDemandRuntimeHost>(ctx, out_error, kDomain, [&](OnDemandRuntimeHost& self) {
    const RecipeGraph::Node* node = self.nodeOf(sdk::toStringView(id));
    if (node == nullptr) {
      return reject(out_error, kDomain, "unknown data processor id");
    }
    self.config_storage_ = self.recipeJson(*node);
    if (out_recipe_json != nullptr) {
      *out_recipe_json = sdk::toAbiString(self.config_storage_);
    }
    return true;
  });
}

bool OnDemandRuntimeHost::onValidate(
    void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script,
    PJ_string_view_t /*params_json*/, PJ_error_t* out_error) noexcept {
  return guarded<OnDemandRuntimeHost>(ctx, out_error, kDomain, [&](OnDemandRuntimeHost&) {
    if (sdk::toStringView(kind) != kKind) {
      return rejectKind(out_error, kind);
    }
    const Expected<sdk::ScriptCheck> check =
        scripting::checkOnDemand(sdk::toStringView(language), sdk::toStringView(script));
    if (!check) {
      return reject(out_error, kDomain, check.error());
    }
    // No third answer here: an unchecked script passes; onCheck reports it unchecked.
    return check->status != sdk::ScriptCheckStatus::kInvalid || reject(out_error, kDomain, check->message);
  });
}

bool OnDemandRuntimeHost::onCheck(
    void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script,
    PJ_string_view_t /*params_json*/, uint32_t* out_status, PJ_error_t* out_error) noexcept {
  return guarded<OnDemandRuntimeHost>(ctx, out_error, kDomain, [&](OnDemandRuntimeHost&) {
    if (sdk::toStringView(kind) != kKind) {
      return rejectKind(out_error, kind);
    }
    if (out_status == nullptr) {
      return reject(out_error, kDomain, "out_status is NULL");
    }
    // Python waits at most `kPythonValidateWaitMillis` for a throwaway compile, then answers unchecked.
    const Expected<sdk::ScriptCheck> check =
        scripting::checkOnDemand(sdk::toStringView(language), sdk::toStringView(script));
    if (!check) {
      return reject(out_error, kDomain, check.error());
    }
    *out_status = static_cast<uint32_t>(check->status);
    if (!check->message.empty()) {
      sdk::fillError(out_error, PJ_ERROR_CODE_REJECTED, kDomain, check->message);
    }
    return true;
  });
}

bool OnDemandRuntimeHost::onSubmit(
    void* ctx, const PJ_data_processor_request_t* request, const PJ_evaluation_budget_t* budget, uint64_t* out_handle,
    PJ_error_t* out_error) noexcept {
  return guarded<OnDemandRuntimeHost>(ctx, out_error, kDomain, [&](OnDemandRuntimeHost& self) {
    const auto decoded = sdk::readDataProcessorRequest(request, sdk::DataProcessorSlot::kSubmitEvaluation);
    if (!decoded) {
      return reject(out_error, kDomain, decoded.error());
    }
    const auto decoded_budget = sdk::readEvaluationBudget(budget);
    if (!decoded_budget) {
      return reject(out_error, kDomain, decoded_budget.error());
    }
    if (sdk::toStringView(decoded->kind) != kKind) {
      return rejectKind(out_error, decoded->kind);
    }
    if (out_handle == nullptr) {
      return reject(out_error, kDomain, "out_handle is NULL");
    }
    const GridRunner::Budget limits = budgetOf(*decoded_budget);
    const std::size_t reservation = std::min(limits.max_report_bytes, GridRunner::kHostCaps.max_report_bytes);
    if (self.evaluations_.size() >= kMaxHandles || reservation > kMaxReportBytes - self.report_bytes_reserved_) {
      return reject(out_error, kDomain, "too many live evaluations: release finished handles first");
    }
    if (self.next_handle_ == std::numeric_limits<uint64_t>::max()) {
      return reject(out_error, kDomain, "evaluation handles exhausted");
    }

    RecipeGraph::Node node;
    const bool installed = decoded->script.size == 0;
    if (installed) {
      const RecipeGraph::Node* found = self.nodeOf(sdk::toStringView(decoded->id));
      if (found == nullptr) {
        return reject(out_error, kDomain, "unknown data processor id");
      }
      node = *found;
    } else {
      Expected<RecipeGraph::Recipe> recipe = self.recipeOf(*decoded);
      if (!recipe) {
        return reject(out_error, kDomain, recipe.error());
      }
      recipe->user_id = std::string(kEvaluationIdPrefix) + recipe->user_id;
      recipe->ephemeral = true;
      Expected<RecipeGraph::Node> bound = self.service_.graph().bind(std::move(*recipe));
      if (!bound) {
        return reject(out_error, kDomain, bound.error());
      }
      node = std::move(*bound);
    }
    const bool instant = (decoded->time_flags & PJ_DATA_PROCESSOR_TIME_FLAG_INSTANT) != 0;
    const Timestamp start = instant ? decoded->time_ns : decoded->window_start_ns;
    const Timestamp end = instant ? decoded->time_ns : decoded->window_end_ns;

    auto evaluation = std::make_shared<Evaluation>();
    evaluation->queued = Submission{std::move(node), installed, start, end, limits};
    if (self.running() < kMaxRunningEvaluations) {  // else one runs, so the queue moves on when it ends
      if (Status started = self.start(evaluation); !started) {
        return reject(out_error, kDomain, started.error());
      }
    } else {
      self.queue_.push_back(evaluation);
    }
    evaluation->reserved_bytes = reservation;
    self.report_bytes_reserved_ += reservation;
    const uint64_t handle = self.next_handle_++;
    self.evaluations_.emplace(handle, std::move(evaluation));
    *out_handle = handle;
    return true;
  });
}

std::size_t OnDemandRuntimeHost::running() const {
  return static_cast<std::size_t>(
      std::ranges::count_if(evaluations_, [](const auto& entry) { return entry.second->job != nullptr; }));
}

Status OnDemandRuntimeHost::start(const std::shared_ptr<Evaluation>& evaluation) {
  Submission submission = std::move(*evaluation->queued);
  evaluation->queued.reset();
  std::weak_ptr<Evaluation> slot = evaluation;
  // The runner calls back only while it lives, and the pool outlives it; this bridge cancels every job it destroys.
  EnginePool& pool = service_.pool();
  auto job = service_.evaluations().submit(
      std::move(submission.node), submission.installed, submission.start, submission.end, submission.limits,
      [this, slot, &pool](GridRunner::Report report) {
        if (const std::shared_ptr<Evaluation> current = slot.lock()) {
          current->job.reset();
          writeReport(pool, slot, std::move(report));
        }
        dispatch();
      });
  if (!job) {
    return unexpected(job.error());
  }
  evaluation->job = std::move(*job);
  return okStatus();
}

void OnDemandRuntimeHost::dispatch() {
  while (!queue_.empty() && running() < kMaxRunningEvaluations) {
    const std::shared_ptr<Evaluation> next = queue_.front().lock();
    queue_.pop_front();
    if (next == nullptr) {
      continue;  // released while it waited
    }
    if (Status started = start(next); !started) {
      GridRunner::Report refused;
      refused.stopped = sdk::EvaluationStop::kError;
      refused.error = started.error().substr(0, GridRunner::kMaxErrorBytes);
      next->state = PJ_EVALUATION_STATE_FAILED;
      next->json = GridRunner::toJson(refused);
    }
  }
}

bool OnDemandRuntimeHost::onPoll(
    void* ctx, uint64_t handle, uint32_t* out_state, PJ_string_view_t* out_json, PJ_error_t* out_error) noexcept {
  return guarded<OnDemandRuntimeHost>(ctx, out_error, kDomain, [&](OnDemandRuntimeHost& self) {
    const auto found = self.evaluations_.find(handle);
    if (found == self.evaluations_.end()) {
      return reject(out_error, kDomain, "unknown evaluation handle");
    }
    if (out_state == nullptr) {
      return reject(out_error, kDomain, "out_state is NULL");
    }
    *out_state = found->second->state;
    if (out_json != nullptr) {
      *out_json = sdk::toAbiString(found->second->json);
    }
    return true;
  });
}

bool OnDemandRuntimeHost::onRelease(void* ctx, uint64_t handle, PJ_error_t* out_error) noexcept {
  return guarded<OnDemandRuntimeHost>(ctx, out_error, kDomain, [&](OnDemandRuntimeHost& self) {
    const auto found = self.evaluations_.find(handle);
    if (found == self.evaluations_.end()) {
      return reject(out_error, kDomain, "unknown or already released evaluation handle");
    }
    self.service_.evaluations().cancel(found->second->job);
    self.report_bytes_reserved_ -= found->second->reserved_bytes;
    self.evaluations_.erase(found);
    self.dispatch();
    return true;
  });
}

}  // namespace PJ
