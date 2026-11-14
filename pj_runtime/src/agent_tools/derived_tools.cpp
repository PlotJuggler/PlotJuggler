// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Agent tool executors. Each is a pure function of (args, host views) and
// returns a ToolResult whose `content` the model reads. Every failure is
// returned as data (ok=false), never thrown.
//
// This file: create_derived_series, evaluate, create_derived_object, create_markers, remove_markers, list_created,
// remove_derived_series.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <pj_base/builtin/plot_markers.hpp>
#include <string>
#include <vector>

#include "pj_runtime/agent_tools/series_stats.hpp"
#include "tools_internal.hpp"

namespace PJ::agent_tools::detail {
namespace {

// Every script the tools build or accept is Luau.
constexpr const char* kScriptLanguage = "luau";

const char* markerKindName(PJ::sdk::MarkerKind k) {
  switch (k) {
    case PJ::sdk::MarkerKind::kRegion:
      return "regions";
    case PJ::sdk::MarkerKind::kEvent:
      return "events";
    case PJ::sdk::MarkerKind::kValueBand:
      return "value_bands";
    case PJ::sdk::MarkerKind::kLabel:
      return "labels";
  }
  return "other";
}

// What the host ACTUALLY published for a marker set, read back out of the
// object store.
//
// This exists because the model cannot see the plot. Told only "created", it
// has no way to distinguish twelve shaded regions from four thousand vertical
// lines that merge into a wall — and it will report both as a success, because
// from where it sits they are identical. The breakdown by kind is the part that
// carries the meaning: "4182 events" is a wall, "12 regions" is an annotation.
//
// A marker topic holds ONE serialized PlotMarkers blob (MarkerService pushes the
// whole set at Timestamp{0} and republishes it on every change), so entryCount()
// would report 1 no matter how many markers there are — the payload has to be
// decoded. Same read path the anomaly detector uses for its live preview.
//
// Returns a null json when the optional read service is absent: the answer then
// carries no count, which is strictly better than the tool failing.
json publishedMarkerSummary(const ToolContext& ctx, const std::vector<std::string>& object_topics) {
  if (!ctx.objects.valid()) {
    return nullptr;
  }
  std::size_t total = 0;
  std::map<std::string, std::size_t> by_kind;
  std::vector<std::pair<PJ::Timestamp, PJ::Timestamp>> regions;
  for (const auto& name : object_topics) {
    const std::optional<PJ::sdk::ObjectTopicHandle> handle = ctx.objects.lookupTopic(name);
    if (!handle) {
      continue;
    }
    const PJ::Expected<PJ::sdk::ObjectBytes> bytes = ctx.objects.readLatestAt(*handle, PJ::Timestamp{0});
    if (!bytes || bytes->empty()) {
      continue;
    }
    const PJ::Span<const uint8_t> view = bytes->view();
    const PJ::Expected<PJ::sdk::PlotMarkers> decoded = PJ::deserializePlotMarkers(view.data(), view.size());
    if (!decoded) {
      continue;
    }
    for (const auto& m : decoded->markers) {
      ++total;
      ++by_kind[markerKindName(m.kind)];
      if (m.kind == PJ::sdk::MarkerKind::kRegion && m.t_end > m.t_start) {
        regions.emplace_back(m.t_start, m.t_end);
      }
    }
  }
  json out = {{"markers_created", total}};
  if (!by_kind.empty()) {
    out["by_kind"] = by_kind;
  }
  // `covered_s` is the UNION of the region intervals — the log time the regions
  // actually cover, which is what a model quotes back to the user. The envelope
  // (first start to last end) used to be reported here as `span_s`, and models
  // read it as coverage every single time: two regions covering 95.1 s were
  // announced as "~122 s, half the drive" because the slow stretch between them
  // sat inside the envelope. Zero-duration marker kinds contribute nothing, and
  // a set with no regions omits the field rather than reporting a meaningless 0.
  if (!regions.empty()) {
    std::sort(regions.begin(), regions.end());
    PJ::Timestamp covered_ns = 0;
    PJ::Timestamp cur_start = regions.front().first;
    PJ::Timestamp cur_end = regions.front().second;
    for (std::size_t i = 1; i < regions.size(); ++i) {
      if (regions[i].first > cur_end) {
        covered_ns += cur_end - cur_start;
        cur_start = regions[i].first;
        cur_end = regions[i].second;
      } else {
        cur_end = std::max(cur_end, regions[i].second);
      }
    }
    covered_ns += cur_end - cur_start;
    out["covered_s"] = static_cast<double>(covered_ns) * 1e-9;
  }
  return out;
}

// Whether the HOST can address this series by name for a marker rule, and the
// error to hand back when it cannot. A rule names its series by bare name;
// when the same topic/field exists in several datasets the bare name no longer
// picks one, so handing it over would either fail or land on whichever dataset
// the host resolves first. Reads are unaffected — they go by handle — so this
// is a create-only limit, refused loudly here instead of silently mistargeted
// there.
std::optional<std::string> hostCreateBlocker(const PJ::sdk::CatalogSnapshot& catalog, const ResolvedSeries& resolved) {
  const auto bare = resolveSeriesPath(catalog, resolved.host_path);
  if (!bare.ambiguous) {
    return std::nullopt;
  }
  return "cannot create from '" + resolved.path + "': several loaded datasets share the path '" + resolved.host_path +
         "', and the host's create interface addresses inputs by bare name, so it cannot target that specific "
         "dataset. Reading it works (read_series); to build on it, ask the user to keep only the relevant file "
         "loaded.";
}

// The name a transform input reaches the host by: the bare path, or the
// dataset-qualified one ("dataset_source:topic/field", which the host resolves
// in that dataset) when several datasets share the bare path.
std::string transformInputPath(const PJ::sdk::CatalogSnapshot& catalog, const ResolvedSeries& resolved) {
  return resolveSeriesPath(catalog, resolved.host_path).ambiguous ? resolved.path : resolved.host_path;
}

// Persistent nodes request exclusion from undo/redo, but only when the SDK
// exposes the bit. A "reserved" substring in the error identifies a host that
// rejects the unknown bit and needs one flags-free retry.
template <typename Fn>
auto createWithFlagFallback(Fn&& fn, bool& degraded) {
  uint32_t flags = 0;
#ifdef PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT
  flags |= PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT;
#endif
  auto result = fn(flags);
  if (!result && flags != 0 && result.error().find("reserved") != std::string::npos) {
    flags = 0;
    result = fn(flags);
    degraded = static_cast<bool>(result);
  }
  return result;
}

#ifdef PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT
// Confirm that the host persisted the requested property in the node recipe.
bool historyExemptConfirmed(ToolContext& ctx, const std::string& id) {
  auto recipe = ctx.dp.recipeOf(id);
  if (!recipe) {
    return false;
  }
  try {
    return json::parse(*recipe).value("history_exempt", false);
  } catch (const json::exception&) {
    return false;
  }
}

#endif

// createWithFlagFallback, followed by the confirmation probe: a successful
// flagged create is protected only when its recipe confirms it, so
// `undo_protection_unavailable` is set unless that check passes. Skipped when
// the fallback already degraded (that retry already established the answer)
// or when the create itself failed (nothing was persisted to confirm).
template <typename Fn>
auto createHistoryExempt(ToolContext& ctx, const std::string& id, Fn&& fn, bool& undo_protection_unavailable) {
  auto result = createWithFlagFallback(std::forward<Fn>(fn), undo_protection_unavailable);
  if (result && !undo_protection_unavailable) {
#ifdef PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT
    if (!historyExemptConfirmed(ctx, id)) {
      undo_protection_unavailable = true;
    }
#else
    (void)ctx;
    (void)id;
    // Built against an SDK with no history-exempt bit, so nothing was ever
    // requested and the node IS in undo's reach. Say so: the reject-retry path
    // above discloses the same thing, and without this the one configuration
    // that actually ships today is the only one that stays silent -- the model
    // then tells the user it created something persistent, and an undo removes
    // it.
    undo_protection_unavailable = true;
#endif
  }
  return result;
}

// Adds the disclosure key when undo/redo protection could not be secured for
// the node just created. The wording carries the consequence, not just the
// fact: a bare "unavailable" leaves the model to guess what it means for the
// user, and the reason differs (this build has no flag / this host refused or
// did not confirm it) while the consequence never does.
void annotateUndoProtection(json& result, bool unavailable) {
  if (unavailable) {
    result["undo_protection"] = "unavailable: an undo can remove this";
  }
}

// Unique per-call id for evaluate()'s ephemeral node, scoped to this
// plugin's own id namespace on the host (so two plugin instances, or two
// calls in flight, never collide). Never reused — a leftover from a failed
// remove is easy to spot rather than silently shadowed by the next call.
std::atomic<unsigned> g_evaluate_counter{0};

// --- evaluate()'s object / on-demand path -----------------------------------
//
// Selected when any input is an object topic, or 'at_s'/'window' was given —
// see evaluateSeries below. Unlike the scalar path (a per-sample transform
// read back through the catalog), this submits a kind="on_demand" request
// through DataProcessorsHostView::submitEvaluation and reads the answer out
// of the report JSON directly: objects never round-trip through the catalog
// as bytes, only as the host's own summaries.

// One evaluation bundle's *_ns fields (requested_ns, stamp_ns, each input's
// resolved_ns), converted to display seconds (*_s) via the same anchor
// toRawNs used going in. The untouched raw values ride along under "raw_ns"
// only when `debug` is set — the model reads display seconds by default,
// same as every other time this file reports.
json convertBundleTimes(const json& bundle_in, ToolContext& ctx, PJ::sdk::DataSourceHandle source, bool debug) {
  json bundle = bundle_in;
  json raw = json::object();
  auto toDisplay = [&](std::int64_t ns) {
    if (auto d = ctx.playback.toDisplayTimeForSource(source, ns)) {
      return *d;
    }
    return static_cast<double>(ns) * 1e-9;
  };
  auto convertTop = [&](const char* key_ns, const char* key_s) {
    if (bundle.contains(key_ns) && bundle[key_ns].is_number_integer()) {
      const std::int64_t ns = bundle[key_ns].get<std::int64_t>();
      if (debug) {
        raw[key_ns] = ns;
      }
      bundle[key_s] = toDisplay(ns);
      bundle.erase(key_ns);
    }
  };
  convertTop("requested_ns", "requested_s");
  convertTop("stamp_ns", "stamp_s");
  if (bundle.contains("inputs") && bundle["inputs"].is_array()) {
    for (auto& in : bundle["inputs"]) {
      if (in.contains("resolved_ns") && in["resolved_ns"].is_number_integer()) {
        const std::int64_t ns = in["resolved_ns"].get<std::int64_t>();
        in["resolved_s"] = toDisplay(ns);
        in.erase("resolved_ns");
      }
    }
  }
  if (debug && !raw.empty()) {
    bundle["raw_ns"] = raw;
  }
  return bundle;
}

// A report's "bundles" array, time-converted and capped to the LAST 50 (the
// most recent instants are what a model asks a WINDOW evaluation for; the
// earliest ones are the ones worth dropping when the budget produced more
// than fit in a response).
struct RenderedBundles {
  json bundles = json::array();
  bool truncated = false;
};

RenderedBundles renderBundles(ToolContext& ctx, PJ::sdk::DataSourceHandle source, const json& report, bool debug) {
  RenderedBundles out;
  if (!report.contains("bundles") || !report["bundles"].is_array()) {
    return out;
  }
  const auto& arr = report["bundles"];
  constexpr std::size_t kMaxBundles = 50;
  const std::size_t total = arr.size();
  const std::size_t start = total > kMaxBundles ? total - kMaxBundles : 0;
  for (std::size_t i = start; i < total; ++i) {
    out.bundles.push_back(convertBundleTimes(arr[i], ctx, source, debug));
  }
  out.truncated = total > kMaxBundles;
  return out;
}

// submitEvaluation + poll-to-completion + releaseEvaluation (RAII, every
// path), shared by evaluate's object path and create_derived_object's
// post-create finding read. `error` is the host's or a timeout message;
// `report` is the parsed report JSON on success only.
struct SubmitAndPollResult {
  bool ok = false;
  json report;
  std::string error;
};

// The host's completion is drained by the GUI event loop. Polling must therefore
// yield between ticks, never sleep on that same thread.
struct PendingEvaluation : std::enable_shared_from_this<PendingEvaluation> {
  PJ::sdk::DataProcessorsHostView host;
  std::uint64_t handle = 0;
  bool released = false;
  std::chrono::steady_clock::time_point deadline;
  std::function<ToolResult(ToolContext&, const SubmitAndPollResult&)> finish;

  ~PendingEvaluation() {
    release();
  }
  void release() {
    if (!released) {
      released = true;
      (void)host.releaseEvaluation(handle);
    }
  }
  ToolResult poll(ToolContext& ctx) {
    SubmitAndPollResult result;
    auto value = host.pollEvaluation(handle);
    if (!value) {
      result.error = value.error();
    } else if (value->state == PJ::sdk::EvaluationState::kPending) {
      if (std::chrono::steady_clock::now() < deadline) {
        return ToolResult::deferred([self = shared_from_this()](ToolContext& current) { return self->poll(current); });
      }
      result.error = "timed out waiting for the host";
    } else if (value->state == PJ::sdk::EvaluationState::kFailed) {
      // A FAILED report has the shape of a COMPLETED one; its reason is coverage.error.
      const auto report = json::parse(value->json, nullptr, false);
      result.error = value->json;
      if (report.is_object() && report.contains("coverage") && report["coverage"].is_object()) {
        const auto& coverage = report["coverage"];
        if (coverage.contains("error") && coverage["error"].is_string()) {
          result.error = coverage["error"].get<std::string>();
        }
      }
    } else {
      result.report = json::parse(value->json, nullptr, false);
      result.ok = result.report.is_object();
      if (!result.ok) {
        result.error = "the host returned an unreadable report";
      }
    }
    release();
    return finish(ctx, result);
  }
};

ToolResult submitAndPoll(
    ToolContext& ctx, const PJ::sdk::DataProcessorRequest& request, std::uint64_t budget_ms,
    std::uint64_t budget_evaluations, std::function<ToolResult(ToolContext&, const SubmitAndPollResult&)> finish) {
  PJ::sdk::EvaluationBudget budget;
  budget.max_millis = budget_ms;
  budget.max_evaluations = budget_evaluations;
  auto handle = ctx.dp.submitEvaluation(request, budget);
  if (!handle) {
    return finish(ctx, SubmitAndPollResult{false, {}, handle.error()});
  }
  auto pending = std::make_shared<PendingEvaluation>();
  pending->host = ctx.dp;
  pending->handle = *handle;
  // The plugin does not cap the budget: the host applies its own limits and may clamp what was asked for.
  // The poll deadline is the request plus a margin, which also covers a host that grants less; a request
  // too large to add to the clock saturates instead of overflowing.
  constexpr std::uint64_t kDeadlineMarginMs = 1000;
  constexpr std::uint64_t kMaxDeadlineMs = 24ULL * 3600 * 1000 * 365;  // a year: far beyond any real budget
  pending->deadline = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(std::min(budget_ms, kMaxDeadlineMs) + kDeadlineMarginMs);
  pending->finish = std::move(finish);
  return pending->poll(ctx);
}

// The budget asked for: a positive value (at least 1), with a default when the call gives none. No upper
// bound here: the host owns that policy and clamps what it grants.
std::uint64_t requestedBudgetMs(const json& args) {
  return static_cast<std::uint64_t>(std::max<std::int64_t>(args.value("budget_ms", std::int64_t{1000}), 1));
}

std::uint64_t requestedBudgetEvaluations(const json& args) {
  return static_cast<std::uint64_t>(std::max<std::int64_t>(args.value("budget_evaluations", std::int64_t{50}), 1));
}

// The host's error text for an installed node whose recipe is in the "error" state, or nullopt when the
// config is unreadable or the node is not in error.
std::optional<std::string> installedNodeErrorDetail(ToolContext& ctx, const std::string& id) {
  auto recipe = ctx.dp.recipeOf(id);
  if (!recipe) {
    return std::nullopt;
  }
  const auto config = json::parse(*recipe, nullptr, false);
  if (!config.is_object() || config.value("state", std::string{}) != "error") {
    return std::nullopt;
  }
  const auto detail = config.find("detail");
  return detail != config.end() && detail->is_string() ? detail->get<std::string>() : std::string("unknown error");
}

// evaluate()'s object path proper: validate 'outputs'/'body', resolve every
// input (object or scalar), turn 'at_s'/'window' (display seconds) or the
// current playhead into raw ns, submit an EPHEMERAL on_demand evaluation,
// and hand back the time-converted report. Never installs or notifies —
// same contract as the scalar path's ephemeral transform.
ToolResult evaluateObjectPath(const json& args, ToolContext& ctx, const std::vector<std::string>& raw_inputs) {
  if (!args.contains("outputs") || !args["outputs"].is_array() || args["outputs"].empty()) {
    return ToolResult::failure(
        "evaluate over objects (an object input, 'at_s' or 'window') requires 'outputs': an array of "
        "\"name:type\" (type is 'number', 'string', or a builtin object type like 'kPointCloud')");
  }
  const ParsedOutputs parsed_outputs = parseTypedOutputs(args["outputs"]);
  if (!parsed_outputs.error.empty()) {
    return ToolResult::failure(parsed_outputs.error);
  }
  if (!args.contains("body") || !args["body"].is_string() || args["body"].get<std::string>().empty()) {
    return ToolResult::failure(
        "evaluate over objects requires 'body': a Luau chunk body reading inputs[\"<topic>\"] and returning a "
        "table of the declared outputs ('expression' only applies to the plain scalar path)");
  }
  if (args.contains("at_s") && args.contains("window")) {
    return ToolResult::failure("evaluate takes 'at_s' OR 'window', not both");
  }

  auto v2 = ctx.host.catalogSnapshotV2();
  if (!v2) {
    return ToolResult::failure(
        "evaluating at a time or over objects requires catalog snapshot v2, which this host does not expose: " +
        v2.error());
  }
  const ResolvedEvalInputs resolved = resolveEvalInputs(ctx.host, *v2, raw_inputs);
  if (!resolved.error.empty()) {
    return ToolResult::failure(resolved.error);
  }
  if (!resolved.anchor_source) {
    return ToolResult::failure("evaluate needs at least one resolvable input to anchor its dataset's time conversion");
  }

  std::optional<std::int64_t> instant_ns;
  std::optional<std::pair<std::int64_t, std::int64_t>> window_ns;
  if (args.contains("window")) {
    if (!args["window"].is_object() || !args["window"].contains("start_s") || !args["window"].contains("end_s") ||
        !args["window"]["start_s"].is_number() || !args["window"]["end_s"].is_number()) {
      return ToolResult::failure("'window' must be {\"start_s\": <number>, \"end_s\": <number>} (display seconds)");
    }
    const double start_s = args["window"]["start_s"].get<double>();
    const double end_s = args["window"]["end_s"].get<double>();
    if (end_s <= start_s) {
      return ToolResult::failure("window.end_s must be greater than window.start_s");
    }
    const auto raw_start = toRawNs(ctx.playback, *resolved.anchor_source, start_s);
    const auto raw_end = toRawNs(ctx.playback, *resolved.anchor_source, end_s);
    if (!raw_start || !raw_end) {
      return ToolResult::failure(
          "cannot convert 'window' to raw time: the host did not expose pj.playback.v1 or per-source time "
          "conversion");
    }
    window_ns = std::pair<std::int64_t, std::int64_t>{*raw_start, *raw_end};
  } else if (args.contains("at_s")) {
    if (!args["at_s"].is_number()) {
      return ToolResult::failure("'at_s' must be a number (display seconds)");
    }
    instant_ns = toRawNs(ctx.playback, *resolved.anchor_source, args["at_s"].get<double>());
    if (!instant_ns) {
      return ToolResult::failure(
          "cannot convert 'at_s' to raw time: the host did not expose pj.playback.v1 or per-source time "
          "conversion");
    }
  } else {
    if (!ctx.playback.valid()) {
      return ToolResult::failure("the host did not expose pj.playback.v1 (cannot evaluate at the current playhead)");
    }
    auto state = ctx.playback.state();
    if (!state) {
      return ToolResult::failure("playback state unavailable: " + state.error());
    }
    instant_ns = toRawNs(ctx.playback, *resolved.anchor_source, state->current_time_s);
    if (!instant_ns) {
      return ToolResult::failure("cannot convert the current playhead to raw time for this dataset");
    }
  }

  const std::string body = args["body"].get<std::string>();
  const std::string script = buildResolvedOnDemandChunk(body, resolved);
  if (auto v = ctx.dp.validateScript("on_demand", kScriptLanguage, script); !v) {
    return ToolResult::failure("invalid script: " + v.error());
  }

  const unsigned call_id = ++g_evaluate_counter;
  const std::string id = "__evaluate_" + std::to_string(call_id);
  std::vector<std::string> input_names;
  for (const auto& ri : resolved.inputs) {
    input_names.push_back(ri.request_path);
  }

  PJ::sdk::DataProcessorRequest request;
  request.id = id;
  request.kind = "on_demand";
  request.language = kScriptLanguage;
  request.script = script;
  request.params_json = "{}";
  request.inputs = input_names;
  request.outputs = parsed_outputs.outputs;
  request.flags = PJ_DATA_PROCESSOR_FLAG_EPHEMERAL;
  request.window = window_ns;
  request.instant_ns = instant_ns;

  return submitAndPoll(
      ctx, request, requestedBudgetMs(args), requestedBudgetEvaluations(args),
      [source = *resolved.anchor_source, debug = args.value("debug", false)](
          ToolContext& current, const SubmitAndPollResult& polled) {
        if (!polled.ok) {
          return ToolResult::failure("evaluate failed: " + polled.error);
        }
        json out = json::object();
        if (polled.report.contains("coverage")) {
          out["coverage"] = polled.report["coverage"];
        }
        const RenderedBundles rendered = renderBundles(current, source, polled.report, debug);
        out["bundles"] = rendered.bundles;
        if (rendered.truncated) {
          out["truncated"] = true;
        }
        return ToolResult::success(out.dump());
      });
}

// Raw form: the model authored the whole Luau rule; declare its inputs, pass
// the script through verbatim, and address the resulting marker set at
// `output` (a series path, or "__global__" for every plot of the dataset;
// defaults to the first input so markers land where the data is).
ToolResult createMarkersFromRule(const json& args, ToolContext& ctx) {
  const std::string rule = args["rule"].get<std::string>();
  if (rule.empty()) {
    return ToolResult::failure("create_markers 'rule' must be a non-empty Luau script");
  }
  if (!args.contains("inputs") || !args["inputs"].is_array() || args["inputs"].empty()) {
    return ToolResult::failure(
        "create_markers with 'rule' requires a non-empty 'inputs' array of topic/field paths "
        "(the series the rule reads via series(...))");
  }
  std::vector<std::string> inputs;
  for (const auto& in : args["inputs"]) {
    if (!in.is_string()) {
      return ToolResult::failure("'inputs' entries must be strings (topic/field paths)");
    }
    inputs.push_back(canonicalSeriesPath(in.get<std::string>()));
  }
  // Same resolution as create_derived_series, and for the same reason: the rule
  // looks its inputs up by exact name via series(...), so a path that is merely
  // close produces a rule that matches nothing and silently draws no markers.
  if (auto catalog = ctx.host.catalogSnapshot()) {
    for (auto& in : inputs) {
      auto lookup = resolveSeriesPath(*catalog, in);
      if (!lookup.resolved) {
        return ToolResult::failure(seriesLookupError(in, lookup));
      }
      if (auto blocked = hostCreateBlocker(*catalog, *lookup.resolved)) {
        return ToolResult::failure(*blocked);
      }
      in = lookup.resolved->host_path;
    }
  }
  const std::string output = args.contains("output") && args["output"].is_string()
                                 ? canonicalSeriesPath(args["output"].get<std::string>())
                                 : inputs.front();

  std::vector<std::string_view> input_views(inputs.begin(), inputs.end());
  bool undo_protection_unavailable = false;
  auto topics = createHistoryExempt(
      ctx, "assistant_markers",
      [&](uint32_t flags) {
        return ctx.dp.createMarkers(
            "assistant_markers", PJ::Span<const std::string_view>(input_views.data(), input_views.size()), output, rule,
            "{}", flags);
      },
      undo_protection_unavailable);
  if (!topics) {
    return ToolResult::failure("create_markers failed: " + topics.error());
  }
  if (ctx.notify_data_changed) {
    ctx.notify_data_changed();
  }
  json result = {{"created_markers_on", output}, {"inputs", inputs}, {"form", "rule"}};
  annotateUndoProtection(result, undo_protection_unavailable);
  // Hand back what was actually produced, not just what was asked for.
  if (json published = publishedMarkerSummary(ctx, *topics); !published.is_null()) {
    result.update(published);
  }
  return ToolResult::success(result.dump());
}

}  // namespace

ToolResult createDerivedSeries(const json& args, ToolContext& ctx) {
  if (!ctx.dp.valid()) {
    return ToolResult::failure("the host did not expose pj.data_processors.v1 (cannot create series)");
  }
  if (!args.contains("name") || !args["name"].is_string() || args["name"].get<std::string>().empty()) {
    return ToolResult::failure("create_derived_series requires a non-empty string 'name'");
  }
  if (!args.contains("inputs") || !args["inputs"].is_array() || args["inputs"].empty()) {
    return ToolResult::failure("create_derived_series requires a non-empty 'inputs' array of topic/field paths");
  }
  const bool has_expr = args.contains("expression") && args["expression"].is_string();
  const bool has_body = args.contains("body") && args["body"].is_string();
  if (!has_expr && !has_body) {
    return ToolResult::failure(
        "create_derived_series requires 'expression' (a stateless Luau expression over value/v1../time, "
        "e.g. 'value * 2') OR 'body' (full Luau statements ending in return, with optional 'global' state)");
  }
  const std::string name = args["name"].get<std::string>();
  // One node can produce SEVERAL series. The host has always accepted a span of
  // outputs and our wrapper already forwards every value the body returns
  // (MULTRET), so the single-output limit was ours alone — it forced three
  // separate nodes, each recomputing the same intermediate, for something like
  // roll/pitch/yaw out of one quaternion. Defaults to [name], so nothing that
  // worked before changes.
  std::vector<std::string> outputs;
  if (args.contains("outputs") && args["outputs"].is_array()) {
    for (const auto& o : args["outputs"]) {
      if (o.is_string() && !o.get<std::string>().empty()) {
        outputs.push_back(o.get<std::string>());
      }
    }
  }
  if (outputs.empty()) {
    outputs.push_back(name);
  }
  std::vector<std::string> inputs;
  for (const auto& in : args["inputs"]) {
    if (in.is_string()) {
      inputs.push_back(canonicalSeriesPath(in.get<std::string>()));
    }
  }
  if (inputs.empty()) {
    return ToolResult::failure("'inputs' contained no string paths");
  }
  // Resolve every input against the catalog before handing it to the host.
  // Two reasons: an abbreviated path becomes the real one here (no correction
  // round-trip), and a path that names nothing fails loudly instead of
  // installing a transform whose input never matches — which produces an empty
  // curve and looks like it worked.
  // Installing over a name that is already taken is not a judgement call the
  // model gets to make — it silently replaces or shadows something the user has,
  // and neither outcome is what "create" was asked to do. The description used to
  // tell the model to call list_created "before creating something that may
  // already exist"; checking here costs nothing and does not depend on it
  // remembering to.
  if (auto existing = ctx.dp.list()) {
    for (const auto& id : *existing) {
      if (id == name || std::find(outputs.begin(), outputs.end(), id) != outputs.end()) {
        return ToolResult::failure(
            "'" + id +
            "' already exists — you created it earlier in the session. Remove it first with "
            "remove_derived_series, or choose another name.");
      }
    }
  }

  JoinForecast forecast;
  // How long the output will be when there is no join to shorten it. Taken here,
  // where the input is already resolved, because the catalog does not outlive
  // this block. rowCount() reads the Arrow header — it decodes no values.
  std::optional<std::size_t> single_input_points;
  if (auto catalog = ctx.host.catalogSnapshot()) {
    for (auto& in : inputs) {
      auto lookup = resolveSeriesPath(*catalog, in);
      if (!lookup.resolved) {
        return ToolResult::failure(seriesLookupError(in, lookup));
      }
      in = transformInputPath(*catalog, *lookup.resolved);
      if (inputs.size() == 1) {
        if (auto view = ctx.host.readSeries(lookup.resolved->handle); view) {
          single_input_points = view->rowCount();
        }
      }
    }
    // The same failure the resolution above guards against — an empty curve that
    // looks like it worked — reached the other way: inputs that all exist but
    // share no timestamps. Refuse to build it, and point at what does work,
    // because wanting to relate two recordings is legitimate even when a joined
    // series cannot express it.
    if (inputs.size() > 1) {
      forecast = forecastJoin(ctx, *catalog, inputs);
      if (forecast.checked && forecast.shared == 0) {
        std::string why = "these inputs share no timestamps, so the joined series would have 0 points: ";
        for (std::size_t i = 0; i < forecast.rates.size(); ++i) {
          why += (i == 0 ? "" : ", ") + forecast.rates[i];
        }
        why +=
            ". Multi-input transforms join on exact timestamp equality, so they only work on series recorded on the "
            "same clock. To relate series that are not (two runs, two devices), read_series each one and compare the "
            "statistics, or tell the user to plot them together.";
        return ToolResult::failure(why);
      }
    }
  }
  const std::size_t num_extra = inputs.size() - 1;
  // `body` (full statements, for stateful transforms like a derivative) wins;
  // otherwise wrap the stateless `expression` in a return. `global` runs once
  // per instance and its locals persist across samples (PJ3 global semantics).
  const std::string global = args.value("global", std::string{});
  const std::string body =
      has_body ? args["body"].get<std::string>() : "    return (" + args["expression"].get<std::string>() + ")";
  // Compile-check before installing, so a bad expression is a clean tool error.
  // The host's validator instantiates the class it expects to be named
  // "__validate__" (DataProcessorService::validateScript), so the validation
  // script MUST use that id — while the install script keeps the real name.
  const std::string validate_script = buildTransformScript("__validate__", "__validate__", global, body, num_extra);
  if (auto v = ctx.dp.validateScript("transform", kScriptLanguage, validate_script); !v) {
    return ToolResult::failure("invalid expression: " + v.error());
  }
  const std::string script = buildTransformScript(name, name, global, body, num_extra);

  std::vector<std::string_view> in_views(inputs.begin(), inputs.end());
  std::vector<std::string_view> out_views(outputs.begin(), outputs.end());
  bool undo_protection_unavailable = false;
  auto status = createHistoryExempt(
      ctx, name,
      [&](uint32_t flags) {
        return ctx.dp.createTransform(
            name, PJ::Span<const std::string_view>(in_views.data(), in_views.size()),
            PJ::Span<const std::string_view>(out_views.data(), out_views.size()), script, "{}", flags);
      },
      undo_protection_unavailable);
  if (!status) {
    return ToolResult::failure("create failed: " + status.error());
  }
  if (ctx.notify_data_changed) {
    ctx.notify_data_changed();
  }
  // Report the readable series path, not just the topic: the output lands as
  // "<name>/value", and models routinely read_series() what this returns.
  json series_paths = json::array();
  for (const auto& o : outputs) {
    series_paths.push_back(o + "/value");
  }
  json result = {
      {"created", name},
      {"series", outputs.size() == 1 ? json(outputs.front() + "/value") : series_paths},
      {"inputs", inputs}};
  annotateUndoProtection(result, undo_protection_unavailable);
  // Report what the call produced, not what to do about it. This used to carry a
  // "verify_with: read_series on ..." string, and models took the hint: across
  // the creation scenarios it cost 79 extra read_series calls, each one a full
  // round trip re-sending the whole conversation, to learn a number we already
  // had here. Deciding whether a result warrants a second look is the model's
  // job — it knows what the user asked for and this code does not. Ours is to
  // hand it the fact, once, at no cost.
  if (forecast.checked) {
    // Multi-input: the join is what determines the length, and losing most of
    // the rows to it ("of 20000 samples, 340 line up") is the difference between
    // a usable series and a handful of stray points.
    result["points"] = forecast.shared;
    if (forecast.shared < forecast.smallest) {
      result["shortest_input_points"] = forecast.smallest;
    }
  } else if (single_input_points) {
    // Single input: nothing joins, so the output is as long as the input.
    result["points"] = *single_input_points;
  }
  return ToolResult::success(result.dump());
}

// Run a Luau computation over series and hand back numbers, without leaving
// anything for the user to see: an EPHEMERAL transform is created, read once,
// and removed before returning — the host hides ephemeral outputs from its
// own catalog, but the plugin-facing ABI catalog snapshot still enumerates
// them, which is what makes the read-back possible at all.
ToolResult evaluateSeries(const json& args, ToolContext& ctx) {
  if (!ctx.dp.valid()) {
    return ToolResult::failure("the host did not expose pj.data_processors.v1 (cannot evaluate)");
  }
  if (!args.contains("inputs") || !args["inputs"].is_array() || args["inputs"].empty()) {
    return ToolResult::failure("evaluate requires a non-empty 'inputs' array of topic/field paths");
  }
  const bool has_expr = args.contains("expression") && args["expression"].is_string();
  const bool has_body = args.contains("body") && args["body"].is_string();
  if (!has_expr && !has_body) {
    return ToolResult::failure(
        "evaluate requires 'expression' (a stateless Luau expression over value/v1../time, e.g. "
        "'value - v1') OR 'body' (full Luau statements ending in return, with optional 'global' state)");
  }
  std::vector<std::string> inputs;
  for (const auto& in : args["inputs"]) {
    if (in.is_string()) {
      inputs.push_back(canonicalSeriesPath(in.get<std::string>()));
    }
  }
  if (inputs.empty()) {
    return ToolResult::failure("'inputs' contained no string paths");
  }

  // The OBJECT path: selected when any input is an object topic (per the v2
  // catalog), or when the model asked to evaluate at a specific time
  // ('at_s'/'window') rather than over the whole series. This is a
  // different host surface (kind="on_demand" via submitEvaluation, not a
  // per-sample transform read back through the catalog), so it is routed
  // out to its own function before anything below — which stays exactly
  // the scalar-path code that shipped before objects existed.
  {
    bool any_object_input = false;
    if (auto v2 = ctx.host.catalogSnapshotV2()) {
      for (const auto& in : inputs) {
        if (resolveObjectTopic(*v2, in).resolved) {
          any_object_input = true;
          break;
        }
      }
    }
    const bool has_time_selector =
        (args.contains("at_s") && !args["at_s"].is_null()) || (args.contains("window") && !args["window"].is_null());
    if (any_object_input || has_time_selector) {
      return evaluateObjectPath(args, ctx, inputs);
    }
  }

  // Same resolution and join-forecast rules as create_derived_series, and for
  // the same reasons: an unresolved or ambiguous input would install (briefly)
  // a transform that computes nothing meaningful, and inputs that share no
  // timestamps join to zero points either way.
  JoinForecast forecast;
  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }
  const TopicDatasetMap topic_dataset = datasetByTopicIndex(*catalog);
  for (auto& in : inputs) {
    auto lookup = resolveSeriesPath(*catalog, in, topic_dataset);
    if (!lookup.resolved) {
      return ToolResult::failure(seriesLookupError(in, lookup));
    }
    in = transformInputPath(*catalog, *lookup.resolved);
  }
  if (inputs.size() > 1) {
    forecast = forecastJoin(ctx, *catalog, inputs);
    if (forecast.checked && forecast.shared == 0) {
      std::string why = "these inputs share no timestamps, so the computed series would have 0 points: ";
      for (std::size_t i = 0; i < forecast.rates.size(); ++i) {
        why += (i == 0 ? "" : ", ") + forecast.rates[i];
      }
      why +=
          ". evaluate joins multiple inputs on exact timestamp equality, so it only works on series recorded on "
          "the same clock. To relate series that are not (two runs, two devices), read_series each one and "
          "compare the statistics.";
      return ToolResult::failure(why);
    }
  }

  const std::size_t num_extra = inputs.size() - 1;
  const std::string global = args.value("global", std::string{});
  const std::string body =
      has_body ? args["body"].get<std::string>() : "    return (" + args["expression"].get<std::string>() + ")";

  const unsigned call_id = ++g_evaluate_counter;
  const std::string id = "__evaluate_" + std::to_string(call_id);
  // Same "__validate__" id trick create_derived_series uses: the host's
  // validator instantiates the class under that fixed name.
  const std::string validate_script = buildTransformScript("__validate__", "__validate__", global, body, num_extra);
  if (auto v = ctx.dp.validateScript("transform", kScriptLanguage, validate_script); !v) {
    return ToolResult::failure("invalid expression: " + v.error());
  }
  const std::string script = buildTransformScript(id, id, global, body, num_extra);

  const std::string output_name = id + "/value";
  std::vector<std::string_view> in_views(inputs.begin(), inputs.end());
  std::array<std::string_view, 1> out_views{output_name};
  auto created = ctx.dp.create(
      id, "transform", kScriptLanguage, PJ::Span<const std::string_view>(in_views.data(), in_views.size()),
      PJ::Span<const std::string_view>(out_views.data(), out_views.size()), script, "{}",
      PJ_DATA_PROCESSOR_FLAG_EPHEMERAL);
  if (!created) {
    return ToolResult::failure("evaluate failed: " + created.error());
  }
  // The node is never shown to the user and must not outlive this call on any
  // path — a successful read, a failed one, or an exception unwinding out of
  // this function. No ctx.notify_data_changed() anywhere here either: nothing
  // changed that the host's GUI should redraw for.
  struct RemoveGuard {
    ToolContext& ctx;
    std::string id;
    ~RemoveGuard() {
      auto status = ctx.dp.remove(id);
      (void)status;  // best-effort teardown of a node the user never saw; nothing to react to here
    }
  } remove_guard{ctx, id};

  const std::string resolved_output = created->empty() ? output_name : created->front();

  // The catalog snapshot taken before create() does not contain the new
  // sink; a fresh one is required to resolve and read it back.
  auto fresh_catalog = ctx.host.catalogSnapshot();
  if (!fresh_catalog) {
    return ToolResult::failure("catalog unavailable after create: " + fresh_catalog.error());
  }
  SeriesRead r = readOne(*fresh_catalog, ctx, resolved_output, datasetByTopicIndex(*fresh_catalog));
  if (!r.ok) {
    return ToolResult::failure(r.error);
  }
  const SeriesStats stats = computeStats(r.ts, r.vals);
  json result = {{"evaluated", r.path}, {"stats", statsWithDisplayStart(stats, ctx, r.topic, r.flat)}};

  if (args.contains("buckets")) {
    if (!args["buckets"].is_number_integer()) {
      return ToolResult::failure("'buckets' must be an integer between 1 and 500");
    }
    const std::size_t max_points = static_cast<std::size_t>(std::clamp(args["buckets"].get<int>(), 1, 500));
    result = withCoarsenedBuckets(std::move(result), r.ts, r.vals, max_points);
  }
  return ToolResult::success(result.dump());
}

// Install a persisted kind="on_demand" node (an object-producing computation
// the host re-evaluates on request, never eagerly) and, on success, evaluate
// it once so the model sees a finding instead of taking "created" on faith.
// Shares evaluateObjectPath's input resolution, time conversion and report
// rendering; the difference is createV2 (persisted, HISTORY_EXEMPT like
// create_derived_series) in place of an EPHEMERAL submitEvaluation.
ToolResult createDerivedObject(const json& args, ToolContext& ctx) {
  if (!ctx.dp.valid()) {
    return ToolResult::failure("the host did not expose pj.data_processors.v1 (cannot create)");
  }
  if (!args.contains("name") || !args["name"].is_string() || args["name"].get<std::string>().empty()) {
    return ToolResult::failure("create_derived_object requires a non-empty string 'name'");
  }
  const std::string name = args["name"].get<std::string>();
  if (!args.contains("inputs") || !args["inputs"].is_array() || args["inputs"].empty()) {
    return ToolResult::failure(
        "create_derived_object requires a non-empty 'inputs' array of topic/field or object-topic paths");
  }
  if (!args.contains("body") || !args["body"].is_string() || args["body"].get<std::string>().empty()) {
    return ToolResult::failure(
        "create_derived_object requires 'body': a Luau chunk body reading inputs[\"<topic>\"] and returning a "
        "table of the declared outputs");
  }
  if (!args.contains("outputs") || !args["outputs"].is_array() || args["outputs"].empty()) {
    return ToolResult::failure(
        "create_derived_object requires 'outputs': an array of \"name:type\" (type is 'number', 'string', or a "
        "builtin object type like 'kPointCloud')");
  }
  const ParsedOutputs parsed_outputs = parseTypedOutputs(args["outputs"]);
  if (!parsed_outputs.error.empty()) {
    return ToolResult::failure(parsed_outputs.error);
  }
  std::vector<std::string> raw_inputs;
  for (const auto& in : args["inputs"]) {
    if (in.is_string()) {
      raw_inputs.push_back(canonicalSeriesPath(in.get<std::string>()));
    }
  }
  if (raw_inputs.empty()) {
    return ToolResult::failure("'inputs' contained no string paths");
  }

  // Same "already taken" guard create_derived_series applies, for the same
  // reason: installing over a name the user already has is not a judgement
  // call this code gets to make.
  if (auto existing = ctx.dp.list()) {
    for (const auto& id : *existing) {
      if (id == name) {
        return ToolResult::failure(
            "'" + name +
            "' already exists — you created it earlier in the session. Remove it first with "
            "remove_derived_series, or choose another name.");
      }
    }
  }

  auto v2 = ctx.host.catalogSnapshotV2();
  if (!v2) {
    return ToolResult::failure(
        "create_derived_object requires catalog snapshot v2, which this host does not expose: " + v2.error());
  }
  const ResolvedEvalInputs resolved = resolveEvalInputs(ctx.host, *v2, raw_inputs);
  if (!resolved.error.empty()) {
    return ToolResult::failure(resolved.error);
  }
  if (!resolved.anchor_source) {
    return ToolResult::failure(
        "create_derived_object needs at least one resolvable input to anchor its dataset's time conversion");
  }

  // A pin is a FINDING: evaluated once, at a fixed instant, and kept —
  // unlike a bare on_demand install (no pin), which the host re-evaluates
  // wherever a later consumer asks. Converted the same way evaluate's at_s
  // is.
  std::optional<std::int64_t> pin_ns;
  if (args.contains("pin_at_s") && !args["pin_at_s"].is_null()) {
    if (!args["pin_at_s"].is_number()) {
      return ToolResult::failure("'pin_at_s' must be a number (display seconds)");
    }
    pin_ns = toRawNs(ctx.playback, *resolved.anchor_source, args["pin_at_s"].get<double>());
    if (!pin_ns) {
      return ToolResult::failure(
          "cannot convert 'pin_at_s' to raw time: the host did not expose pj.playback.v1 or per-source time "
          "conversion");
    }
  }

  const std::string body = args["body"].get<std::string>();
  const std::string script = buildResolvedOnDemandChunk(body, resolved);
  if (auto v = ctx.dp.validateScript("on_demand", kScriptLanguage, script); !v) {
    return ToolResult::failure("invalid script: " + v.error());
  }

  std::vector<std::string> input_names;
  for (const auto& ri : resolved.inputs) {
    input_names.push_back(ri.request_path);
  }
  const std::string params_json = args.contains("params") && args["params"].is_object() ? args["params"].dump() : "{}";

  bool undo_protection_unavailable = false;
  auto created = createHistoryExempt(
      ctx, name,
      [&](uint32_t flags) {
        PJ::sdk::DataProcessorRequest request;
        request.id = name;
        request.kind = "on_demand";
        request.language = kScriptLanguage;
        request.script = script;
        request.params_json = params_json;
        request.inputs = input_names;
        request.outputs = parsed_outputs.outputs;
        request.flags = flags;
        request.instant_ns = pin_ns;
        return ctx.dp.createV2(request);
      },
      undo_protection_unavailable);
  if (!created) {
    return ToolResult::failure("create failed: " + created.error());
  }
  if (ctx.notify_data_changed) {
    ctx.notify_data_changed();
  }

  json result = {{"created", name}, {"out_topics", *created}};
  if (pin_ns) {
    result["pinned_at_s"] = args["pin_at_s"].get<double>();  // a frozen finding, not a live object
  } else {
    result["live"] = true;  // recomputed wherever the playhead is
  }
  annotateUndoProtection(result, undo_protection_unavailable);

  // Evaluate the installed node once, at the pin (if given) or the current
  // playhead, so the model sees what it made instead of taking "created" on
  // faith — the same principle "Closing the loop on what it creates"
  // documents for markers and derived series. Best-effort: the node IS
  // installed either way, so a failed read-back degrades the response
  // rather than the tool call.
  std::optional<std::int64_t> eval_ns = pin_ns;
  if (!eval_ns && ctx.playback.valid()) {
    if (auto state = ctx.playback.state()) {
      eval_ns = toRawNs(ctx.playback, *resolved.anchor_source, state->current_time_s);
    }
  }
  if (eval_ns) {
    PJ::sdk::DataProcessorRequest eval_request;
    eval_request.id = name;
    eval_request.kind = "on_demand";
    // An installed node is evaluated by id only: its stored recipe is authoritative (SDK contract).
    eval_request.instant_ns = eval_ns;

    return submitAndPoll(
        ctx, eval_request, 1000, 1,
        [result = std::move(result), name, source = *resolved.anchor_source](
            ToolContext& current, const SubmitAndPollResult& polled) mutable {
          if (polled.ok) {
            const RenderedBundles rendered = renderBundles(current, source, polled.report, false);
            if (!rendered.bundles.empty()) {
              result["bundle"] = rendered.bundles.front();
            }
          } else {
            // A node in error is a script that compiles but cannot run: remove it and fail so the model fixes it.
            if (auto detail = installedNodeErrorDetail(current, name)) {
              (void)current.dp.remove(name);
              if (current.notify_data_changed) {
                current.notify_data_changed();
              }
              return ToolResult::failure(
                  "create_derived_object: '" + name + "' failed when evaluated: " + *detail +
                  ". Nothing was kept; fix the script and call create_derived_object again.");
            }
            result["bundle_unavailable"] = polled.error;
          }
          return ToolResult::success(result.dump());
        });
  }
  return ToolResult::success(result.dump());
}

ToolResult createMarkers(const json& args, ToolContext& ctx) {
  if (!ctx.dp.valid()) {
    return ToolResult::failure("the host did not expose pj.data_processors.v1 (cannot create markers)");
  }

  // Two mutually exclusive forms (mirrors create_derived_series's
  // expression-XOR-body): a raw Luau `rule` over declared `inputs` — the model
  // writes the whole marker script against the vocabulary documented in the
  // tool schema — or the simple series/comparison/threshold template kept for
  // small models. On PJ4 main the host rejects kind="markers" and either form
  // returns that error to the model by design (graceful degrade).
  const bool has_rule = args.contains("rule") && args["rule"].is_string();
  const bool has_template = args.contains("series") || args.contains("threshold");
  if (has_rule && has_template) {
    return ToolResult::failure(
        "create_markers takes EITHER a raw 'rule' (with 'inputs') OR the series/comparison/threshold "
        "template — not both");
  }
  if (has_rule) {
    return createMarkersFromRule(args, ctx);
  }

  if (!args.contains("series") || !args["series"].is_string()) {
    return ToolResult::failure(
        "create_markers requires a string 'series' (a topic/field path), or a raw 'rule' with 'inputs'");
  }
  std::string series = canonicalSeriesPath(args["series"].get<std::string>());
  // The rule embeds this name and the host resolves it by itself, so it must
  // be the resolved HOST form — the raw argument used to go straight through,
  // which is how a bare name that exists in two datasets landed on whichever
  // one the host tried first.
  std::string display_series = series;
  if (auto catalog = ctx.host.catalogSnapshot()) {
    auto lookup = resolveSeriesPath(*catalog, series);
    if (!lookup.resolved) {
      return ToolResult::failure(seriesLookupError(series, lookup));
    }
    if (auto blocked = hostCreateBlocker(*catalog, *lookup.resolved)) {
      return ToolResult::failure(*blocked);
    }
    display_series = lookup.resolved->path;
    series = lookup.resolved->host_path;
  }
  const std::string comparison = args.value("comparison", std::string(">"));
  if (!isComparison(comparison)) {
    return ToolResult::failure(kComparisonError);
  }
  if (!args.contains("threshold") || !args["threshold"].is_number()) {
    return ToolResult::failure("create_markers requires a numeric 'threshold'");
  }
  const double threshold = args["threshold"].get<double>();
  // Locale-independent, round-trippable literal (std::to_string is locale
  // sensitive — a comma decimal separator breaks the generated Luau — and
  // truncates to 6 decimals, corrupting small thresholds).
  const std::string threshold_lit = json(threshold).dump();
  const std::string label = luaStringEscape(args.value("label", "exceeds " + threshold_lit));
  const std::string style = args.value("style", std::string("region"));
  if (style != "region" && style != "line") {
    return ToolResult::failure(
        "'style' must be \"region\" (one shaded band per exceedance stretch) or "
        "\"line\" (one vertical line per matching sample)");
  }

  const std::string opts = "{label=\"" + label + "\", severity=\"warning\"}";
  std::string rule = "local s = series(\"" + luaStringEscape(series) + "\")\n";
  if (style == "region") {
    // One region per contiguous stretch of matching samples: open on the first
    // matching sample, close on the first non-matching one (or at series end).
    rule += "local open = false\n";
    rule += "local last_t = nil\n";
    rule += "for i = 0, s:size() - 1 do\n";
    rule += "  local p = s:at(i)\n";
    rule += "  last_t = p.t\n";
    rule += "  local hit = p.v " + comparison + " " + threshold_lit + "\n";
    rule += "  if hit and not open then\n";
    rule += "    startMarker(p.t)\n";
    rule += "    open = true\n";
    rule += "  elseif open and not hit then\n";
    rule += "    closeMarker(p.t, " + opts + ")\n";
    rule += "    open = false\n";
    rule += "  end\n";
    rule += "end\n";
    rule += "if open and last_t then\n";
    rule += "  closeMarker(last_t, " + opts + ")\n";
    rule += "end\n";
  } else {
    rule += "for i = 0, s:size() - 1 do\n";
    rule += "  local p = s:at(i)\n";
    rule += "  if p.v " + comparison + " " + threshold_lit + " then\n";
    rule += "    createVerticalMarker(p.t, " + opts + ")\n";
    rule += "  end\n";
    rule += "end\n";
  }

  // The output marker topic addresses where the set renders: a persistent
  // kind="markers" node requires one (empty is preview-only). Use the input
  // series' own field path (markerSeriesKey semantics) so the markers appear
  // on every plot showing that series.
  std::array<std::string_view, 1> inputs{series};
  bool undo_protection_unavailable = false;
  auto topics = createHistoryExempt(
      ctx, "assistant_markers",
      [&](uint32_t flags) {
        return ctx.dp.createMarkers(
            "assistant_markers", PJ::Span<const std::string_view>(inputs.data(), inputs.size()),
            /*output_marker_topic=*/series, rule, "{}", flags);
      },
      undo_protection_unavailable);
  if (!topics) {
    return ToolResult::failure("create_markers failed: " + topics.error());
  }
  if (ctx.notify_data_changed) {
    ctx.notify_data_changed();
  }
  json result = {{"created_markers_on", display_series}, {"style", style}, {"rule", label}};
  annotateUndoProtection(result, undo_protection_unavailable);
  if (json published = publishedMarkerSummary(ctx, *topics); !published.is_null()) {
    result.update(published);
  }
  return ToolResult::success(result.dump());
}

ToolResult removeMarkers(const json& /*args*/, ToolContext& ctx) {
  if (!ctx.dp.valid()) {
    return ToolResult::failure("the host did not expose pj.data_processors.v1 (cannot remove markers)");
  }
  auto status = ctx.dp.remove("assistant_markers");
  if (!status) {
    return ToolResult::failure("remove_markers failed: " + status.error());
  }
  if (ctx.notify_data_changed) {
    ctx.notify_data_changed();
  }
  return ToolResult::success(json({{"removed", "assistant_markers"}}).dump());
}

// What this assistant has installed in the session so far.
//
// dp.list() enumerates only THIS plugin's nodes, so both this and the removal
// below are bounded by construction: neither can see or touch loaded data, or
// anything another plugin created.
ToolResult listCreated(const json& /*args*/, ToolContext& ctx) {
  if (!ctx.dp.valid()) {
    return ToolResult::failure("the host did not expose pj.data_processors.v1");
  }
  auto ids = ctx.dp.list();
  if (!ids) {
    return ToolResult::failure("list failed: " + ids.error());
  }
  json arr = json::array();
  for (const auto& id : *ids) {
    arr.push_back(id);
  }
  return ToolResult::success(json({{"created", arr}, {"count", arr.size()}}).dump());
}

ToolResult removeDerivedSeries(const json& args, ToolContext& ctx) {
  if (!ctx.dp.valid()) {
    return ToolResult::failure("the host did not expose pj.data_processors.v1");
  }
  if (!args.contains("name") || !args["name"].is_string() || args["name"].get<std::string>().empty()) {
    return ToolResult::failure("remove_derived_series requires a non-empty string 'name'");
  }
  const std::string name = args["name"].get<std::string>();
  // Check membership first so an unknown name gets a useful answer listing what
  // does exist, rather than whatever the host says about a handle it never had.
  auto ids = ctx.dp.list();
  if (ids && std::find(ids->begin(), ids->end(), name) == ids->end()) {
    std::string known;
    for (const auto& id : *ids) {
      known += (known.empty() ? "" : ", ") + id;
    }
    return ToolResult::failure(
        "you did not create '" + name + "'" +
        (known.empty() ? "; you have created nothing in this session" : "; you have created: " + known) +
        ". Only series created here can be removed — loaded data cannot be touched.");
  }
  auto status = ctx.dp.remove(name);
  if (!status) {
    return ToolResult::failure("remove_derived_series failed: " + status.error());
  }
  if (ctx.notify_data_changed) {
    ctx.notify_data_changed();
  }
  return ToolResult::success(json({{"removed", name}}).dump());
}

}  // namespace PJ::agent_tools::detail
