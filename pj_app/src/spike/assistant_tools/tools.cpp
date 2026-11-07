// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The assistant's tool executors. Each is a pure function of (args, host views)
// and returns a ToolResult whose `content` the model reads. Every failure is
// returned as data (ok=false) — exceptions must never cross the plugin ABI, so
// nothing here throws out.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/builtin/builtin_object.hpp>
#include <pj_base/builtin/field_table.hpp>
#include <pj_base/builtin/field_table_registry.hpp>
#include <pj_base/builtin/plot_markers.hpp>
#include <pj_base/sdk/object_topic_metadata.hpp>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "luau_transform.hpp"
#include "object_ops_catalog.hpp"
#include "series_stats.hpp"
#include "tool_registry.hpp"

namespace assistant_agent {
namespace {

using nlohmann::json;

// Catalog resolution, typed-output parsing and the recipe chunk builders are shared with the
// Transform Editor (common/derived_recipes).
using derived_recipes::buildResolvedOnDemandChunk;
using derived_recipes::canonicalSeriesPath;
using derived_recipes::datasetByTopicIndex;
using derived_recipes::dataSourceHandleFor;
using derived_recipes::isMarkerObjectTopic;
using derived_recipes::joinSeriesPath;
using derived_recipes::kMaxCandidates;
using derived_recipes::matchDatasetQualifier;
using derived_recipes::ObjectLookup;
using derived_recipes::objectLookupError;
using derived_recipes::objectTopicDatasetName;
using derived_recipes::ParsedOutputs;
using derived_recipes::parseTypedOutputs;
using derived_recipes::QualifierMatch;
using derived_recipes::qualifyWithDataset;
using derived_recipes::ResolvedEvalInput;
using derived_recipes::ResolvedEvalInputs;
using derived_recipes::resolveObjectTopic;
using derived_recipes::seriesLookupError;
using derived_recipes::TopicDatasetMap;

// The shared helpers take explicit host views; these keep the executors' ToolContext call sites.
ResolvedEvalInputs resolveEvalInputs(
    ToolContext& ctx, const PJ::sdk::CatalogSnapshotV2& v2, const std::vector<std::string>& raw_inputs) {
  return derived_recipes::resolveEvalInputs(ctx.host, v2, raw_inputs);
}

std::optional<std::int64_t> toRawNs(ToolContext& ctx, PJ::sdk::DataSourceHandle source, double display_s) {
  return derived_recipes::toRawNs(ctx.playback, source, display_s);
}

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

// Cap on a single tool response handed back to the model, so a wide catalog or
// a long series can't blow the context window. read_series coarsens to fit.
constexpr std::size_t kMaxResponseBytes = 16 * 1024;

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

// --- object topics (catalog snapshot v2) ------------------------------------
//
// Object topics (point clouds, scene entities, images…) live beside the
// scalar catalog in PJ_catalog_snapshot_v2_t. Marker sets are object topics
// too, but they are drawn, not read — kMarkerObjectTopicPrefix filters them
// out of everything a model sees (list_topics, describe_topic, the digest,
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

// Whether the HOST can address this series by name for a create, and the error
// to hand back when it cannot. The create side of pj.data_processors.v1 takes
// bare names; when the same topic/field exists in several datasets the bare
// name no longer picks one, so handing it over would either fail or land on
// whichever dataset the host resolves first. Reads are unaffected — they go by
// handle — so this is a create-only limit, refused loudly here instead of
// silently mistargeted there.
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

// Materialize a numeric field into parallel timestamp + double columns,
// coercing any numeric column type to double. Returns false for a non-numeric
// column (string/bool) or an empty/mismatched read.
bool readSeriesDoubles(
    const PJ::sdk::MaterializedSeriesView& view, std::vector<std::int64_t>& out_ts, std::vector<double>& out_vals) {
  const std::size_t n = view.rowCount();
  auto ts = view.timestamps();
  if (n == 0 || ts.size() != n) {
    return false;
  }
  out_ts.assign(ts.begin(), ts.end());
  out_vals.resize(n);

  auto copy_from = [&](const auto* p) -> bool {
    if (p == nullptr) {
      return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
      out_vals[i] = static_cast<double>(p[i]);
    }
    return true;
  };

  switch (view.type()) {
    case PJ::PrimitiveType::kFloat64:
      return copy_from(view.valuesAsFloat64());
    case PJ::PrimitiveType::kFloat32:
      return copy_from(view.valuesAsFloat32());
    case PJ::PrimitiveType::kInt8:
      return copy_from(view.valuesAsInt8());
    case PJ::PrimitiveType::kInt16:
      return copy_from(view.valuesAsInt16());
    case PJ::PrimitiveType::kInt32:
      return copy_from(view.valuesAsInt32());
    case PJ::PrimitiveType::kInt64:
      return copy_from(view.valuesAsInt64());
    case PJ::PrimitiveType::kUint8:
      return copy_from(view.valuesAsUint8());
    case PJ::PrimitiveType::kUint16:
      return copy_from(view.valuesAsUint16());
    case PJ::PrimitiveType::kUint32:
      return copy_from(view.valuesAsUint32());
    case PJ::PrimitiveType::kUint64:
      return copy_from(view.valuesAsUint64());
    default:
      return false;  // bool/string aren't plottable numeric series
  }
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

// Scalar-topic half of describe_topic, shared by the catalog-snapshot-v2 and
// legacy-v1 paths (identical span shapes either way).
ToolResult describeScalarTopic(
    std::span<const PJ_topic_info_t> topics, std::span<const PJ_field_info_t> fields,
    const std::map<std::uint32_t, std::string>& topic_dataset, const std::string& want) {
  for (std::uint32_t ti = 0; ti < topics.size(); ++ti) {
    const auto& topic = topics[ti];
    if (std::string(PJ::sdk::toStringView(topic.name)) != want) {
      continue;
    }
    json field_arr = json::array();
    for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
      const std::size_t idx = topic.first_field + fi;
      if (idx >= fields.size()) {
        break;
      }
      const std::string leaf(PJ::sdk::toStringView(fields[idx].name));
      json entry = {
          {"field", leaf},
          {"path", joinSeriesPath(want, leaf)},
          {"type", primitiveTypeName(PJ::sdk::fromAbiType(fields[idx].type))}};
      field_arr.push_back(entry);
    }
    json out = {{"topic", want}, {"kind", "scalar"}, {"fields", field_arr}};
    if (auto it = topic_dataset.find(ti); it != topic_dataset.end()) {
      out["dataset"] = it->second;
    }
    return ToolResult::success(out.dump());
  }
  return ToolResult::failure("no topic named '" + want + "' (use list_topics)");
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
    return describeScalarTopic(v2->topics(), v2->fields(), datasetByTopicIndex(v2->dataSources()), want);
  }

  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }
  return describeScalarTopic(catalog->topics(), catalog->fields(), datasetByTopicIndex(catalog->dataSources()), want);
}

json statsToJson(const SeriesStats& s) {
  json out = {{"count", s.count}, {"duration_s", s.duration_s}, {"rate_hz", s.rate_hz}};
  // NaN/inf poison a straight sum, so min/max/mean/stddev are computed over
  // finite values only; when none exist there is nothing honest to report, so
  // they are omitted rather than serialized as NaN (which nlohmann turns into
  // `null` — indistinguishable from a field that was never populated).
  if (s.has_values) {
    out["min"] = s.min;
    out["max"] = s.max;
    out["min_at_s"] = s.min_at_s;
    out["max_at_s"] = s.max_at_s;
    out["mean"] = s.mean;
    out["stddev"] = s.stddev;
  } else {
    out["note"] = "no finite values";
  }
  // The spacing facts that count/mean/rate cannot carry: a dropout leaves all
  // three untouched. The keys are self-describing on purpose — the tool's
  // schema description says nothing about them, so they cost prefix tokens in
  // no turn and appear exactly when a series is read.
  if (s.has_gap) {
    out["max_gap_s"] = s.max_gap_s;
    out["max_gap_at_s"] = s.max_gap_at_s;
  }
  if (s.invalid > 0) {
    out["invalid"] = s.invalid;
    out["invalid_fraction"] = static_cast<double>(s.invalid) / static_cast<double>(s.count);
  }
  return out;
}

// How a multi-input transform would fare BEFORE anything is installed.
//
// PJ4 joins the inputs of a multi-input transform on EXACT timestamp equality
// (pj_datastore run_mimo_incremental). Inputs that share no timestamps produce a
// series with zero points — created "successfully", drawn as nothing, with no
// error anywhere. That is not a rare corner: two recordings of the same robot
// are exactly that, and comparing two runs is a perfectly reasonable thing to
// ask for.
//
// So this measures the real intersection rather than guessing from sample rates
// (two 100 Hz series can still share nothing if one is offset by half a sample —
// a rate comparison would call that compatible and be wrong).
struct JoinForecast {
  bool checked = false;            // false when an input could not be read; draw no conclusion
  std::size_t shared = 0;          // timestamps common to every input
  std::size_t smallest = 0;        // rows in the shortest input, the ceiling for `shared`
  std::vector<std::string> rates;  // "path (100.0 Hz, 200 samples)" per input, for the message
};

JoinForecast forecastJoin(
    const ToolContext& ctx, const PJ::sdk::CatalogSnapshot& catalog, const std::vector<std::string>& inputs) {
  JoinForecast out;
  std::vector<std::int64_t> common;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    auto lookup = resolveSeriesPath(catalog, inputs[i]);
    if (!lookup.resolved) {
      return out;
    }
    auto view = ctx.host.readSeries(lookup.resolved->handle);
    if (!view) {
      return out;
    }
    std::vector<std::int64_t> ts;
    std::vector<double> vals;
    if (!readSeriesDoubles(*view, ts, vals)) {
      return out;  // a non-numeric input fails later, on its own terms
    }
    const SeriesStats stats = computeStats(ts, vals);
    std::ostringstream rate;
    rate << inputs[i] << " (" << std::fixed << std::setprecision(1) << stats.rate_hz << " Hz, " << ts.size()
         << " samples)";
    out.rates.push_back(rate.str());
    out.smallest = (i == 0) ? ts.size() : std::min(out.smallest, ts.size());

    std::sort(ts.begin(), ts.end());
    ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
    if (i == 0) {
      common = std::move(ts);
    } else {
      std::vector<std::int64_t> next;
      std::set_intersection(common.begin(), common.end(), ts.begin(), ts.end(), std::back_inserter(next));
      common = std::move(next);
    }
  }
  out.checked = true;
  out.shared = common.size();
  return out;
}

// Cap on how many series one call may read. Stats are small and fixed-size per
// series, so this is not about the response cap — it is about not turning a
// single tool call into an unbounded scan of the whole dataset.
constexpr std::size_t kMaxBatchPaths = 32;

// Buckets are the expensive, variable payload — several of them share ONE
// response cap (see batchBuckets), so the batch stays smaller than the flat
// per-path cap above: past this many series sharing the cap coarsens every
// one of them past usefulness rather than saving a round trip.
constexpr std::size_t kMaxBucketsBatchPaths = 8;

// Hard cap on samples mode 'raw' returns per series: raw exists so the model
// can see actual values once it has narrowed to a handful of interesting
// ones, not to ship a whole recording through the tool loop one call at a
// time.
constexpr std::size_t kMaxRawSamples = 200;

// Batch cap for mode 'raw' — tighter than buckets' own, since each series
// here can carry up to kMaxRawSamples individual numbers rather than a
// bounded bucket count, so a handful of series already approaches the
// response cap.
constexpr std::size_t kMaxRawBatchPaths = 4;

// Collect the requested paths from either a bare string or an array.
//
// The parameter is named `paths` rather than `series` on purpose. The object
// store holds point clouds, images and occupancy grids behind the same read
// view, and when this call learns to summarize those too, "series" would be the
// wrong word for what it takes. The model learns the name from the description,
// so renaming it later is more expensive than choosing it now. `series` still
// works, undocumented, so a conversation already in flight does not break.
std::vector<std::string> requestedPaths(
    const json& args, std::initializer_list<const char*> keys = {"paths", "series"}) {
  std::vector<std::string> out;
  for (const char* key : keys) {
    if (!args.contains(key)) {
      continue;
    }
    const json& v = args[key];
    if (v.is_string()) {
      out.push_back(canonicalSeriesPath(v.get<std::string>()));
    } else if (v.is_array()) {
      for (const auto& e : v) {
        if (e.is_string()) {
          out.push_back(canonicalSeriesPath(e.get<std::string>()));
        }
      }
    }
    if (!out.empty()) {
      break;
    }
  }
  return out;
}

// Read one series into timestamps + doubles, or return why not. Shared by both
// modes so a batch entry and a single read fail with the same wording.
struct SeriesRead {
  bool ok = false;
  std::string error;
  std::string path;     // the resolved path, which may differ from what was asked
  std::string topic;    // the owning topic — the key display-time conversion wants
  std::string dataset;  // source name; empty when at most one dataset is loaded
  std::vector<std::int64_t> ts;
  std::vector<double> vals;
  // Present only when a t_start_s/t_end_s window was requested and applied —
  // echoed back on the entry so a later turn knows count/rate_hz/duration_s
  // and the bucket time axis are the WINDOW's, not the whole series'.
  json window;
  // Computed by readOne on the full, unwindowed ts/vals — see
  // FlatRunSummary's comment for why this must stay whole-series even when
  // a window narrows everything else in this struct.
  FlatRunSummary flat;
  // display(ts) = ts*1e-9 + *display_offset_s -- set by applyDisplayWindow
  // when a window was applied (its anchor sample's own ns->display
  // conversion), so renderRawSamples can derive t0_display_s from it
  // directly instead of re-running the ns->display conversion for the first
  // sample. Absent when no window was requested.
  std::optional<double> display_offset_s;
};

// With a playback host bound, report where this series STARTS on the plot
// axis, so the model can turn bucket-relative times into seek/zoom targets:
// display time of a bucket = t_start_display_s + bucket.t. Best-effort — the
// conversion is frame-dependent (user-editable offsets), never an error here.
json statsWithDisplayStart(
    const SeriesStats& stats, ToolContext& ctx, const std::string& topic, const FlatRunSummary& flat) {
  json stats_json = statsToJson(stats);
  bool has_display_start = false;
  if (ctx.playback.valid() && stats.count > 0) {
    if (auto display_s = ctx.playback.toDisplayTime(topic, stats.t_start_ns)) {
      stats_json["t_start_display_s"] = *display_s;
      has_display_start = true;
    }
  }
  // Whole-series facts (see FlatRunSummary): a constant channel says so
  // instead of the flat span, which would just restate "the whole thing".
  // Each fact carries a fixed-text interpretation alongside it -- "constant"
  // reads as "stuck" to a model unless told it just means "never sampled
  // differently", and a bare flat span invites the same misreading in the
  // other direction (a change WITHIN the recording, not since the start).
  if (flat.constant) {
    stats_json["constant"] = true;
    stats_json["constant_note"] =
        "held one value for the entire recording — an unused or unmapped output, not something that changed "
        "during it";
  } else if (flat.has_flat_span) {
    stats_json["flat_span_s"] = flat.flat_span_s;
    stats_json["flat_span_at_s"] = flat.flat_span_at_s;
    std::ostringstream note;
    note << std::fixed << std::setprecision(1);
    note << "held one value for " << flat.flat_span_s << " s starting at " << flat.flat_span_at_s
         << " s into the series";
    if (has_display_start) {
      note << " (display: t_start_display_s + " << flat.flat_span_at_s << ")";
    }
    note << " — a change within the recording";
    stats_json["flat_span_note"] = note.str();
  }
  return stats_json;
}

SeriesRead readOne(
    const PJ::sdk::CatalogSnapshot& catalog, ToolContext& ctx, const std::string& want,
    const TopicDatasetMap& topic_dataset) {
  SeriesRead r;
  r.path = want;
  auto lookup = resolveSeriesPath(catalog, want, topic_dataset);
  if (!lookup.resolved) {
    r.error = seriesLookupError(want, lookup);
    return r;
  }
  r.path = lookup.resolved->path;
  r.topic = lookup.resolved->topic;
  r.dataset = lookup.resolved->dataset;
  auto view = ctx.host.readSeries(lookup.resolved->handle);
  if (!view) {
    r.error = "read failed for '" + want + "': " + view.error();
    return r;
  }
  if (!readSeriesDoubles(*view, r.ts, r.vals)) {
    r.error = "series '" + want + "' is not a numeric time series";
    return r;
  }
  // Whole-series facts, computed here — before any t_start_s/t_end_s window
  // narrows r.ts/r.vals below — so "never changes" / "freezes for N s" stay
  // claims about the entire recording even under a windowed read.
  r.flat = flatRunSummary(r.ts, r.vals);
  r.ok = true;
  return r;
}

// Best-effort absolute-ns -> display-seconds conversion for one series,
// preferring the dataset-scoped host call (immune to the "ambiguous topic
// between datasets" failure mode the plain topic-scoped one has) and falling
// back to the topic-scoped one when the host does not offer the former.
// Empty when neither is bound or neither succeeds — never guessed.
std::optional<double> toDisplaySeconds(
    ToolContext& ctx, const PJ::sdk::CatalogSnapshot& catalog, const SeriesRead& r, std::int64_t absolute_ns) {
  if (auto handle = dataSourceHandleFor(catalog, r.dataset)) {
    if (auto display_s = ctx.playback.toDisplayTimeForSource(*handle, absolute_ns)) {
      return *display_s;
    }
  }
  if (auto display_s = ctx.playback.toDisplayTime(r.topic, absolute_ns)) {
    return *display_s;
  }
  return std::nullopt;
}

// Narrows a successfully-read series to [t_start_s, t_end_s] on the DISPLAY
// axis (playback's, plot_tab zoom's, stats' own t_start_display_s) when
// either bound was requested; a no-op otherwise. The series' own first
// sample is the only anchor available for the ns<->display conversion (see
// toDisplaySeconds): the affine offset it yields — display(ts.front()) minus
// ts.front() in seconds — is applied to translate the window's bounds into
// absolute ns (floor at the start, ceil at the end), and the series is
// trimmed to that ns range via lower_bound/upper_bound before stats or
// buckets ever see it. So a windowed read's t_start_display_s and bucket 't'
// come out relative to the first sample INSIDE the window, never to
// t_start_s itself.
//
// Failure — no anchor sample, no conversion available, or an empty result —
// turns the entry into an error (r.ok becomes false) rather than silently
// reinterpreting the window as relative ns, which would quietly answer a
// different question than the one asked.
void applyDisplayWindow(
    SeriesRead& r, ToolContext& ctx, const PJ::sdk::CatalogSnapshot& catalog, const std::optional<double>& t_start_s,
    const std::optional<double>& t_end_s) {
  if (!t_start_s && !t_end_s) {
    return;
  }
  if (r.ts.empty()) {
    r.ok = false;
    r.error = "cannot map the display window for this series";
    return;
  }
  const std::int64_t anchor_ns = r.ts.front();
  const std::optional<double> anchor_display = toDisplaySeconds(ctx, catalog, r, anchor_ns);
  if (!anchor_display) {
    r.ok = false;
    r.error = "cannot map the display window for this series";
    return;
  }
  const double offset = *anchor_display - static_cast<double>(anchor_ns) * 1e-9;
  auto toAbsNs = [&](double display_s, bool round_up) {
    const double ns = (display_s - offset) * 1e9;
    return static_cast<std::int64_t>(round_up ? std::ceil(ns) : std::floor(ns));
  };

  auto lo_it = r.ts.begin();
  auto hi_it = r.ts.end();
  if (t_start_s) {
    lo_it = std::lower_bound(r.ts.begin(), r.ts.end(), toAbsNs(*t_start_s, /*round_up=*/false));
  }
  if (t_end_s) {
    hi_it = std::upper_bound(r.ts.begin(), r.ts.end(), toAbsNs(*t_end_s, /*round_up=*/true));
  }
  if (lo_it >= hi_it) {
    const double series_start_display = *anchor_display;
    const double series_end_display = static_cast<double>(r.ts.back()) * 1e-9 + offset;
    std::ostringstream msg;
    msg << "no samples in window; series spans display [" << series_start_display << ", " << series_end_display << "]";
    r.ok = false;
    r.error = msg.str();
    return;
  }

  // Trim by INDEX, not by copying through the iterators above: ts and vals
  // are reassigned in place below, and an iterator pair that aliases the
  // vector being assigned is not safe to feed straight to vector::assign.
  const auto lo = static_cast<std::size_t>(lo_it - r.ts.begin());
  const auto hi = static_cast<std::size_t>(hi_it - r.ts.begin());
  std::vector<std::int64_t> windowed_ts(
      r.ts.begin() + static_cast<std::ptrdiff_t>(lo), r.ts.begin() + static_cast<std::ptrdiff_t>(hi));
  std::vector<double> windowed_vals(
      r.vals.begin() + static_cast<std::ptrdiff_t>(lo), r.vals.begin() + static_cast<std::ptrdiff_t>(hi));
  r.ts = std::move(windowed_ts);
  r.vals = std::move(windowed_vals);
  r.display_offset_s = offset;

  r.window = {{"axis", "display"}};
  if (t_start_s) {
    r.window["t_start_s"] = *t_start_s;
  }
  if (t_end_s) {
    r.window["t_end_s"] = *t_end_s;
  }
}

// Round to `sig_figs` significant decimal digits before serializing: the raw
// float32 sources rarely carry more precision than that, and a double's full
// ~17-digit decimal expansion is roughly twice the bytes for no information
// the model can use. Leaves 0 and non-finite values untouched (a NaN/inf
// still round-trips through nlohmann as JSON null via renderBucketArray's own
// isfinite guard, not through this helper).
double roundSignificant(double v, int sig_figs = 6) {
  if (!std::isfinite(v) || v == 0.0) {
    return v;
  }
  const double magnitude = std::pow(10.0, sig_figs - 1 - static_cast<int>(std::floor(std::log10(std::fabs(v)))));
  return std::round(v * magnitude) / magnitude;
}

// Bucketize and render as the columnar JSON object both readSeriesTool's
// 'buckets' mode (single-series and batched) and evaluate's optional bucket
// summary send back: parallel arrays ("n"/"min"/"max"/"mean") instead of one
// object per bucket, since every bucket used to repeat the same handful of
// keys — measured 69 bytes/bucket as objects vs. 16 as columns on a real
// response, which is the difference between ~240 and ~1000 buckets under the
// 16 KiB cap. "t0"/"dt" let the model reconstruct t_i = t0 + i*dt; an
// explicit "t" array is emitted only when some bucket's actual t_rel_s
// deviates from that reconstruction by more than a tiny tolerance — either
// bucketize's "one bucket per sample" path over uneven timestamps, or a
// bucket dropped entirely because its window held no samples at all (both
// break the even grid the t0/dt formula assumes). A bucket with no finite
// samples reports null min/max/mean but keeps its 0 in "n", so a window of
// missing data is visible rather than silently absent from the arrays.
json renderBucketArray(std::span<const std::int64_t> ts, std::span<const double> vals, std::size_t max_points) {
  auto buckets = bucketize(ts, vals, max_points);
  json out = json::object();
  const std::size_t n = buckets.size();
  const double t0 = n > 0 ? buckets.front().t_rel_s : 0.0;
  const double dt = n >= 2 ? (buckets.back().t_rel_s - t0) / static_cast<double>(n - 1) : 0.0;
  const double tol = 1e-6 * std::max(1.0, std::fabs(dt));

  bool irregular = false;
  for (std::size_t i = 0; i < n; ++i) {
    if (std::fabs(buckets[i].t_rel_s - (t0 + static_cast<double>(i) * dt)) > tol) {
      irregular = true;
      break;
    }
  }

  json n_arr = json::array();
  json min_arr = json::array();
  json max_arr = json::array();
  json mean_arr = json::array();
  json invalid_arr = json::array();
  json t_arr = json::array();
  bool any_invalid = false;
  for (const auto& b : buckets) {
    n_arr.push_back(b.count);
    if (b.count > 0) {
      min_arr.push_back(roundSignificant(b.min));
      max_arr.push_back(roundSignificant(b.max));
      mean_arr.push_back(roundSignificant(b.mean));
    } else {
      min_arr.push_back(nullptr);
      max_arr.push_back(nullptr);
      mean_arr.push_back(nullptr);
    }
    invalid_arr.push_back(b.invalid);
    any_invalid = any_invalid || b.invalid > 0;
    if (irregular) {
      t_arr.push_back(b.t_rel_s);
    }
  }

  out["t0"] = t0;
  out["dt"] = dt;
  out["n"] = std::move(n_arr);
  out["min"] = std::move(min_arr);
  out["max"] = std::move(max_arr);
  out["mean"] = std::move(mean_arr);
  if (irregular) {
    out["t"] = std::move(t_arr);
  }
  if (any_invalid) {
    out["invalid"] = std::move(invalid_arr);
  }
  return out;
}

// Render a windowed, successfully-read series as mode 'raw' samples: once the
// model has narrowed to a handful of points, buckets can no longer show it
// the actual values, so this closes that last step -- but only inside a
// window the caller set, and only up to `sample_cap` of them (kMaxRawSamples,
// halved by the raw envelope's own shrink-to-fit when the batch is too big),
// so a raw call cannot become an unbounded dump of a whole recording. Times
// are seconds relative to the first RETURNED sample's OWN display time
// ("t0_display_s"), not to the window's t_start_s, mirroring how bucket "t0"
// is relative to the first bucket rather than to the window bound. Values
// are rounded the same way bucket min/max/mean are; a non-finite value comes
// back as JSON null rather than poisoning the array. A window holding more
// than `sample_cap` is truncated to its first `sample_cap` samples and
// flagged "truncated", with "window_samples" carrying the true count, so the
// model knows to narrow further rather than assume it saw everything.
json renderRawSamples(
    ToolContext& ctx, const PJ::sdk::CatalogSnapshot& catalog, const SeriesRead& r, std::size_t sample_cap) {
  const std::size_t window_samples = r.ts.size();
  const std::size_t count = std::min(window_samples, sample_cap);
  const std::int64_t t0_ns = r.ts.front();
  // The window's already-computed offset (display(ts) = ts*1e-9 + offset)
  // applies to any ns in this series, so reuse it instead of re-running the
  // ns->display conversion for this first sample; absent a window, fall back
  // to the call this always used to make.
  const std::optional<double> t0_display =
      r.display_offset_s ? std::optional<double>(static_cast<double>(t0_ns) * 1e-9 + *r.display_offset_s)
                         : toDisplaySeconds(ctx, catalog, r, t0_ns);
  if (!t0_display) {
    return {{"series", r.path}, {"error", "cannot map the display time of the first sample for this series"}};
  }

  json t_arr = json::array();
  json v_arr = json::array();
  for (std::size_t i = 0; i < count; ++i) {
    t_arr.push_back(static_cast<double>(r.ts[i] - t0_ns) * 1e-9);
    const double v = r.vals[i];
    v_arr.push_back(std::isfinite(v) ? json(roundSignificant(v)) : json(nullptr));
  }

  json out = {
      {"series", r.path}, {"t0_display_s", *t0_display}, {"t", std::move(t_arr)}, {"v", std::move(v_arr)},
      {"count", count},
  };
  if (count < window_samples) {
    out["truncated"] = true;
    out["window_samples"] = window_samples;
  }
  return out;
}

// Shrink `max_points` by half, re-rendering via `build`, until the serialized
// payload fits `cap` or `max_points` bottoms out at 16 -- the halving loop
// withCoarsenedBuckets and batchBuckets both ran themselves, over a
// single-series render and a whole batch envelope respectively. Adds a
// "note" to the last rendering when it still does not fit at the floor.
json shrinkToFit(const std::function<json(std::size_t)>& build, std::size_t cap, std::size_t max_points) {
  for (;;) {
    json out = build(max_points);
    const std::string dumped = out.dump();
    if (dumped.size() <= cap || max_points <= 16) {
      if (dumped.size() > cap) {
        out["note"] = "coarsened to fit the response cap";
      }
      return out;
    }
    max_points /= 2;
  }
}

// Coarsen until the serialized payload fits the response cap so spiky data
// stays representable without overrunning the model's context. Returns
// `base` with a "buckets" array attached (and a "note" when it still does not
// fit at the smallest allowed resolution). Shared by read_series's single-path
// 'buckets' mode and evaluate's optional bucket summary.
json withCoarsenedBuckets(
    json base, std::span<const std::int64_t> ts, std::span<const double> vals, std::size_t max_points) {
  return shrinkToFit(
      [&](std::size_t points) {
        base["buckets"] = renderBucketArray(ts, vals, points);
        return base;
      },
      kMaxResponseBytes, max_points);
}

// Several series' buckets, sharing ONE response cap instead of each getting
// the single-series kMaxResponseBytes on its own: min(N, 4) times that cap,
// halved in lockstep across every series until the whole envelope fits (or
// each is down to 16 buckets — the same floor withCoarsenedBuckets uses). A
// bad path becomes an {"error"} entry beside the ones that worked, same as
// 'stats'. Stats do not depend on max_points, so they are computed once,
// outside the halving loop that only re-renders the bucket arrays.
json batchBuckets(ToolContext& ctx, const std::vector<SeriesRead>& reads, std::size_t max_points) {
  const std::size_t n = reads.size();
  const std::size_t cap = std::min(n, std::size_t{4}) * kMaxResponseBytes;

  std::vector<json> stats(n);
  std::size_t failed = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (reads[i].ok) {
      stats[i] = statsWithDisplayStart(computeStats(reads[i].ts, reads[i].vals), ctx, reads[i].topic, reads[i].flat);
    } else {
      ++failed;
    }
  }

  return shrinkToFit(
      [&](std::size_t points) {
        json read_arr = json::array();
        for (std::size_t i = 0; i < n; ++i) {
          const SeriesRead& r = reads[i];
          if (!r.ok) {
            read_arr.push_back({{"series", r.path}, {"error", r.error}});
            continue;
          }
          json entry = {{"series", r.path}, {"stats", stats[i]}, {"buckets", renderBucketArray(r.ts, r.vals, points)}};
          if (!r.window.is_null()) {
            entry["window"] = r.window;
          }
          read_arr.push_back(std::move(entry));
        }
        json out = {{"count", n}, {"read", read_arr}};
        if (failed != 0) {
          out["failed"] = failed;
        }
        return out;
      },
      cap, max_points);
}

// True for every primitive type read_series can plot as a numeric time
// series -- everything except the two kinds readSeriesDoubles already
// refuses (string, bool). Shared with the topic expansion below so "numeric"
// means the same thing in both places.
bool isNumericFieldType(PJ::PrimitiveType t) {
  return t != PJ::PrimitiveType::kString && t != PJ::PrimitiveType::kBool;
}

// True for a field that counts as one of a topic's "real" signals: numeric,
// and NOT header/*-style (a ROS header's stamp/seq are metadata, not
// something to plot). One predicate for every place that enumerates a
// topic's fields for read_series -- the bare-topic expansion
// (topicNumericFieldPaths) and the unread-fields count
// (topicNumericFieldNames) -- so a topic with a header spends its
// buckets/raw batch cap on its actual channels, not on header/stamp/*.
bool isSignalField(PJ::PrimitiveType type, std::string_view field_name) {
  if (!isNumericFieldType(type)) {
    return false;
  }
  while (!field_name.empty() && field_name.front() == '/') {
    field_name.remove_prefix(1);
  }
  return field_name.rfind("header/", 0) != 0;
}

// Outcome of trying to resolve a read_series path as a bare TOPIC (no
// field) -- used by expandRequestedPaths when a path fails to resolve as a
// series. Exact match only: unlike a series path there is no abbreviated
// form to fall back on, a topic is either named or it is not.
struct TopicLookup {
  std::optional<std::uint32_t> topic_index;  // into catalog.topics()
  std::vector<std::string> candidates;       // qualified names, set only when ambiguous
  bool ambiguous = false;
};

// Resolve one "topic" or "dataset:topic" path against the catalog: the
// topic-only analogue of resolveSeriesPath's qualifier handling, with the
// same dataset-qualifier rule (matched against the KNOWN source names,
// longest match wins, rather than parsed at ':'). Tolerates a leading '/'
// on either side, since topic names carry one by host convention but a
// model may or may not echo it back. An unqualified name that exists in
// several datasets is ambiguous, exactly like a series would be. Takes the
// caller's own datasetByTopicIndex result so a call site working through
// several topics builds it once rather than once per topic.
TopicLookup resolveTopicPath(
    const PJ::sdk::CatalogSnapshot& catalog, const std::string& series, const TopicDatasetMap& topic_dataset) {
  TopicLookup out;
  auto topics = catalog.topics();

  const QualifierMatch qm = matchDatasetQualifier(catalog, series);
  std::string_view want = qm.bare;
  while (!want.empty() && want.front() == '/') {
    want.remove_prefix(1);
  }
  if (want.empty()) {
    return out;
  }

  auto qualifiedName = [&](std::uint32_t ti, std::string_view name) {
    const auto it = topic_dataset.find(ti);
    return qualifyWithDataset(it == topic_dataset.end() ? std::string() : it->second, std::string(name));
  };

  for (std::uint32_t ti = qm.topic_lo; ti < qm.topic_hi; ++ti) {
    std::string_view name = PJ::sdk::toStringView(topics[ti].name);
    std::string_view bare_name = name;
    while (!bare_name.empty() && bare_name.front() == '/') {
      bare_name.remove_prefix(1);
    }
    if (bare_name != want) {
      continue;
    }
    if (out.candidates.size() < kMaxCandidates) {
      out.candidates.push_back(qualifiedName(ti, name));
    }
    if (!out.topic_index) {
      out.topic_index = ti;
    } else {
      out.ambiguous = true;
    }
  }
  if (out.ambiguous) {
    out.topic_index.reset();
  } else {
    out.candidates.clear();
  }
  return out;
}

// Every numeric field of one already-resolved topic, as read_series paths
// in catalog order -- what a topic-only path expands to. Qualified with
// "dataset:" using the same rule resolveSeriesPath's own qualifying does, so
// an expanded path is never ambiguous even when several loaded datasets
// share this topic name. Takes the caller's own datasetByTopicIndex result,
// same reasoning as resolveTopicPath.
std::vector<std::string> topicNumericFieldPaths(
    const PJ::sdk::CatalogSnapshot& catalog, std::uint32_t topic_index, const TopicDatasetMap& topic_dataset) {
  std::vector<std::string> out;
  auto topics = catalog.topics();
  auto fields = catalog.fields();
  const auto& topic = topics[topic_index];
  const std::string topic_name(PJ::sdk::toStringView(topic.name));
  const auto dataset_it = topic_dataset.find(topic_index);
  const std::string dataset = dataset_it == topic_dataset.end() ? std::string() : dataset_it->second;
  for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
    const std::size_t idx = topic.first_field + fi;
    if (idx >= fields.size()) {
      break;
    }
    if (!isSignalField(PJ::sdk::fromAbiType(fields[idx].type), PJ::sdk::toStringView(fields[idx].name))) {
      continue;
    }
    out.push_back(qualifyWithDataset(dataset, joinSeriesPath(topic_name, PJ::sdk::toStringView(fields[idx].name))));
  }
  return out;
}

// Numeric field NAMES of one topic (relative to the topic, not full paths),
// excluding header/*-style fields -- timestamps and sequence numbers are not
// signals, so they count toward neither "numeric_fields" nor "fields" in the
// "unread" disclosure below. Field-name analogue of topicNumericFieldPaths,
// in the same catalog order.
std::vector<std::string> topicNumericFieldNames(const PJ::sdk::CatalogSnapshot& catalog, std::uint32_t topic_index) {
  std::vector<std::string> out;
  auto topics = catalog.topics();
  auto fields = catalog.fields();
  const auto& topic = topics[topic_index];
  for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
    const std::size_t idx = topic.first_field + fi;
    if (idx >= fields.size()) {
      break;
    }
    std::string_view name = PJ::sdk::toStringView(fields[idx].name);
    if (!isSignalField(PJ::sdk::fromAbiType(fields[idx].type), name)) {
      continue;
    }
    while (!name.empty() && name.front() == '/') {
      name.remove_prefix(1);
    }
    out.push_back(std::string(name));
  }
  return out;
}

// The field name a successful read actually covered, relative to its owning
// topic -- e.g. "channels[4]" out of a resolved path
// "/mavros/rc/out/channels[4]" -- recovered by stripping the same
// dataset-qualifier and topic prefix joinSeriesPath added, so it lines up
// with topicNumericFieldNames's entries below.
std::string fieldNameFromResolvedPath(const SeriesRead& r) {
  std::string path = r.path;
  if (!r.dataset.empty()) {
    const std::string prefix = r.dataset + ":";
    if (path.compare(0, prefix.size(), prefix) == 0) {
      path = path.substr(prefix.size());
    }
  }
  if (path.size() > r.topic.size() && path.compare(0, r.topic.size(), r.topic) == 0) {
    path = path.substr(r.topic.size());
  }
  while (!path.empty() && path.front() == '/') {
    path.erase(path.begin());
  }
  return path;
}

// Cap on how many unread field names the "unread" disclosure below names per
// topic -- fed back to the model and re-sent on every later round-trip of the
// turn, so an unbounded list would be paid for repeatedly. Same reasoning as
// kMaxCandidates.
constexpr std::size_t kMaxUnreadFields = 12;

// read_series discloses, per response, which topic(s) it read only PART of:
// grouped by owning topic, a topic where THIS call read fewer distinct
// numeric fields (excluding header/*) than it has gets a "read"/
// "numeric_fields" count and the unread field names, pointing at the
// bare-topic form that would have read all of them in one call (see the
// commit this shipped with: faults on unread fields went unnoticed because
// the model asked for only the first few of a wide array). Built once from
// the raw reads so every read_series mode attaches it the same way. Empty
// (no "unread" key at the call site) when every topic touched by this call
// was read whole.
json unreadDisclosure(
    const PJ::sdk::CatalogSnapshot& catalog, const std::vector<SeriesRead>& reads,
    const TopicDatasetMap& topic_dataset) {
  struct Tally {
    std::uint32_t topic_index = 0;
    std::vector<std::string> read_fields;
  };
  std::map<std::string, Tally> by_topic;  // keyed by the bare topic name read_series echoes as "topic"

  for (const auto& r : reads) {
    if (!r.ok) {
      continue;
    }
    auto it = by_topic.find(r.topic);
    if (it == by_topic.end()) {
      const std::string lookup_name = qualifyWithDataset(r.dataset, r.topic);
      const TopicLookup topic = resolveTopicPath(catalog, lookup_name, topic_dataset);
      if (!topic.topic_index) {
        continue;  // a series we just read always has an owning topic; defensive only
      }
      it = by_topic.emplace(r.topic, Tally{*topic.topic_index, {}}).first;
    }
    const std::string field = fieldNameFromResolvedPath(r);
    if (std::find(it->second.read_fields.begin(), it->second.read_fields.end(), field) ==
        it->second.read_fields.end()) {
      it->second.read_fields.push_back(field);
    }
  }

  json out = json::object();
  for (const auto& [topic_key, tally] : by_topic) {
    const std::vector<std::string> counted = topicNumericFieldNames(catalog, tally.topic_index);
    std::vector<std::string> unread_names;
    std::size_t read_count = 0;
    for (const auto& f : counted) {
      if (std::find(tally.read_fields.begin(), tally.read_fields.end(), f) != tally.read_fields.end()) {
        ++read_count;
      } else {
        unread_names.push_back(f);
      }
    }
    if (unread_names.empty()) {
      continue;
    }
    json fields_json = json::array();
    for (std::size_t i = 0; i < unread_names.size() && i < kMaxUnreadFields; ++i) {
      fields_json.push_back(unread_names[i]);
    }
    if (unread_names.size() > kMaxUnreadFields) {
      fields_json.push_back("…");
    }
    out[topic_key] = {
        {"read", read_count},
        {"numeric_fields", counted.size()},
        {"fields", std::move(fields_json)},
        {"hint", "a bare topic path reads every field of the topic in one call"},
    };
  }
  return out;
}

// How many of a topic's OWN numeric fields one read_series call may expand
// into before the mode's degradation kicks in (see expandRequestedPaths) --
// the same limit the mode already enforces on the whole call, so a topic
// that fits comfortably never trips a second, tighter limit later.
std::size_t topicExpansionCap(const std::string& mode) {
  if (mode == "buckets") {
    return kMaxBucketsBatchPaths;
  }
  if (mode == "raw") {
    return kMaxRawBatchPaths;
  }
  return kMaxBatchPaths;
}

// One item read_series works on after path expansion. `forced` bypasses the
// normal lookup+read entirely: it is set for an ambiguous topic, a topic
// with no numeric fields, or (mode buckets/raw) a topic too big for this
// call's cap -- cases expandRequestedPaths has already turned into a final
// answer, rendered by forcedFailure exactly like any other per-path error.
struct PathTask {
  std::string path;
  bool forced = false;
  std::string forced_error;
};

// A forced PathTask rendered as a SeriesRead failure, so every read_series
// mode's existing per-entry error rendering (built for a normal failed
// lookup) handles a topic-expansion error the same way, with no
// special-casing needed in the mode branches below.
SeriesRead forcedFailure(const PathTask& task) {
  SeriesRead r;
  r.path = task.path;
  r.error = task.forced_error;
  return r;
}

// Expand each requested path: unchanged when it already names a series (a
// field path always wins over a topic-only reading of the same string),
// and also unchanged when it names neither a series nor a topic -- the
// existing "unknown series" error fires later exactly as it did before this
// feature. Only a path that fails as a series AND succeeds as a bare topic
// expands, into every numeric field of that topic, in catalog order. A
// field a topic expands to is dropped when the caller also named it
// explicitly elsewhere in the same call, so the two never duplicate each
// other -- but two explicit requests for the same series are NOT
// deduplicated against each other, unchanged from before this feature
// (RawBatchCappedAtFour and friends rely on a repeated path still costing a
// slot against the batch cap). `notes_out` collects the "read the first N,
// cap" note mode 'stats' surfaces when a topic alone has more numeric
// fields than the cap; 'buckets' and 'raw' never populate it -- they fail
// that one task instead (see topicExpansionCap).
std::vector<PathTask> expandRequestedPaths(
    const PJ::sdk::CatalogSnapshot& catalog, const std::vector<std::string>& paths, const std::string& mode,
    std::vector<std::string>& notes_out, const TopicDatasetMap& topic_dataset) {
  std::vector<PathTask> out;
  out.reserve(paths.size());
  const std::size_t cap = topicExpansionCap(mode);

  // Resolve every requested path as a series exactly once -- reused below
  // both to seed explicit_canonical and, in the main loop, as `direct` --
  // rather than resolving each one twice (a pre-pass, then the loop).
  std::vector<SeriesLookup> direct_lookups;
  direct_lookups.reserve(paths.size());
  for (const auto& want : paths) {
    direct_lookups.push_back(resolveSeriesPath(catalog, want, topic_dataset));
  }

  // Canonical form of every path in THIS call that resolves directly as a
  // series, computed once up front (order-independent) so a topic expansion
  // can skip a field the caller also named explicitly, regardless of
  // whether that explicit path appears before or after the topic path.
  std::vector<std::string> explicit_canonical;
  for (const auto& lookup : direct_lookups) {
    if (lookup.resolved) {
      explicit_canonical.push_back(lookup.resolved->path);
    }
  }
  auto namedExplicitly = [&](const std::string& canon) {
    return std::find(explicit_canonical.begin(), explicit_canonical.end(), canon) != explicit_canonical.end();
  };
  std::vector<std::string> topic_expanded_seen;  // fields already produced by an earlier topic expansion this call

  for (std::size_t i = 0; i < paths.size(); ++i) {
    const std::string& want = paths[i];
    const SeriesLookup& direct = direct_lookups[i];
    if (direct.resolved) {
      PathTask task;
      task.path = want;
      out.push_back(std::move(task));
      continue;
    }

    const TopicLookup topic = resolveTopicPath(catalog, want, topic_dataset);
    if (topic.ambiguous) {
      SeriesLookup shim;
      shim.ambiguous = true;
      shim.candidates = topic.candidates;
      PathTask task;
      task.path = want;
      task.forced = true;
      task.forced_error = seriesLookupError(want, shim);
      out.push_back(std::move(task));
      continue;
    }
    if (!topic.topic_index) {
      // Not a topic either: pass through unchanged, same as before this
      // feature -- readOne will fail it with the usual "unknown series".
      PathTask task;
      task.path = want;
      out.push_back(std::move(task));
      continue;
    }

    std::vector<std::string> fields = topicNumericFieldPaths(catalog, *topic.topic_index, topic_dataset);
    const std::string topic_name(PJ::sdk::toStringView(catalog.topics()[*topic.topic_index].name));
    if (fields.empty()) {
      PathTask task;
      task.path = want;
      task.forced = true;
      task.forced_error = "topic has no numeric fields";
      out.push_back(std::move(task));
      continue;
    }
    if (fields.size() > cap) {
      if (mode == "stats") {
        notes_out.push_back(
            "topic " + topic_name + " has " + std::to_string(fields.size()) + " numeric fields; the first " +
            std::to_string(cap) + " were read (cap); name the fields to read the rest");
        fields.resize(cap);
      } else {
        PathTask task;
        task.path = want;
        task.forced = true;
        task.forced_error = "topic " + topic_name + " has " + std::to_string(fields.size()) +
                            " numeric fields, more than mode '" + mode + "' allows in one call (" +
                            std::to_string(cap) +
                            "); read it in stats first, then name the fields whose shape you want";
        out.push_back(std::move(task));
        continue;
      }
    }
    for (auto& f : fields) {
      if (namedExplicitly(f)) {
        continue;
      }
      if (std::find(topic_expanded_seen.begin(), topic_expanded_seen.end(), f) != topic_expanded_seen.end()) {
        continue;
      }
      topic_expanded_seen.push_back(f);
      PathTask task;
      task.path = std::move(f);
      out.push_back(std::move(task));
    }
  }
  return out;
}

// The batch-shaped {count, read, failed?, unread?, note?} envelope built
// from already-read series, regardless of how many there are -- render each
// into its entry JSON (a failed read becomes {"series", "error"};
// `render_entry` renders a successful one, and may itself add an "error" --
// raw's renderRawSamples does, when the display time cannot be mapped), and
// count the failures. `extra_note`, when non-empty, is stats' own extra: the
// topic-expansion-cap note. Split out from finishReadEnvelope below so raw's
// shrink-to-fit loop (which needs a plain json to measure) can call it
// directly, without going through the single-task unwrapping.
json buildReadEnvelope(
    const PJ::sdk::CatalogSnapshot& catalog, const std::vector<SeriesRead>& reads, const TopicDatasetMap& topic_dataset,
    const std::function<json(const SeriesRead&)>& render_entry, const std::string& extra_note = {}) {
  json arr = json::array();
  std::size_t failed = 0;
  for (const auto& r : reads) {
    json entry = r.ok ? render_entry(r) : json{{"series", r.path}, {"error", r.error}};
    if (entry.contains("error")) {
      ++failed;
    }
    arr.push_back(std::move(entry));
  }
  json out = {{"count", arr.size()}, {"read", arr}};
  if (failed != 0) {
    out["failed"] = failed;
  }
  const json unread = unreadDisclosure(catalog, reads, topic_dataset);
  if (!unread.empty()) {
    out["unread"] = unread;
  }
  if (!extra_note.empty()) {
    out["note"] = extra_note;
  }
  return out;
}

// Shared tail of read_series's per-task modes (stats, buckets' single-path,
// raw): buildReadEnvelope, then finish either as the one task's own JSON (an
// "error" there becomes a ToolResult::failure) or the batch envelope as-is.
ToolResult finishReadEnvelope(
    const PJ::sdk::CatalogSnapshot& catalog, const std::vector<SeriesRead>& reads, const TopicDatasetMap& topic_dataset,
    const std::function<json(const SeriesRead&)>& render_entry, const std::string& extra_note = {}) {
  const json out = buildReadEnvelope(catalog, reads, topic_dataset, render_entry, extra_note);
  // A lone path keeps the shape it has always had, so nothing that worked
  // before starts reading differently. A whole-topic path that expanded to
  // several series never takes this branch (reads.size() > 1 then).
  if (reads.size() == 1) {
    json only = out["read"].front();
    if (only.contains("error")) {
      return ToolResult::failure(only["error"].get<std::string>());
    }
    if (out.contains("unread")) {
      only["unread"] = out["unread"];
    }
    return ToolResult::success(only.dump());
  }
  return ToolResult::success(out.dump());
}

ToolResult readSeriesTool(const json& args, ToolContext& ctx) {
  const std::vector<std::string> paths = requestedPaths(args);
  if (paths.empty()) {
    return ToolResult::failure(
        "read_series requires 'paths': one topic/field path, or an array of them to read several in "
        "a single call");
  }
  const std::string mode = args.value("mode", std::string("stats"));

  // Optional display-axis window, shared by every path in this call (not
  // per-path — one read_series call reads one moment in time). Validated
  // once here so a backwards range fails the whole call instead of quietly
  // producing an empty-window error on every entry.
  std::optional<double> t_start_s;
  std::optional<double> t_end_s;
  if (args.contains("t_start_s") && !args["t_start_s"].is_null()) {
    t_start_s = args["t_start_s"].get<double>();
  }
  if (args.contains("t_end_s") && !args["t_end_s"].is_null()) {
    t_end_s = args["t_end_s"].get<double>();
  }
  if (t_start_s && t_end_s && *t_end_s <= *t_start_s) {
    return ToolResult::failure("t_end_s must be greater than t_start_s");
  }

  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }
  // Built once and threaded through every lookup this call makes (expansion,
  // resolution, the unread disclosure) instead of each one rebuilding it.
  const TopicDatasetMap topic_dataset = datasetByTopicIndex(*catalog);

  // A path naming a bare topic (no field) expands to every numeric field of
  // that topic here -- see expandRequestedPaths. This is where the caps
  // below start counting: a topic that names twelve fields costs twelve,
  // exactly as if the model had named them one by one.
  std::vector<std::string> topic_notes;
  const std::vector<PathTask> tasks = expandRequestedPaths(*catalog, paths, mode, topic_notes, topic_dataset);
  if (tasks.size() > kMaxBatchPaths) {
    return ToolResult::failure(
        "read_series takes at most " + std::to_string(kMaxBatchPaths) + " series per call, got " +
        std::to_string(tasks.size()) + " after expanding any whole-topic paths to their fields");
  }

  // readOne, then narrow to the window when one was requested — shared by
  // every mode below so 'stats' and 'buckets', single-path and batched, all
  // apply the same window the same way.
  auto readWindowed = [&](const std::string& want) {
    SeriesRead r = readOne(*catalog, ctx, want, topic_dataset);
    if (r.ok) {
      applyDisplayWindow(r, ctx, *catalog, t_start_s, t_end_s);
    }
    return r;
  };
  auto readTask = [&](const PathTask& task) { return task.forced ? forcedFailure(task) : readWindowed(task.path); };

  if (mode == "stats") {
    // One entry per requested path, each carrying its own error. A single typo
    // must not cost the whole round trip — which is the entire point of asking
    // for several at once.
    std::vector<SeriesRead> reads;
    reads.reserve(tasks.size());
    for (const auto& task : tasks) {
      reads.push_back(readTask(task));
    }
    std::string note;
    if (!topic_notes.empty()) {
      note = topic_notes.front();
      for (std::size_t i = 1; i < topic_notes.size(); ++i) {
        note += "; " + topic_notes[i];
      }
    }
    return finishReadEnvelope(
        *catalog, reads, topic_dataset,
        [&](const SeriesRead& r) {
          json entry = {
              {"series", r.path}, {"stats", statsWithDisplayStart(computeStats(r.ts, r.vals), ctx, r.topic, r.flat)}};
          if (!r.window.is_null()) {
            entry["window"] = r.window;
          }
          return entry;
        },
        note);
  }

  if (mode == "buckets") {
    // Several series' buckets share ONE response cap (batchBuckets), so the
    // batch is capped tighter than the flat kMaxBatchPaths above: past this
    // many, sharing the cap would coarsen every one of them past usefulness.
    if (tasks.size() > kMaxBucketsBatchPaths) {
      return ToolResult::failure(
          "mode 'buckets' takes at most " + std::to_string(kMaxBucketsBatchPaths) +
          " paths per call — they share one response cap. Batch 'stats' first, then ask for buckets on "
          "the series worth the shape.");
    }
    const std::size_t max_points = static_cast<std::size_t>(std::clamp(args.value("max_points", 200), 1, 500));
    // A lone path keeps the shape it has always had, at full single-series
    // resolution — the batch envelope (and its shared, smaller cap) only
    // kicks in once there is more than one series to fit together.
    if (tasks.size() == 1) {
      SeriesRead r = readTask(tasks.front());
      return finishReadEnvelope(*catalog, {r}, topic_dataset, [&](const SeriesRead& rr) {
        const json stats_json = statsWithDisplayStart(computeStats(rr.ts, rr.vals), ctx, rr.topic, rr.flat);
        json base = {{"series", rr.path}, {"stats", stats_json}};
        if (!rr.window.is_null()) {
          base["window"] = rr.window;
        }
        return withCoarsenedBuckets(std::move(base), rr.ts, rr.vals, max_points);
      });
    }
    std::vector<SeriesRead> reads;
    reads.reserve(tasks.size());
    for (const auto& task : tasks) {
      reads.push_back(readTask(task));
    }
    json out = batchBuckets(ctx, reads, max_points);
    const json unread = unreadDisclosure(*catalog, reads, topic_dataset);
    if (!unread.empty()) {
      out["unread"] = unread;
    }
    return ToolResult::success(out.dump());
  }

  if (mode == "raw") {
    // Raw returns actual values, not a summary, so an unbounded window would
    // let one call dump an entire recording — the window is mandatory rather
    // than optional-and-defaulting-to-everything the way it is for stats and
    // buckets.
    if (!t_start_s || !t_end_s) {
      return ToolResult::failure(
          "mode 'raw' needs t_start_s and t_end_s: it returns samples, so the window is not "
          "optional");
    }
    // Each series can carry up to kMaxRawSamples individual numbers here,
    // not a bounded bucket count, so the batch cap is tighter than buckets'.
    if (tasks.size() > kMaxRawBatchPaths) {
      return ToolResult::failure(
          "mode 'raw' takes at most " + std::to_string(kMaxRawBatchPaths) +
          " paths per call — each can return up "
          "to " +
          std::to_string(kMaxRawSamples) +
          " samples, so more would blow the response cap. Narrow with 'buckets' "
          "or 'stats' first.");
    }
    std::vector<SeriesRead> reads;
    reads.reserve(tasks.size());
    for (const auto& task : tasks) {
      reads.push_back(readTask(task));
    }
    // The 4-paths/200-samples caps above are a guess, not a measurement: a
    // realistic batch of dataset-qualified paths at 200 samples each can
    // still overrun kMaxResponseBytes. Shrink the per-series sample count in
    // lockstep, same as buckets' batch envelope, until the whole response
    // fits (or every series is down to 16 samples).
    const json out = shrinkToFit(
        [&](std::size_t sample_cap) {
          return buildReadEnvelope(*catalog, reads, topic_dataset, [&](const SeriesRead& r) {
            return renderRawSamples(ctx, *catalog, r, sample_cap);
          });
        },
        kMaxResponseBytes, kMaxRawSamples);
    // A lone path keeps the shape it has always had, same as 'stats' and
    // single-path 'buckets': the batch envelope only kicks in once there is
    // more than one series to fit together.
    if (reads.size() == 1) {
      json only = out["read"].front();
      if (only.contains("error")) {
        return ToolResult::failure(only["error"].get<std::string>());
      }
      if (out.contains("unread")) {
        only["unread"] = out["unread"];
      }
      if (out.contains("note")) {
        only["note"] = out["note"];
      }
      return ToolResult::success(only.dump());
    }
    return ToolResult::success(out.dump());
  }
  return ToolResult::failure("unknown mode '" + mode + "' (use 'stats', 'buckets' or 'raw')");
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
      for (const auto& out : outputs) {
        if (id == out) {
          return ToolResult::failure(
              "'" + out +
              "' already exists — this assistant created it earlier in the session. Remove it first with "
              "remove_derived_series, or choose another name.");
        }
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
      if (auto blocked = hostCreateBlocker(*catalog, *lookup.resolved)) {
        return ToolResult::failure(*blocked);
      }
      in = lookup.resolved->host_path;
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
  const std::string validate_script =
      derived_recipes::buildTransformScript("__validate__", "__validate__", global, body, num_extra, "luau").script;
  if (auto v = ctx.dp.validateScript("transform", ctx.language, validate_script); !v) {
    return ToolResult::failure("invalid expression: " + v.error());
  }
  const std::string script = derived_recipes::buildTransformScript(name, name, global, body, num_extra, "luau").script;

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
  const ResolvedEvalInputs resolved = resolveEvalInputs(ctx, *v2, raw_inputs);
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
    const auto raw_start = toRawNs(ctx, *resolved.anchor_source, start_s);
    const auto raw_end = toRawNs(ctx, *resolved.anchor_source, end_s);
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
    instant_ns = toRawNs(ctx, *resolved.anchor_source, args["at_s"].get<double>());
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
    instant_ns = toRawNs(ctx, *resolved.anchor_source, state->current_time_s);
    if (!instant_ns) {
      return ToolResult::failure("cannot convert the current playhead to raw time for this dataset");
    }
  }

  const std::string body = args["body"].get<std::string>();
  const std::string script = buildResolvedOnDemandChunk(body, resolved).script;
  if (auto v = ctx.dp.validateScript("on_demand", ctx.language, script); !v) {
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
  request.language = ctx.language;
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

  // Same resolution, host-create-blocker and join-forecast rules as
  // create_derived_series, and for the same reasons: an unresolved or
  // ambiguous input would install (briefly) a transform that computes
  // nothing meaningful, and inputs that share no timestamps join to zero
  // points either way.
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
    if (auto blocked = hostCreateBlocker(*catalog, *lookup.resolved)) {
      return ToolResult::failure(*blocked);
    }
    in = lookup.resolved->host_path;
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
  const std::string validate_script =
      derived_recipes::buildTransformScript("__validate__", "__validate__", global, body, num_extra, "luau").script;
  if (auto v = ctx.dp.validateScript("transform", ctx.language, validate_script); !v) {
    return ToolResult::failure("invalid expression: " + v.error());
  }
  const std::string script = derived_recipes::buildTransformScript(id, id, global, body, num_extra, "luau").script;

  const std::string output_name = id + "/value";
  std::vector<std::string_view> in_views(inputs.begin(), inputs.end());
  std::array<std::string_view, 1> out_views{output_name};
  auto created = ctx.dp.create(
      id, "transform", ctx.language, PJ::Span<const std::string_view>(in_views.data(), in_views.size()),
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
            "' already exists — this assistant created it earlier in the session. Remove it first with "
            "remove_derived_series, or choose another name.");
      }
    }
  }

  auto v2 = ctx.host.catalogSnapshotV2();
  if (!v2) {
    return ToolResult::failure(
        "create_derived_object requires catalog snapshot v2, which this host does not expose: " + v2.error());
  }
  const ResolvedEvalInputs resolved = resolveEvalInputs(ctx, *v2, raw_inputs);
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
    pin_ns = toRawNs(ctx, *resolved.anchor_source, args["pin_at_s"].get<double>());
    if (!pin_ns) {
      return ToolResult::failure(
          "cannot convert 'pin_at_s' to raw time: the host did not expose pj.playback.v1 or per-source time "
          "conversion");
    }
  }

  const std::string body = args["body"].get<std::string>();
  const std::string script = buildResolvedOnDemandChunk(body, resolved).script;
  if (auto v = ctx.dp.validateScript("on_demand", ctx.language, script); !v) {
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
        request.language = ctx.language;
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
      eval_ns = toRawNs(ctx, *resolved.anchor_source, state->current_time_s);
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
  if (comparison != ">" && comparison != "<" && comparison != ">=" && comparison != "<=") {
    return ToolResult::failure("'comparison' must be one of >, <, >=, <=");
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
        "this assistant did not create '" + name + "'" +
        (known.empty() ? "; it has created nothing in this session" : "; it has created: " + known) +
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
    // The two services are meant to be registered together, so this only fires
    // on a host that half-adopted them — worth saying plainly rather than
    // failing as if the tab were at fault.
    if (!ctx.viewport.valid()) {
      return ToolResult::failure(kNoViewport);
    }
    const bool has_start = args.contains("start_s") && args["start_s"].is_number();
    const bool has_end = args.contains("end_s") && args["end_s"].is_number();
    if (!has_start && !has_end) {
      if (auto status = ctx.viewport.zoomReset(); !status) {
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
    if (auto status = ctx.viewport.zoomToTimeRange(start_s, end_s); !status) {
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

}  // namespace

// --- catalog digest --------------------------------------------------------

namespace {

// How much of a topic's field list catalogDigest rendered, cheapest to
// costliest.
enum class Tier { kCount, kPartial, kFull };

// Plain truncation: the full tree, then (if that still does not fit) topic
// names only, with a footer naming the gap. This is the ENTIRE pre-mixed-tier
// catalogDigest, kept verbatim as the fallback for the rare budget so tight
// that not even a bare field COUNT fits for every topic — see catalogDigest's
// count-only feasibility check below, the only caller.
// Templated over the catalog type so it serves both the legacy (v1) host and
// catalogSnapshotV2()'s scalar half -- both expose the same topics()/fields()/
// dataSources() shape, and this fallback never touches object topics.
template <class Catalog>
std::string legacyCatalogDigest(const Catalog& catalog, std::size_t budget_chars) {
  auto topics = catalog.topics();
  auto fields = catalog.fields();
  const std::map<std::uint32_t, std::string> topic_dataset = datasetByTopicIndex(catalog.dataSources());

  auto build = [&](bool with_fields, std::size_t& shown) -> std::string {
    std::string body;
    shown = 0;
    std::string current_dataset;
    for (std::uint32_t ti = 0; ti < topics.size(); ++ti) {
      const auto& topic = topics[ti];
      std::string dataset_header;
      if (auto it = topic_dataset.find(ti); it != topic_dataset.end() && it->second != current_dataset) {
        current_dataset = it->second;
        dataset_header = "dataset \"" + current_dataset + "\":\n";
      }
      const auto topic_name = PJ::sdk::toStringView(topic.name);
      std::string line = dataset_header + "  " + std::string(topic_name);
      if (with_fields) {
        line += ": ";
        for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
          const std::size_t idx = topic.first_field + fi;
          if (idx >= fields.size()) {
            break;
          }
          if (fi != 0) {
            line += ", ";
          }
          line += std::string(PJ::sdk::toStringView(fields[idx].name));
          line += " (";
          line += primitiveTypeName(PJ::sdk::fromAbiType(fields[idx].type));
          line += ")";
        }
      }
      line += "\n";
      if (body.size() + line.size() > budget_chars) {
        break;
      }
      body += line;
      ++shown;
    }
    return body;
  };

  std::size_t shown = 0;
  std::string body = build(/*with_fields=*/true, shown);
  bool with_fields = true;
  if (shown < topics.size()) {
    // Didn't fit with fields — retry with topic names only, which is far
    // cheaper per topic and usually fits a dataset that the full tree can't.
    std::size_t shown_names = 0;
    std::string names_body = build(/*with_fields=*/false, shown_names);
    if (shown_names > shown) {
      body = std::move(names_body);
      shown = shown_names;
      with_fields = false;
    }
  }

  std::string header = "Loaded data (" + std::to_string(topics.size()) + " topic(s)";
  if (shown < topics.size()) {
    header += ", listing the first " + std::to_string(shown);
  }
  header += "):\n";
  if (catalog.dataSources().size() >= 2) {
    header +=
        "Several datasets are loaded; when the same topic exists in more than one, address the series as "
        "\"<dataset>:<topic>/<field>\".\n";
  }

  std::string footer;
  if (!with_fields) {
    footer += "Field names are omitted here — call describe_topic for a topic's fields.\n";
  }
  if (shown < topics.size()) {
    footer +=
        "This listing is TRUNCATED; topics not shown above still exist. Use list_topics with a filter to find "
        "them.\n";
  }
  return header + body + footer;
}

// The scalar half of the digest -- everything catalogDigest rendered before
// object topics existed, unchanged in behavior. Templated over the catalog
// type so catalogSnapshotV2()'s scalar arrays (identical shape to v1's) share
// this without a second copy; the caller has already handled "nothing is
// loaded" (that verdict needs to know about object topics too, on a v2 host).
template <class Catalog>
std::string renderScalarCatalogDigest(const Catalog& catalog, std::size_t budget_chars) {
  auto topics = catalog.topics();
  auto fields = catalog.fields();
  const std::uint32_t n = static_cast<std::uint32_t>(topics.size());

  // Which dataset each topic belongs to. PJ4 can hold several loaded at once —
  // two runs of the same robot, say — and a flat topic list hides that
  // completely: the model cannot offer to compare them because it does not know
  // there are two, and cannot avoid mixing them for the same reason. Topics are
  // grouped contiguously per source (first_topic/topic_count), so this is a
  // lookup table rather than a scan. Empty when the host reports no sources, in
  // which case the listing stays exactly as it was.
  const std::map<std::uint32_t, std::string> topic_dataset = datasetByTopicIndex(catalog.dataSources());

  // Dataset-header prefix per topic index — empty except where a new dataset
  // starts. Precomputed once so every tier below (full/partial/count) shares
  // it without re-deriving dataset transitions per topic.
  std::vector<std::string> dataset_prefix(n);
  {
    std::string current_dataset;
    for (std::uint32_t ti = 0; ti < n; ++ti) {
      if (auto it = topic_dataset.find(ti); it != topic_dataset.end() && it->second != current_dataset) {
        current_dataset = it->second;
        dataset_prefix[ti] = "dataset \"" + current_dataset + "\":\n";
      }
    }
  }

  // The dominant field type, when at least 80% of every field in the catalog
  // shares one: that type is then left off every "name (type)" rendering, and
  // only the minority still carries one — the header states the rule once
  // instead of repeating "(float64)" on thirty topics that are all float64.
  std::optional<std::string> dominant_type;
  {
    std::map<std::string, std::size_t> type_counts;
    std::size_t total_fields = 0;
    for (const auto& f : fields) {
      ++type_counts[primitiveTypeName(PJ::sdk::fromAbiType(f.type))];
      ++total_fields;
    }
    if (total_fields > 0) {
      const auto best = std::max_element(
          type_counts.begin(), type_counts.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
      if (static_cast<double>(best->second) >= 0.8 * static_cast<double>(total_fields)) {
        dominant_type = best->first;
      }
    }
  }
  auto renderField = [&](std::size_t idx) {
    std::string out(PJ::sdk::toStringView(fields[idx].name));
    const char* type_name = primitiveTypeName(PJ::sdk::fromAbiType(fields[idx].type));
    if (!dominant_type || *dominant_type != type_name) {
      out += " (";
      out += type_name;
      out += ")";
    }
    return out;
  };

  auto topicPrefix = [&](std::uint32_t ti) {
    return "  " + std::string(PJ::sdk::toStringView(topics[ti].name)) + ": ";
  };

  // Sub-budget for a partial line's field list (before the "  <topic>: "
  // prefix and the trailing "… +N more") — enough for a handful of short
  // field names at far less than a full line's cost.
  constexpr std::size_t kPartialFieldBudget = 120;

  auto buildFullBody = [&](std::uint32_t ti) {
    const auto& topic = topics[ti];
    std::string full = topicPrefix(ti);
    for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
      const std::size_t idx = topic.first_field + fi;
      if (idx >= fields.size()) {
        break;
      }
      if (fi != 0) {
        full += ", ";
      }
      full += renderField(idx);
    }
    return full + "\n";
  };
  auto buildCountBody = [&](std::uint32_t ti) {
    return topicPrefix(ti) + std::to_string(topics[ti].field_count) + " fields\n";
  };
  auto buildPartialBody = [&](std::uint32_t ti) {
    const auto& topic = topics[ti];
    const std::string prefix = topicPrefix(ti);
    std::string partial_fields;
    std::uint32_t partial_shown = 0;
    for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
      const std::size_t idx = topic.first_field + fi;
      if (idx >= fields.size()) {
        break;
      }
      const std::string candidate = (fi != 0 ? ", " : "") + renderField(idx);
      // Always take at least one field, even an oversized one, so a partial
      // line is never emptier than the count line it is meant to beat.
      if (partial_shown > 0 && partial_fields.size() + candidate.size() > kPartialFieldBudget) {
        break;
      }
      partial_fields += candidate;
      ++partial_shown;
    }
    const std::uint32_t remaining = topic.field_count - partial_shown;
    return remaining == 0 ? (prefix + partial_fields + "\n")
                          : (prefix + partial_fields + ", … +" + std::to_string(remaining) + " more\n");
  };

  auto renderHeader = [&] {
    std::string header = "Loaded data (" + std::to_string(n) + " topic(s)):\n";
    // Teach the qualifier by stating it where the dataset names are, instead
    // of spending schema tokens on it in every session: this line exists
    // only when several datasets are actually loaded.
    if (catalog.dataSources().size() >= 2) {
      header +=
          "Several datasets are loaded; when the same topic exists in more than one, address the series as "
          "\"<dataset>:<topic>/<field>\".\n";
    }
    if (dominant_type) {
      header += "fields are " + *dominant_type + " unless marked\n";
    }
    return header;
  };

  // Every topic's full rendering, built up front — the common case (the
  // whole catalog fits with every field spelled out) needs nothing else, so
  // the count and partial tiers below are built lazily, only once that fast
  // path has been ruled out.
  std::vector<std::string> full_body(n);
  for (std::uint32_t ti = 0; ti < n; ++ti) {
    full_body[ti] = buildFullBody(ti);
  }

  {
    std::size_t full_total = 0;
    for (std::uint32_t ti = 0; ti < n; ++ti) {
      full_total += dataset_prefix[ti].size() + full_body[ti].size();
    }
    if (full_total <= budget_chars) {
      std::string body;
      for (std::uint32_t ti = 0; ti < n; ++ti) {
        body += dataset_prefix[ti];
        body += full_body[ti];
      }
      return renderHeader() + body;
    }
  }

  // The full tree doesn't fit whole: every topic's cheapest rendering (a
  // bare field count) is the feasibility floor -- if even THAT doesn't fit,
  // there is nothing left to trim except fields entirely, which is exactly
  // what the pre-existing full-tree/names-only truncation already does.
  std::vector<std::string> count_body(n);
  for (std::uint32_t ti = 0; ti < n; ++ti) {
    count_body[ti] = buildCountBody(ti);
  }
  std::size_t count_total = 0;
  for (std::uint32_t ti = 0; ti < n; ++ti) {
    count_total += dataset_prefix[ti].size() + count_body[ti].size();
  }
  if (count_total > budget_chars) {
    return legacyCatalogDigest(catalog, budget_chars);
  }

  // Every topic starts at "count" (already paid for above) and ascends
  // cheapest-upgrade-first — (full size - count size), stable-sorted so a tie
  // resolves in catalog order — trying full, then partial, stopping the first
  // time an upgrade would blow the budget. Signed deltas: a topic with no
  // fields renders SHORTER as "full" than as "count", and that must read as
  // an improvement, not wrap around as an unsigned underflow. partial_body is
  // filled in lazily, only for the topics this loop actually considers one
  // for (a topic whose full upgrade already fit never needs it).
  std::vector<std::string> partial_body(n);
  std::vector<Tier> tier(n, Tier::kCount);
  {
    std::vector<std::uint32_t> order(n);
    for (std::uint32_t ti = 0; ti < n; ++ti) {
      order[ti] = ti;
    }
    auto delta = [](const std::string& upgraded, const std::string& base) {
      return static_cast<long long>(upgraded.size()) - static_cast<long long>(base.size());
    };
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
      return delta(full_body[a], count_body[a]) < delta(full_body[b], count_body[b]);
    });
    long long total = static_cast<long long>(count_total);
    const long long budget = static_cast<long long>(budget_chars);
    for (std::uint32_t ti : order) {
      const long long full_delta = delta(full_body[ti], count_body[ti]);
      if (total + full_delta <= budget) {
        total += full_delta;
        tier[ti] = Tier::kFull;
        continue;
      }
      partial_body[ti] = buildPartialBody(ti);
      const long long partial_delta = delta(partial_body[ti], count_body[ti]);
      if (total + partial_delta <= budget) {
        total += partial_delta;
        tier[ti] = Tier::kPartial;
      }
    }
  }

  std::string body;
  bool any_reduced = false;
  for (std::uint32_t ti = 0; ti < n; ++ti) {
    body += dataset_prefix[ti];
    switch (tier[ti]) {
      case Tier::kFull:
        body += full_body[ti];
        break;
      case Tier::kPartial:
        body += partial_body[ti];
        any_reduced = true;
        break;
      case Tier::kCount:
        body += count_body[ti];
        any_reduced = true;
        break;
    }
  }

  std::string footer;
  if (any_reduced) {
    footer +=
        "Some topics list only part of their fields (… +N more) or a field count: describe_topic gives the "
        "rest.\n";
  }
  return renderHeader() + body + footer;
}

// One line per non-marker object topic, grouped under the same "dataset
// \"X\":" headers the scalar half uses. Budgeted like a topic with one field:
// every line is a fixed-size unit -- there is no field list inside it to
// shrink -- so unlike a scalar topic it has only one tier, taken whole or
// left out. Lines are appended greedily in catalog order until the next one
// would exceed `budget_chars`; `shown < total` is catalogDigest's signal to
// say the list was cut.
struct ObjectDigestLines {
  std::string body;
  std::size_t shown = 0;
  std::size_t total = 0;
};

ObjectDigestLines renderObjectDigestLines(
    const PJ::sdk::CatalogSnapshotV2& catalog, std::size_t budget_chars, const PJ::sdk::PlaybackHostView& playback) {
  ObjectDigestLines out;
  const bool multi_dataset = catalog.dataSources().size() >= 2;
  std::string current_dataset;
  for (const auto& entry : derived_recipes::listObjectTopics(catalog)) {
    const PJ_object_topic_info_t& obj = entry.info;
    const std::string& name = entry.name;
    const std::string& dataset = entry.dataset;
    ++out.total;
    std::string line;
    if (multi_dataset && dataset != current_dataset) {
      current_dataset = dataset;
      line += "dataset \"" + current_dataset + "\":\n";
    }
    const std::string& type_name = entry.type;
    const ObjectTimeRange range = objectTimeRange(playback, obj.source, obj.time_min_ns, obj.time_max_ns);
    std::ostringstream oss;
    oss << "  " << name << "  [object " << (type_name.empty() ? std::string("unknown") : type_name) << ", "
        << obj.entry_count << " entries, " << std::fixed << std::setprecision(3) << range.a << "-" << range.b
        << (range.raw ? " raw seconds" : " display seconds") << "]\n";
    line += oss.str();
    if (out.body.size() + line.size() > budget_chars) {
      break;
    }
    out.body += line;
    ++out.shown;
  }
  return out;
}

}  // namespace

std::string catalogDigest(
    const PJ::sdk::ToolboxHostView& host, std::size_t budget_chars, const PJ::sdk::PlaybackHostView& playback) {
  auto v2 = host.catalogSnapshotV2();
  if (v2) {
    const ObjectDigestLines object_lines = renderObjectDigestLines(*v2, budget_chars, playback);
    if (v2->topics().empty() && object_lines.total == 0) {
      return "Loaded data: nothing is loaded yet.";
    }
    // Object topics are fixed-size lines with nothing to shrink, so they take
    // their share of the budget first; the scalar tiering algorithm gets
    // whatever is left, which is the whole budget on the common catalog with
    // no object topics -- every existing scalar guarantee is unchanged then.
    const std::size_t scalar_budget =
        budget_chars > object_lines.body.size() ? budget_chars - object_lines.body.size() : 0;
    std::string out = v2->topics().empty() ? std::string() : renderScalarCatalogDigest(*v2, scalar_budget);
    out += object_lines.body;
    if (object_lines.shown < object_lines.total) {
      out += "Some object topics are TRUNCATED above; use list_topics with a filter to find the rest.\n";
    }
    return out;
  }

  auto catalog = host.catalogSnapshot();
  if (!catalog) {
    return "Loaded data: unavailable (" + catalog.error() + "). Use list_topics to look it up.";
  }
  if (catalog->topics().empty()) {
    return "Loaded data: nothing is loaded yet.";
  }
  return renderScalarCatalogDigest(*catalog, budget_chars);
}

// --- registry --------------------------------------------------------------

void ToolRegistry::add(ToolSpec spec) {
  tools_.push_back(std::move(spec));
}

ToolRegistry::ToolRegistry() {
  using nlohmann::json;
  const json empty_obj = {{"type", "object"}, {"properties", json::object()}};

  add(
      {"list_topics",
       "Search the loaded topics by substring. You do NOT need this to find out what is loaded — "
       "that listing already arrives with every message. Call it only to look for something the "
       "listing does not show, e.g. when it was truncated or the data changed. Optional 'filter' "
       "and 'limit'.",
       {{"type", "object"},
        {"properties",
         {{"filter", {{"type", "string"}, {"description", "case-sensitive substring to match topic names"}}},
          {"dataset",
           {{"type", "string"},
            {"description", "only topics from this dataset (substring); useful when several are loaded"}}},
          {"limit", {{"type", "integer"}, {"description", "max topics to return (default 100, capped at 500)"}}}}}},
       &listTopics});

  add(
      {"describe_topic",
       "List the fields of one topic, with types and full paths. Only needed for a topic whose "
       "fields are NOT already in the listing you were given — do not use it to confirm a path you "
       "can already see.",
       {{"type", "object"},
        {"properties", {{"topic", {{"type", "string"}, {"description", "exact topic name"}}}}},
        {"required", json::array({"topic"})}},
       &describeTopic});

  add(
      {"read_series",
       "Read summary statistics ('stats'), a min/max-preserving downsample ('buckets'), or up to 200 "
       "raw samples inside a mandatory window ('raw'). Non-finite values are 'invalid', excluded "
       "from min/max/mean/stddev. buckets are columnar (t0, dt, n, min, max, mean; t_i = t0 + i*dt, "
       "or 't' when irregular). Stats carry 't_start_display_s' when playback is bound, so "
       "bucket i's display time = t_start_display_s + t0 + i*dt.\n"
       "'paths' is an ARRAY: ask several at once. A bad path returns as an error beside "
       "the ones that worked. A topic path (no field) reads all its numeric fields. buckets take "
       "up to 8 paths; raw takes 4. t_start_s/t_end_s (display axis) narrow the read and "
       "re-base t; raw REQUIRES both.",
       {{"type", "object"},
        {"properties",
         {{"paths",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "topic/field paths; a bare string is accepted for a single series"}}},
          {"mode", {{"type", "string"}, {"enum", json::array({"stats", "buckets", "raw"})}}},
          {"max_points", {{"type", "integer"}, {"description", "bucket count for mode=buckets (<=500)"}}},
          {"t_start_s", {{"type", "number"}, {"description", "display-axis window start (optional)"}}},
          {"t_end_s", {{"type", "number"}, {"description", "display-axis window end (optional)"}}}}},
        {"required", json::array({"paths"})}},
       &readSeriesTool});

  add(
      {"evaluate",
       "Run a Luau computation and get the answer back — nothing is created or shown. Picked "
       "automatically: SCALAR (default) takes the same inputs/expression/body/global as "
       "create_derived_series and returns stats, add 'buckets' for the shape. OBJECT form kicks in "
       "when an input is an object topic or 'at_s'/'window' is given: requires 'body' (reads "
       "inputs[\"<topic>\"], returns a table of 'outputs') and 'outputs' ([\"name:type\", …], type is "
       "'number', 'string', or a builtin object type e.g. 'kPointCloud'); 'at_s' evaluates at one "
       "display-seconds instant, 'window':{start_s,end_s} over a span, neither defaults to the "
       "playhead. Objects come back only as summaries, never bytes. To keep a result, "
       "create_derived_object.",
       {{"type", "object"},
        {"properties",
         {{"inputs", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"expression", {{"type", "string"}, {"description", "scalar only"}}},
          {"body",
           {{"type", "string"}, {"description", "scalar: return ...; object: reads inputs[..], returns table"}}},
          {"global", {{"type", "string"}, {"description", "scalar only"}}},
          {"buckets", {{"type", "integer"}, {"description", "scalar only; 1-500"}}},
          {"outputs", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "object only, required"}}},
          {"at_s", {{"type", "number"}, {"description", "object: instant, display seconds"}}},
          {"window",
           {{"type", "object"},
            {"properties", {{"start_s", {{"type", "number"}}}, {"end_s", {{"type", "number"}}}}},
            {"description", "object: span, display seconds"}}},
          {"budget_ms", {{"type", "integer"}, {"description", "object; default 1000 (the host may clamp)"}}},
          {"budget_evaluations", {{"type", "integer"}, {"description", "object; default 50 (the host may clamp)"}}},
          {"debug", {{"type", "boolean"}, {"description", "object: also report raw nanoseconds"}}}}},
        {"required", json::array({"inputs"})}},
       &evaluateSeries});

  add(
      {"create_derived_series",
       "Create a new derived timeseries computed live from one or more inputs. Each sample sees "
       "'value' (first input), 'v1'..'vN' (further inputs, in order), and 'time' (seconds). Use "
       "'expression' for a stateless formula (e.g. 'value * 2'); for stateful transforms like a "
       "derivative, use 'global' (runs once, persistent locals) + 'body' (statements ending in "
       "return; return nothing to suppress a sample). Saved in layouts. Undo protection is "
       "verified when supported; failure is reported.",
       {{"type", "object"},
        {"properties",
         {{"name", {{"type", "string"}, {"description", "name of the new series"}}},
          {"inputs", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "topic/field paths"}}},
          {"outputs",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description",
             "OPTIONAL — several series from ONE node (default: just 'name'). The body then returns one value per "
             "output, in order: outputs ['roll','pitch','yaw'] with a body ending 'return r, p, y'. Prefer this over "
             "three nodes recomputing the same intermediate."}}},
          {"expression", {{"type", "string"}, {"description", "stateless Luau expression over value/v1../time"}}},
          {"body",
           {{"type", "string"}, {"description", "full Luau statements ending in return (overrides expression)"}}},
          {"global",
           {{"type", "string"}, {"description", "Luau run once per instance; locals persist across samples"}}}}},
        {"required", json::array({"name", "inputs"})}},
       &createDerivedSeries});

  add(
      {"create_derived_object",
       "Install a live on-demand computation over object topics (point clouds, scene entities...) — "
       "evaluated on request, not eagerly per sample like create_derived_series. 'body' reads "
       "inputs[\"<topic>\"] and returns a table of the declared 'outputs' ([\"name:type\", …]). Without "
       "'pin_at_s' it stays live for a later consumer (e.g. scene_view) to query; with 'pin_at_s' it is "
       "a FINDING, evaluated once at that display-seconds instant and kept (shown under "
       "<plugin>/<name>/…). Either way this call also evaluates it once itself and returns that first "
       "bundle. Saved in layouts. Undo protection is verified when supported; failure is reported.",
       {{"type", "object"},
        {"properties",
         {{"name", {{"type", "string"}, {"description", "name of the new node"}}},
          {"inputs", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "topic/object paths"}}},
          {"outputs",
           {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "[\"name:type\", …], required"}}},
          {"body", {{"type", "string"}, {"description", "reads inputs[..], returns table of 'outputs'"}}},
          {"params", {{"type", "object"}, {"description", "forwarded to the script (optional)"}}},
          {"pin_at_s", {{"type", "number"}, {"description", "OPTIONAL — pin as a finding, display seconds"}}}}},
        {"required", json::array({"name", "inputs", "outputs", "body"})}},
       &createDerivedObject});

  add(
      {"create_markers",
       "Create plot markers from a Luau rule you write (preferred), or from a simple threshold "
       "template. There is ONE assistant marker set: calling this again REPLACES it (use "
       "remove_markers to clear it). Saved in layouts. Undo protection is verified when supported; "
       "failure is reported.\n"
       "A condition true over a STRETCH of time wants ONE region per stretch "
       "(startMarker/closeMarker), never one line per matching sample. The call reports how many "
       "markers it produced and of which kind: past ~50, the shape is wrong — merge contiguous "
       "hits into regions or raise the threshold.\n"
       "A per-sample threshold on a raw high-rate signal (IMU and the like) marks vibration, not "
       "events: require the condition to hold for a minimum duration and cross-check against an "
       "independent slower signal before labeling.\n"
       "RAW FORM: 'inputs' (series the rule reads) + 'rule' (a whole Luau script, run once over "
       "the full series) + optional 'output' (series path the markers attach to; default = first "
       "input; \"__global__\" renders on every plot — only for events meaningful against ANY "
       "signal).\n"
       "Rule vocabulary:\n"
       "- series(\"topic/field\") -> accessor or nil: s:size(); s:at(i) 0-based -> {t, v} or nil; "
       "s:atTime(t) -> interpolated value. GetSeriesNames() -> declared input names. "
       "Timestamps t are int64 NANOSECONDS.\n"
       "- startMarker(t) then closeMarker(t2, opts) -> ONE shaded region per pair.\n"
       "- createVerticalMarker(t, opts); createPointMarker(t, y, opts); "
       "createHorizontalMarker(y, opts); createBandMarker(y_low, y_high, opts); "
       "createMarker(x?, y?, opts).\n"
       "- bandPower(s, f_lo_hz, f_hi_hz) -> summed FFT power for spectral rules.\n"
       "- opts (all optional): label, description, category, color \"#RRGGBB\", severity "
       "(\"info\"|\"warning\"|\"error\"|\"critical\"), status.\n"
       "TEMPLATE FORM (simple threshold): 'series' + 'comparison' (>, <, >=, <=) + 'threshold' + "
       "optional 'label' and 'style' (\"region\" default: one band per exceedance stretch; "
       "\"line\": one vertical line per matching sample).",
       {{"type", "object"},
        {"properties",
         {{"rule", {{"type", "string"}, {"description", "raw Luau marker script (raw form)"}}},
          {"inputs",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "topic/field paths the rule reads (raw form)"}}},
          {"output",
           {{"type", "string"},
            {"description",
             "series path the marker set renders on, or \"__global__\" (raw form, "
             "optional; default = first input)"}}},
          {"series", {{"type", "string"}, {"description", "topic/field path (template form)"}}},
          {"comparison", {{"type", "string"}, {"enum", json::array({">", "<", ">=", "<="})}}},
          {"threshold", {{"type", "number"}}},
          {"style", {{"type", "string"}, {"enum", json::array({"region", "line"})}}},
          {"label", {{"type", "string"}, {"description", "marker label (optional)"}}}}}},
       &createMarkers});

  add(
      {"remove_markers",
       "Remove the assistant-created marker set from all plots. Only affects markers this "
       "assistant created; cannot delete user data.",
       empty_obj, &removeMarkers});

  add(
      {"list_created",
       "List the derived series and marker sets THIS assistant has created in this session. Use it "
       "to find the name to remove. A name that is already taken is refused by create_derived_series "
       "itself, so there is no need to call this first.",
       empty_obj, &listCreated});

  add(
      {"remove_derived_series",
       "Delete a derived series or object this assistant created, by its name. Only its own creations — "
       "loaded data cannot be touched. Use it to withdraw a series or object that turned out wrong "
       "instead of leaving it in the user's panel.",
       {{"type", "object"},
        {"properties", {{"name", {{"type", "string"}, {"description", "name given at creation"}}}}},
        {"required", json::array({"name"})}},
       &removeDerivedSeries});

  add(
      {"playback",
       "Drive the transport: the time cursor, shared by every plot. action: 'state' (read it), "
       "'play', 'pause', 'seek' (to 'time_s'), 'rate' (to 'rate'; 1.0 = real time, clamped to "
       "[0.05, 20]). Every call returns the whole state - playing, current_time_s, range "
       "{min_s, max_s}, rate - so a clamped seek shows where the cursor actually landed. Times are "
       "DISPLAY-AXIS SECONDS, the numbers on the plot X axes: a feature found in read_series "
       "buckets sits at stats.t_start_display_s + bucket.t.",
       {{"type", "object"},
        {"properties",
         {{"action", {{"type", "string"}, {"enum", json::array({"state", "play", "pause", "seek", "rate"})}}},
          {"time_s", {{"type", "number"}, {"description", "target time, display-axis seconds (seek)"}}},
          {"rate", {{"type", "number"}, {"description", "speed multiplier, > 0 (rate)"}}}}},
        {"required", json::array({"action"})}},
       &playbackTool});

  add(
      {"plot_tab",
       "Compose plot tabs of your own. A tab you create is marked with this assistant's ownership badge and is the "
       "only place "
       "you may draw: the user's tabs are not yours to fill, zoom or close, and they do not go away "
       "when you close yours. Supporting hosts save your tabs with the layout and exclude them from "
       "undo/redo; older hosts may keep them only for the session.\n"
       "action: 'create' (optional 'tab' name and 'title') | 'add'/'remove' ('curves', topic/field "
       "paths) | 'zoom' ('start_s'..'end_s' display-axis seconds; omit both to fit) | 'close' | "
       "'list'. Every action answers with the tab as the host holds it, so a curve that did not "
       "land shows as missing instead of being reported as drawn. A series you made with "
       "create_derived_series is addressable here as 'name/value'.",
       {{"type", "object"},
        {"properties",
         {{"action", {{"type", "string"}, {"enum", json::array({"create", "add", "remove", "zoom", "close", "list"})}}},
          {"tab", {{"type", "string"}, {"description", "your name for the tab"}}},
          {"title", {{"type", "string"}, {"description", "tab title shown to the user (create)"}}},
          {"curves", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "topic/field paths"}}},
          {"start_s", {{"type", "number"}}},
          {"end_s", {{"type", "number"}}}}},
        {"required", json::array({"action"})}},
       &plotTabTool});

  add(
      {"scene_view",
       "Open 3D/2D scene views of your own (the scene tabs of pj.plot_tabs.v1), the counterpart of plot_tab: same "
       "watermark and ownership, the user's scene docks unreachable.\n"
       "action: 'create' (optional 'view', 'kind' \"3d\"|\"2d\" default \"3d\", 'title') | "
       "'attach'/'detach' ('topics', object-topic paths) | 'focus' | 'close' | 'list'. Every action "
       "reads back what the view actually holds, so a topic that did not land shows as missing "
       "instead of attached.",
       {{"type", "object"},
        {"properties",
         {{"action",
           {{"type", "string"}, {"enum", json::array({"create", "attach", "detach", "focus", "close", "list"})}}},
          {"view", {{"type", "string"}, {"description", "your name for the view"}}},
          {"kind", {{"type", "string"}, {"enum", json::array({"3d", "2d"})}, {"description", "create only"}}},
          {"title", {{"type", "string"}, {"description", "view title shown to the user (create)"}}},
          {"topics", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "object-topic paths"}}}}},
        {"required", json::array({"action"})}},
       &sceneViewTool});

  add(
      {"report_status",
       "Counts of loaded sources/topics/fields. Only when the user asks for an overview — never as "
       "an opening step, since the listing you already have covers it.",
       empty_obj, &reportStatus});
}

const ToolSpec* ToolRegistry::find(std::string_view name) const {
  for (const auto& spec : tools_) {
    if (spec.name == name) {
      return &spec;
    }
  }
  return nullptr;
}

ToolResult ToolRegistry::execute(std::string_view name, const nlohmann::json& args, ToolContext& ctx) const {
  const ToolSpec* spec = find(name);
  if (spec == nullptr) {
    return ToolResult::failure("unknown tool '" + std::string(name) + "'");
  }
  // Choke point for the no-throw contract: executors use nlohmann typed getters
  // on MODEL-controlled arguments, and a wrong-typed value throws. The call
  // chain above this (GuiExecutor -> onTick -> plugin vtable) must never see an
  // exception, so convert anything thrown into a failure the model can react to.
  try {
    return spec->executor(args.is_object() ? args : nlohmann::json::object(), ctx);
  } catch (const std::exception& e) {
    return ToolResult::failure(std::string(name) + ": invalid arguments: " + e.what());
  }
}

nlohmann::json ToolRegistry::toFunctionSpecs() const {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& spec : tools_) {
    arr.push_back(
        {{"type", "function"},
         {"function", {{"name", spec.name}, {"description", spec.description}, {"parameters", spec.input_schema}}}});
  }
  return arr;
}

nlohmann::json ToolRegistry::toMcpToolsList() const {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& spec : tools_) {
    arr.push_back({{"name", spec.name}, {"description", spec.description}, {"inputSchema", spec.input_schema}});
  }
  return arr;
}

}  // namespace assistant_agent
