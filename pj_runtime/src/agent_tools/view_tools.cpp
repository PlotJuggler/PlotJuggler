// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Agent tool executors. Each is a pure function of (args, host views) and
// returns a ToolResult whose `content` the model reads. Every failure is
// returned as data (ok=false), never thrown.
//
// This file: list_topics, describe_topic, playback, plot_tab, scene_view, report_status.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <pj_base/builtin/builtin_object.hpp>
#include <pj_base/builtin/field_table.hpp>
#include <pj_base/builtin/field_table_registry.hpp>
#include <pj_base/builtin/plot_markers.hpp>
#include <pj_base/sdk/object_topic_metadata.hpp>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "pj_runtime/agent_tools/object_ops_catalog.hpp"
#include "tools_internal.hpp"

namespace PJ::agent_tools::detail {
namespace {

// An object topic's time range, in the best axis available: display seconds
// when a playback view is bound and can convert both ends (matching every
// other display-time report in this file), else the raw dataset-domain
// seconds the host reported, flagged so the model never mistakes one axis for
// the other.
struct ObjectTimeRange {
  double a = 0.0;
  double b = 0.0;
  bool raw = false;
};

// Wire name of a primitive type ("float64", ...); "unspecified" for anything unknown.
const char* primitiveTypeName(PJ::PrimitiveType t) {
  switch (t) {
    case PJ::PrimitiveType::kFloat32:
      return "float32";
    case PJ::PrimitiveType::kFloat64:
      return "float64";
    case PJ::PrimitiveType::kInt8:
      return "int8";
    case PJ::PrimitiveType::kInt16:
      return "int16";
    case PJ::PrimitiveType::kInt32:
      return "int32";
    case PJ::PrimitiveType::kInt64:
      return "int64";
    case PJ::PrimitiveType::kUint8:
      return "uint8";
    case PJ::PrimitiveType::kUint16:
      return "uint16";
    case PJ::PrimitiveType::kUint32:
      return "uint32";
    case PJ::PrimitiveType::kUint64:
      return "uint64";
    case PJ::PrimitiveType::kBool:
      return "bool";
    case PJ::PrimitiveType::kString:
      return "string";
    default:
      return "unspecified";
  }
}

// Converts [t_min_ns, t_max_ns] of `source` to display seconds through `playback`;
// falls back to raw seconds (raw=true) when the view is unbound or a conversion fails.
ObjectTimeRange objectTimeRange(
    const PJ::sdk::PlaybackHostView& playback, PJ_data_source_handle_t source, std::int64_t t_min_ns,
    std::int64_t t_max_ns) {
  if (playback.valid()) {
    const auto display_a = playback.toDisplayTimeForSource(source, t_min_ns);
    const auto display_b = playback.toDisplayTimeForSource(source, t_max_ns);
    if (display_a && display_b) {
      return {*display_a, *display_b, false};
    }
  }
  return {static_cast<double>(t_min_ns) * 1e-9, static_cast<double>(t_max_ns) * 1e-9, true};
}

// --- object topics (catalog snapshot v2) ------------------------------------
//
// Object topics (point clouds, scene entities, images…) live beside the
// scalar catalog in PJ_catalog_snapshot_v2_t. Marker sets are object topics
// too, but they are drawn, not read — kMarkerObjectTopicPrefix filters them
// out of everything a model sees (list_topics, describe_topic,
// report_status), the same way pj_scene overlays already treat them.

// Whether an object topic's metadata document names it as derived (produced
// by an on-demand script, not ingested from a file/stream). Tolerant of a
// missing or malformed document -- metadata is host-provided, not modeled
// data, but it still crosses the plugin ABI as an untrusted string.
bool metadataHasKey(std::string_view metadata_json, std::string_view key) {
  const json parsed = json::parse(std::string(metadata_json), nullptr, /*allow_exceptions=*/false);
  return !parsed.is_discarded() && parsed.is_object() && parsed.contains(std::string(key));
}

const char* fieldKindName(PJ::sdk::FieldKind kind) {
  using PJ::sdk::FieldKind;
  switch (kind) {
    case FieldKind::kNumber:
      return "number";
    case FieldKind::kOptionalNumber:
      return "number|null";
    case FieldKind::kBool:
      return "bool";
    case FieldKind::kInt64:
      return "int64";
    case FieldKind::kString:
      return "string";
    case FieldKind::kEnum:
      return "enum";
    case FieldKind::kStruct:
      return "struct";
    case FieldKind::kList:
      return "list";
    case FieldKind::kBuffer:
      return "buffer";
  }
  return "unspecified";
}

// A field table's own field list is depth 1; a nested struct or a list's
// struct element goes one level deeper. Capped at kMaxFieldDepth so a
// describe_topic call on a deeply nested type stays bounded -- a list shows
// its element shape once at the next depth rather than per element (there is
// only one shape, the field table is per-TYPE not per-instance), and a buffer
// field (raw record bytes, e.g. PointCloud::data) carries nothing past its
// name/kind: the bytes never reach the model, only the native operations that
// summarize them (see object_ops_catalog.hpp).
constexpr int kMaxFieldDepth = 3;

json fieldTableToJson(const PJ::sdk::FieldTableView& table, int depth) {
  json arr = json::array();
  for (const auto& f : table.fields) {
    json entry = {{"name", std::string(f.name)}, {"kind", fieldKindName(f.kind)}};
    const bool nestable = f.kind == PJ::sdk::FieldKind::kStruct || f.kind == PJ::sdk::FieldKind::kList;
    if (nestable && f.nested != nullptr && depth < kMaxFieldDepth) {
      entry["fields"] = fieldTableToJson(*f.nested, depth + 1);
    }
    arr.push_back(std::move(entry));
  }
  return arr;
}

// One list_topics/describe_topic entry for an object topic, the shape shared
// by both tools: {"topic","kind":"object","type","entries","t_range_s"
// [,"time_basis":"raw"][,"dataset"]}. Callers add whatever is specific to
// their tool (list_topics adds "derived"; describe_topic adds "fields" and
// "operations").
json objectTopicCommonJson(
    ToolContext& ctx, std::span<const PJ_data_source_info_t> sources, const PJ_object_topic_info_t& obj) {
  const ObjectTimeRange range = objectTimeRange(ctx.playback, obj.source, obj.time_min_ns, obj.time_max_ns);
  json out = {
      {"topic", std::string(PJ::sdk::toStringView(obj.name))},
      {"kind", "object"},
      {"type", std::string(PJ::sdk::toStringView(obj.builtin_object_type))},
      {"entries", obj.entry_count},
      {"t_range_s", json::array({range.a, range.b})},
  };
  if (range.raw) {
    out["time_basis"] = "raw";
  }
  const std::string dataset = objectTopicDatasetName(sources, obj.source);
  if (!dataset.empty()) {
    out["dataset"] = dataset;
  }
  return out;
}

// --- executors -------------------------------------------------------------

// Scalar topics matching filter/dataset_filter, capped at `limit`, in
// list_topics' shape -- {"topic","kind":"scalar","fields"[,"dataset"]} --
// appended to `out_topics`. `matched`/`shown` accumulate so a caller merging
// this with object topics (listTopics) gets one combined count of both kinds.
void listScalarTopics(
    std::span<const PJ_topic_info_t> topics, const std::map<std::uint32_t, std::string>& topic_dataset,
    const std::string& filter, const std::string& dataset_filter, int limit, json& out_topics, int& matched,
    int& shown) {
  for (std::uint32_t ti = 0; ti < topics.size(); ++ti) {
    const auto& topic = topics[ti];
    const std::string name(PJ::sdk::toStringView(topic.name));
    if (!filter.empty() && name.find(filter) == std::string::npos) {
      continue;
    }
    const auto ds = topic_dataset.find(ti);
    const std::string dataset = ds != topic_dataset.end() ? ds->second : std::string{};
    if (!dataset_filter.empty() && dataset.find(dataset_filter) == std::string::npos) {
      continue;
    }
    ++matched;
    if (shown < limit) {
      json entry = {{"topic", name}, {"kind", "scalar"}, {"fields", topic.field_count}};
      if (!dataset.empty()) {
        entry["dataset"] = dataset;
      }
      out_topics.push_back(std::move(entry));
      ++shown;
    }
  }
}

// Scalar-topic half of describe_topic, shared by the catalog-snapshot-v2 and
// legacy-v1 paths (identical span shapes either way). The topic resolves like a
// read_series topic path, and the field paths carry its dataset when several are
// loaded, so read_series takes them as they are.
ToolResult describeScalarTopic(
    std::span<const PJ_topic_info_t> topics, std::span<const PJ_field_info_t> fields,
    std::span<const PJ_data_source_info_t> sources, const std::string& want) {
  const TopicDatasetMap topic_dataset = datasetByTopicIndex(sources);
  const TopicLookup lookup = resolveTopicPath(topics, sources, want, topic_dataset);
  if (lookup.ambiguous) {
    SeriesLookup ambiguous;
    ambiguous.ambiguous = true;
    ambiguous.candidates = lookup.candidates;
    return ToolResult::failure(seriesLookupError(want, ambiguous));
  }
  if (!lookup.topic_index) {
    return ToolResult::failure("no topic named '" + want + "' (use list_topics)");
  }
  const auto& topic = topics[*lookup.topic_index];
  const std::string name(PJ::sdk::toStringView(topic.name));
  const auto dataset_it = topic_dataset.find(*lookup.topic_index);
  const std::string dataset = dataset_it == topic_dataset.end() ? std::string() : dataset_it->second;
  json field_arr = json::array();
  for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
    const std::size_t idx = topic.first_field + fi;
    if (idx >= fields.size()) {
      break;
    }
    const std::string leaf(PJ::sdk::toStringView(fields[idx].name));
    field_arr.push_back(
        {{"field", leaf},
         {"path", qualifyWithDataset(dataset, joinSeriesPath(name, leaf))},
         {"type", primitiveTypeName(PJ::sdk::fromAbiType(fields[idx].type))}});
  }
  json out = {{"topic", name}, {"kind", "scalar"}, {"fields", field_arr}};
  if (!dataset.empty()) {
    out["dataset"] = dataset;
  }
  return ToolResult::success(out.dump());
}

// Object-topic half of describe_topic: the field table walked recursively
// (fieldTableToJson) plus the static operations a script may call on it
// (object_ops_catalog.hpp). A type with no field table (kNone, buffer-only,
// or not yet described) or an unrecognized builtin_object_type string
// answers with empty "fields"/"operations" rather than failing -- the topic
// itself is real, only its detail is unavailable.
ToolResult describeObjectTopic(
    ToolContext& ctx, std::span<const PJ_data_source_info_t> sources, const PJ_object_topic_info_t& obj) {
  json out = objectTopicCommonJson(ctx, sources, obj);
  json field_arr = json::array();
  json op_arr = json::array();
  const std::string type_name = out["type"].get<std::string>();
  if (const auto type = PJ::sdk::parseBuiltinObjectType(type_name)) {
    if (const PJ::sdk::FieldTableView* table = PJ::sdk::describe(*type)) {
      field_arr = fieldTableToJson(*table, /*depth=*/1);
    }
    for (const ObjectOperation& op : objectOperationsFor(*type)) {
      op_arr.push_back({{"name", std::string(op.name)}, {"doc", std::string(op.doc)}});
    }
  }
  out["fields"] = std::move(field_arr);
  out["operations"] = std::move(op_arr);
  return ToolResult::success(out.dump());
}

// --- playback / viewport executors ------------------------------------------

constexpr const char* kNoPlayback = "the host did not expose pj.playback.v1 (cannot control playback)";

constexpr const char* kNoViewport = "the host did not expose pj.viewport.v1 (cannot zoom plots)";

json stateToJson(const PJ::sdk::PlaybackState& state) {
  return {
      {"playing", state.is_playing},
      {"current_time_s", state.current_time_s},
      {"range", {{"min_s", state.range_min_s}, {"max_s", state.range_max_s}}},
      {"rate", state.playback_rate}};
}

// Full transport snapshot, echoed by every playback mutation so the model sees
// the effect (e.g. a clamped seek) without a follow-up call. Best-effort: a
// failed state read after a SUCCESSFUL mutation self-describes instead of
// failing the tool. All times are display-axis seconds — the numbers on the
// plot X axes.
json playbackStateJson(ToolContext& ctx) {
  auto state = ctx.playback.state();
  if (!state) {
    return {{"state_unavailable", state.error()}};
  }
  return stateToJson(*state);
}

// Shared shell of the playback mutation tools: valid() gate, the host call,
// error wrapping, and the state echo. Arg validation stays per-tool, above.
template <class Op>
ToolResult runPlaybackOp(ToolContext& ctx, const char* verb, Op&& op) {
  if (!ctx.playback.valid()) {
    return ToolResult::failure(kNoPlayback);
  }
  if (auto status = op(); !status) {
    return ToolResult::failure(std::string(verb) + " failed: " + status.error());
  }
  return ToolResult::success(playbackStateJson(ctx).dump());
}

// --- the assistant's own plot tabs -------------------------------------------

constexpr const char* kNoPlotTabs = "the host did not expose pj.plot_tabs.v1 (cannot compose plot tabs)";

// One tab's config, fetched and parsed once. pj.plot_tabs.v1 lists plot tabs AND
// scene tabs together; a scene tab's config carries a "kind" key and a plot tab's
// does not, which is how plot_tab and scene_view each keep to their own.
struct TabConfig {
  std::string error;  // the host's refusal; empty when the config was fetched
  json parsed;        // null when unreadable

  [[nodiscard]] bool isScene() const {
    return parsed.is_object() && parsed.contains("kind");
  }
};

TabConfig fetchTabConfig(ToolContext& ctx, const std::string& id) {
  TabConfig out;
  auto config = ctx.plot_tabs.configOf(id);
  if (!config) {
    out.error = config.error();
    return out;
  }
  out.parsed = json::parse(*config, nullptr, /*allow_exceptions=*/false);
  return out;
}

// Ids this assistant owns of one flavour: scene tabs (3d/2d) or plot tabs. An
// unreadable list degrades to no ids rather than replacing the real failure with a
// secondary one.
std::vector<std::string> ownedIdsOfKind(ToolContext& ctx, bool scene) {
  std::vector<std::string> out;
  auto ids = ctx.plot_tabs.list();
  if (!ids) {
    return out;
  }
  for (const std::string& id : *ids) {
    if (fetchTabConfig(ctx, id).isScene() == scene) {
      out.push_back(id);
    }
  }
  return out;
}

std::string joinOrNone(const std::vector<std::string>& ids) {
  if (ids.empty()) {
    return "none yet";
  }
  std::string out;
  for (const std::string& id : ids) {
    out += (out.empty() ? "" : ", ") + id;
  }
  return out;
}

std::string ownedTabList(ToolContext& ctx) {
  return joinOrNone(ownedIdsOfKind(ctx, /*scene=*/false));
}

// The tab (`key` "tab") or scene view (`key` "view") as the HOST holds it, parsed back
// from tab_config. Every action answers with this rather than an echo of the request,
// so a curve or topic that did not land shows as absent instead of being reported as
// drawn or attached.
json readBackFrom(TabConfig config, const std::string& id, const char* key) {
  if (!config.error.empty()) {
    return {{key, id}, {"contents_unavailable", config.error}};
  }
  if (!config.parsed.is_object()) {
    return {{key, id}, {"contents_unavailable", std::string("the host returned no readable ") + key + " contents"}};
  }
  config.parsed[key] = id;
  return std::move(config.parsed);
}

json readBack(ToolContext& ctx, const std::string& id, const char* key) {
  return readBackFrom(fetchTabConfig(ctx, id), id, key);
}

json tabReadBack(ToolContext& ctx, const std::string& tab) {
  return readBack(ctx, tab, "tab");
}

// --- the assistant's own scene views -----------------------------------------

// Names of the scene tabs this assistant currently owns, mirroring
// ownedTabList's role in plotTabTool's errors.
std::string ownedViewList(ToolContext& ctx) {
  return joinOrNone(ownedIdsOfKind(ctx, /*scene=*/true));
}

json viewReadBack(ToolContext& ctx, const std::string& view) {
  return readBack(ctx, view, "view");
}

// One attach/detach outcome, resolved before the host is asked so a bad path
// never reaches attachTopic/detachTopic in the first place.
struct SceneTopicOutcome {
  std::string display_topic;  // qualified form shown back to the model
  std::string bare_topic;     // host_path, matched against the read-back below
  std::string dataset;
  bool called_ok = false;  // the host call itself succeeded (not yet verified as landed)
  std::string error;       // set on resolve failure or a host refusal
};

}  // namespace

ToolResult listTopics(const json& args, ToolContext& ctx) {
  const std::string filter = args.value("filter", std::string{});
  // Clamped, not just defaulted: this response is re-sent on every remaining
  // round-trip of the turn, so an unbounded list would be paid for repeatedly.
  // read_series bounds its payload the same way. The truncation note below
  // still tells the model how to narrow the search.
  const int limit = std::clamp(args.value("limit", 100), 1, 500);

  // Scoping to one dataset is what makes this usable with several loaded: the
  // same topic names repeat across runs, so a name filter alone returns both.
  const std::string dataset_filter = args.value("dataset", std::string{});

  json topics = json::array();
  int matched = 0;
  int shown = 0;

  auto v2 = ctx.host.catalogSnapshotV2();
  bool objects_listed = false;
  if (v2) {
    listScalarTopics(
        v2->topics(), datasetByTopicIndex(v2->dataSources()), filter, dataset_filter, limit, topics, matched, shown);
    for (const auto& obj : v2->objectTopics()) {
      const std::string name(PJ::sdk::toStringView(obj.name));
      if (isMarkerObjectTopic(name)) {
        continue;  // drawn, not read -- never surfaced to the model
      }
      if (!filter.empty() && name.find(filter) == std::string::npos) {
        continue;
      }
      const std::string dataset = objectTopicDatasetName(v2->dataSources(), obj.source);
      if (!dataset_filter.empty() && dataset.find(dataset_filter) == std::string::npos) {
        continue;
      }
      ++matched;
      if (shown < limit) {
        json entry = objectTopicCommonJson(ctx, v2->dataSources(), obj);
        entry["derived"] = metadataHasKey(PJ::sdk::toStringView(obj.metadata_json), PJ::sdk::kDerivedMetadataKey);
        topics.push_back(std::move(entry));
        ++shown;
      }
    }
    objects_listed = true;
  } else {
    auto catalog = ctx.host.catalogSnapshot();
    if (!catalog) {
      return ToolResult::failure("catalog unavailable: " + catalog.error());
    }
    listScalarTopics(
        catalog->topics(), datasetByTopicIndex(catalog->dataSources()), filter, dataset_filter, limit, topics, matched,
        shown);
  }

  json out = {{"count", matched}, {"shown", shown}, {"topics", topics}};
  if (matched > shown) {
    out["note"] = "truncated to " + std::to_string(shown) + " of " + std::to_string(matched) +
                  "; refine with a filter or raise limit";
  }
  if (!objects_listed) {
    out["objects"] = "not listed (host predates catalog snapshot v2)";
  }
  return ToolResult::success(out.dump());
}

ToolResult describeTopic(const json& args, ToolContext& ctx) {
  if (!args.contains("topic") || !args["topic"].is_string()) {
    return ToolResult::failure("describe_topic requires a string 'topic'");
  }
  const std::string want = args["topic"].get<std::string>();

  auto v2 = ctx.host.catalogSnapshotV2();
  if (v2) {
    // The same resolution every other object input gets: a "dataset:" qualifier is honoured and a name on
    // several datasets is refused with the qualified candidates.
    const ObjectLookup lookup = resolveObjectTopic(*v2, want);
    if (lookup.ambiguous) {
      return ToolResult::failure(objectLookupError(want, lookup));
    }
    if (lookup.resolved) {
      return describeObjectTopic(ctx, v2->dataSources(), lookup.resolved->info);
    }
    return describeScalarTopic(v2->topics(), v2->fields(), v2->dataSources(), want);
  }

  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }
  return describeScalarTopic(catalog->topics(), catalog->fields(), catalog->dataSources(), want);
}

ToolResult playbackTool(const json& args, ToolContext& ctx) {
  const std::string action = args.value("action", std::string());
  if (action.empty()) {
    return ToolResult::failure("playback requires 'action': one of 'state', 'play', 'pause', 'seek', 'rate'");
  }
  if (!ctx.playback.valid()) {
    return ToolResult::failure(kNoPlayback);
  }
  if (action == "state") {
    auto state = ctx.playback.state();
    if (!state) {
      return ToolResult::failure(state.error());
    }
    return ToolResult::success(stateToJson(*state).dump());
  }
  if (action == "play") {
    return runPlaybackOp(ctx, "play", [&] { return ctx.playback.play(); });
  }
  if (action == "pause") {
    return runPlaybackOp(ctx, "pause", [&] { return ctx.playback.pause(); });
  }
  if (action == "seek") {
    if (!args.contains("time_s") || !args["time_s"].is_number()) {
      return ToolResult::failure("seek requires a numeric 'time_s' (display-axis seconds)");
    }
    // The echoed current_time_s shows the host's clamp into the playback range.
    return runPlaybackOp(ctx, "seek", [&] { return ctx.playback.seek(args["time_s"].get<double>()); });
  }
  if (action == "rate") {
    if (!args.contains("rate") || !args["rate"].is_number()) {
      return ToolResult::failure("rate requires a numeric 'rate' (> 0; 1.0 = real time)");
    }
    // Bound the blast radius before the host sees it: a model asking for 0 or
    // 10000x gets the nearest sane speed instead of an error loop.
    const double rate = std::clamp(args["rate"].get<double>(), 0.05, 20.0);
    return runPlaybackOp(ctx, "rate", [&] { return ctx.playback.setPlaybackRate(rate); });
  }
  return ToolResult::failure("unknown playback action '" + action + "'; use state/play/pause/seek/rate");
}

ToolResult plotTabTool(const json& args, ToolContext& ctx) {
  const std::string action = args.value("action", std::string());
  if (action.empty()) {
    return ToolResult::failure("plot_tab requires 'action': one of 'create', 'add', 'remove', 'zoom', 'close', 'list'");
  }
  if (!ctx.plot_tabs.valid()) {
    return ToolResult::failure(kNoPlotTabs);
  }
  if (action == "list") {
    auto ids = ctx.plot_tabs.list();
    if (!ids) {
      return ToolResult::failure(ids.error());
    }
    json arr = json::array();
    for (const std::string& id : *ids) {
      TabConfig config = fetchTabConfig(ctx, id);
      if (config.isScene()) {
        continue;  // scene tabs belong to scene_view
      }
      arr.push_back(readBackFrom(std::move(config), id, "tab"));
    }
    // Owning nothing is an answer, not a failure.
    return ToolResult::success(json({{"count", arr.size()}, {"tabs", arr}}).dump());
  }

  const std::string tab = args.value("tab", std::string());
  if (action == "create") {
    // A name of its own, so the model can address the tab again next turn
    // without having to remember a host-chosen handle.
    const std::string id = tab.empty() ? "view" : tab;
    if (auto status = ctx.plot_tabs.create(id, args.value("title", std::string())); !status) {
      return ToolResult::failure("could not create the tab: " + status.error());
    }
    return ToolResult::success(tabReadBack(ctx, id).dump());
  }
  if (tab.empty()) {
    return ToolResult::failure(
        "'" + action + "' needs 'tab', the name of one of your own tabs (you have: " + ownedTabList(ctx) +
        "). Only tabs you created can be changed; the user's tabs are not yours to touch.");
  }

  if (action == "close") {
    if (auto status = ctx.plot_tabs.close(tab); !status) {
      return ToolResult::failure(status.error() + " (yours: " + ownedTabList(ctx) + ")");
    }
    return ToolResult::success(json({{"closed", tab}}).dump());
  }
  if (action == "zoom") {
    const std::vector<std::string> mine = ownedIdsOfKind(ctx, /*scene=*/false);
    if (std::find(mine.begin(), mine.end(), tab) == mine.end()) {
      return ToolResult::failure(
          "you have no plot tab '" + tab + "' (yours: " + joinOrNone(mine) +
          "). Only tabs you created can be zoomed; the user's tabs are not yours to touch.");
    }
    // The two services are meant to be registered together, so this only fires
    // on a host that half-adopted them — worth saying plainly rather than
    // failing as if the tab were at fault.
    if (!ctx.zoom_tab && !ctx.viewport.valid()) {
      return ToolResult::failure(kNoViewport);
    }
    // Without the host's per-tab zoom, the viewport service frames every tab the caller owns.
    const auto zoom = [&ctx, &tab](std::optional<std::pair<double, double>> range) {
      if (ctx.zoom_tab) {
        return ctx.zoom_tab(tab, range);
      }
      return range ? ctx.viewport.zoomToTimeRange(range->first, range->second) : ctx.viewport.zoomReset();
    };
    const bool has_start = args.contains("start_s") && args["start_s"].is_number();
    const bool has_end = args.contains("end_s") && args["end_s"].is_number();
    if (!has_start && !has_end) {
      if (auto status = zoom(std::nullopt); !status) {
        return ToolResult::failure("could not fit the view: " + status.error());
      }
      return ToolResult::success(json({{"tab", tab}, {"x_range", "fit"}}).dump());
    }
    if (!has_start || !has_end) {
      return ToolResult::failure("zoom needs both 'start_s' and 'end_s' (display-axis seconds), or neither to fit");
    }
    const double start_s = args["start_s"].get<double>();
    const double end_s = args["end_s"].get<double>();
    if (start_s >= end_s) {
      return ToolResult::failure("'start_s' must be less than 'end_s'");
    }
    if (auto status = zoom(std::pair{start_s, end_s}); !status) {
      return ToolResult::failure("could not zoom: " + status.error());
    }
    return ToolResult::success(json({{"tab", tab}, {"x_range", {{"start_s", start_s}, {"end_s", end_s}}}}).dump());
  }

  if (action != "add" && action != "remove") {
    return ToolResult::failure("unknown plot_tab action '" + action + "'; use create/add/remove/zoom/close/list");
  }
  const std::vector<std::string> paths = requestedPaths(args, {"curves"});
  if (paths.empty()) {
    return ToolResult::failure("'" + action + "' needs 'curves': one topic/field path, or an array of them");
  }
  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }
  const bool adding = action == "add";
  std::vector<std::string> refused;
  std::vector<std::pair<std::string, std::string>> accepted;  // (topic, field) the host took without complaint
  for (const std::string& want : paths) {
    SeriesLookup lookup = resolveSeriesPath(*catalog, want);
    if (!lookup.resolved) {
      refused.push_back(seriesLookupError(want, lookup));
      continue;
    }
    // The host addresses a series by its parts and resolves the dataset itself,
    // so hand it the pieces rather than a joined path it would have to split.
    const std::string topic = lookup.resolved->topic;
    const std::string field = lookup.resolved->host_path.substr(topic.size() + 1);
    auto status = adding ? ctx.plot_tabs.addCurve(tab, topic, field, lookup.resolved->dataset)
                         : ctx.plot_tabs.removeCurve(tab, topic, field, lookup.resolved->dataset);
    if (status) {
      accepted.emplace_back(topic, field);
    } else {
      refused.push_back(status.error());
    }
  }

  json out = tabReadBack(ctx, tab);
  // A call the host accepted is not yet a curve on screen: it may resolve to
  // nothing and simply leave the tab as it was. So the verdict comes from what
  // the tab HOLDS, not from what the calls returned — otherwise this reports a
  // drawing that never happened, which is the one thing the read-back exists to
  // prevent.
  const json& held = out.contains("curves") ? out["curves"] : json::array();
  for (const auto& [topic, field] : accepted) {
    const bool present = std::any_of(held.begin(), held.end(), [&](const json& curve) {
      return curve.value("topic", std::string()) == topic && curve.value("field", std::string()) == field;
    });
    if (present == adding) {
      continue;  // added and there, or removed and gone: what was asked for
    }
    refused.push_back(
        adding ? "'" + topic + "/" + field + "' did not land in the tab"
               : "'" + topic + "/" + field + "' is still drawn");
  }
  if (!refused.empty()) {
    out["refused"] = refused;
  }
  // Nothing landed at all is a failure; a partial landing is a success whose
  // truth the model still has to see.
  return refused.size() == paths.size() ? ToolResult::failure(out.dump()) : ToolResult::success(out.dump());
}

ToolResult sceneViewTool(const json& args, ToolContext& ctx) {
  const std::string action = args.value("action", std::string());
  if (action.empty()) {
    return ToolResult::failure(
        "scene_view requires 'action': one of 'create', 'attach', 'detach', 'focus', 'close', 'list'");
  }
  if (action == "list") {
    auto ids = ctx.plot_tabs.list();
    if (!ids) {
      return ToolResult::failure(ids.error());
    }
    json arr = json::array();
    for (const std::string& id : *ids) {
      TabConfig config = fetchTabConfig(ctx, id);
      if (!config.isScene()) {
        continue;  // plot tabs belong to plot_tab
      }
      arr.push_back(readBackFrom(std::move(config), id, "view"));
    }
    // Owning nothing is an answer, not a failure.
    return ToolResult::success(json({{"count", arr.size()}, {"views", arr}}).dump());
  }

  const std::string view = args.value("view", std::string());
  if (action == "create") {
    // A name of its own, so the model can address the view again next turn
    // without having to remember a host-chosen handle.
    const std::string id = view.empty() ? "scene" : view;
    const std::string kind = args.value("kind", std::string("3d"));
    if (kind != "3d" && kind != "2d") {
      return ToolResult::failure("'kind' must be \"3d\" or \"2d\"");
    }
    if (auto status = ctx.plot_tabs.createTabV2(id, kind, args.value("title", std::string())); !status) {
      return ToolResult::failure("could not create the view: " + status.error());
    }
    return ToolResult::success(viewReadBack(ctx, id).dump());
  }
  if (view.empty()) {
    return ToolResult::failure(
        "'" + action + "' needs 'view', the name of one of your own scene views (you have: " + ownedViewList(ctx) +
        "). Only views you created can be changed; the user's scene docks are not yours to touch.");
  }

  if (action == "focus") {
    if (auto status = ctx.plot_tabs.focusTab(view); !status) {
      return ToolResult::failure(status.error() + " (yours: " + ownedViewList(ctx) + ")");
    }
    return ToolResult::success(json({{"focused", view}}).dump());
  }
  if (action == "close") {
    if (auto status = ctx.plot_tabs.close(view); !status) {
      return ToolResult::failure(status.error() + " (yours: " + ownedViewList(ctx) + ")");
    }
    return ToolResult::success(json({{"closed", view}}).dump());
  }

  if (action != "attach" && action != "detach") {
    return ToolResult::failure("unknown scene_view action '" + action + "'; use create/attach/detach/focus/close/list");
  }
  const std::vector<std::string> paths = requestedPaths(args, {"topics"});
  if (paths.empty()) {
    return ToolResult::failure("'" + action + "' needs 'topics': one object-topic path, or an array of them");
  }
  auto v2 = ctx.host.catalogSnapshotV2();
  if (!v2) {
    return ToolResult::failure(
        "scene_view requires catalog snapshot v2, which this host does not expose: " + v2.error());
  }
  const bool attaching = action == "attach";
  std::vector<SceneTopicOutcome> outcomes;
  outcomes.reserve(paths.size());
  for (const std::string& want : paths) {
    SceneTopicOutcome o;
    ObjectLookup lookup = resolveObjectTopic(*v2, want);
    if (lookup.ambiguous) {
      o.display_topic = want;
      o.error = objectLookupError(want, lookup);
      outcomes.push_back(std::move(o));
      continue;
    }
    if (!lookup.resolved) {
      o.display_topic = want;
      o.error = "'" + want + "' is not a loaded object topic";
      outcomes.push_back(std::move(o));
      continue;
    }
    o.display_topic = lookup.resolved->display_path;
    o.bare_topic = lookup.resolved->host_path;
    o.dataset = objectTopicDatasetName(v2->dataSources(), lookup.resolved->source);
    // The host addresses a view topic by its bare name and resolves the
    // dataset itself, the same convention plotTabTool's addCurve follows.
    auto status = attaching ? ctx.plot_tabs.attachTopic(view, o.bare_topic, o.dataset)
                            : ctx.plot_tabs.detachTopic(view, o.bare_topic, o.dataset);
    if (!status) {
      o.error = status.error();
      outcomes.push_back(std::move(o));
      continue;
    }
    o.called_ok = true;
    outcomes.push_back(std::move(o));
  }

  json out = viewReadBack(ctx, view);
  // A call the host accepted is not yet a topic on screen: it may resolve to
  // nothing and simply leave the view as it was. So, as in plotTabTool, the
  // verdict comes from what the view HOLDS, not from what the calls returned.
  const json& held = out.contains("topics") ? out["topics"] : json::array();
  json results = json::array();
  std::size_t landed = 0;
  for (const auto& o : outcomes) {
    if (!o.called_ok) {
      results.push_back({{"topic", o.display_topic}, {"error", o.error}});
      continue;
    }
    const bool present = std::any_of(held.begin(), held.end(), [&](const json& t) {
      return t.value("topic", std::string()) == o.bare_topic && t.value("dataset", std::string()) == o.dataset;
    });
    if (present == attaching) {
      results.push_back({{"topic", o.display_topic}, {attaching ? "attached" : "detached", true}});
      ++landed;
    } else {
      results.push_back(
          {{"topic", o.display_topic}, {"error", attaching ? "did not land in the view" : "is still attached"}});
    }
  }
  out["results"] = results;
  // Nothing landed at all is a failure; a partial landing is a success whose
  // truth the model still has to see.
  return landed == 0 ? ToolResult::failure(out.dump()) : ToolResult::success(out.dump());
}

ToolResult reportStatus(const json& /*args*/, ToolContext& ctx) {
  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }
  // Name the datasets rather than only counting them: "2" cannot be acted on,
  // and which two is exactly what decides whether series may be combined.
  json names = json::array();
  for (const auto& src : catalog->dataSources()) {
    names.push_back(std::string(PJ::sdk::toStringView(src.name)));
  }
  json out = {
      {"data_sources", catalog->dataSources().size()},
      {"topics", catalog->topics().size()},
      {"fields", catalog->fields().size()}};
  if (!names.empty()) {
    out["dataset_names"] = names;
  }

  auto v2 = ctx.host.catalogSnapshotV2();
  if (v2) {
    std::size_t object_topics = 0;
    std::size_t derived_object_topics = 0;
    for (const auto& obj : v2->objectTopics()) {
      const std::string name(PJ::sdk::toStringView(obj.name));
      if (isMarkerObjectTopic(name)) {
        continue;  // drawn, not read -- not counted as an object topic
      }
      ++object_topics;
      if (metadataHasKey(PJ::sdk::toStringView(obj.metadata_json), PJ::sdk::kDerivedMetadataKey)) {
        ++derived_object_topics;
      }
    }
    out["object_topics"] = object_topics;
    out["derived_object_topics"] = derived_object_topics;
  } else {
    out["object_topics"] = 0;
    out["derived_object_topics"] = 0;
    out["objects"] = "not listed (host predates catalog snapshot v2)";
  }
  // The host scopes list()/recipeOf() to this assistant's namespace.
  json findings = {{"pinned", 0}, {"bytes", 0}, {"complete", true}};
  json processors = json::array();
  auto ids = ctx.dp.list();
  if (!ids) {
    findings["complete"] = false;
  } else {
    for (const auto& id : *ids) {
      auto recipe = ctx.dp.recipeOf(id);
      const auto node = recipe ? json::parse(*recipe, nullptr, false) : json();
      if (!node.is_object()) {
        findings["complete"] = false;
        continue;
      }
      if (node.contains("pinned_t_ns")) {
        findings["pinned"] = findings["pinned"].get<std::size_t>() + 1;
        if (node.contains("memory_bytes") && node["memory_bytes"].is_number_unsigned()) {
          findings["bytes"] = findings["bytes"].get<std::uint64_t>() + node["memory_bytes"].get<std::uint64_t>();
        } else {
          findings["complete"] = false;
        }
      }
      json status = {{"name", id}};
      for (const auto* key : {"kind", "state", "error", "detail", "requested_ns", "stale"}) {
        if (node.contains(key)) {
          status[key] = node[key];
        }
      }
      processors.push_back(std::move(status));
    }
  }
  out["findings"] = std::move(findings);
  out["processors"] = std::move(processors);
  return ToolResult::success(out.dump());
}

}  // namespace PJ::agent_tools::detail
