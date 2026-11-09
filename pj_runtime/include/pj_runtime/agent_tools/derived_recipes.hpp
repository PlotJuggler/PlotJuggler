// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

// Shared, UI-free helpers for creating persisted derived recipes (kind="transform" /
// "on_demand") through pj.data_processors.v1: catalog path resolution (scalar series and
// object topics, dataset qualifiers), input/output resolution, the generated Luau chunk, and
// the display<->raw time conversion. Used by the agent tools and by pj_app's MCP tools.
//
// Nothing here owns host state: every function takes the host views it needs explicitly.

#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace PJ::agent_tools {

// --- Luau text -------------------------------------------------------------

// Escape a string for embedding inside a double-quoted Luau or Python string literal (names, labels
// and series paths land verbatim in generated scripts): backslash, quote, line break and tab.
[[nodiscard]] std::string luaStringEscape(std::string_view s);

// Wrap a body into an on-demand Luau chunk (kind="on_demand"). `inputs` (a table keyed by each
// declared input's LITERAL name) and `params` (the node's params_json, decoded) are bound at
// the chunk's top via the vararg `...`; the body reads inputs["<topic>"] and returns a table
// of the declared outputs by name.
[[nodiscard]] std::string buildOnDemandChunk(const std::string& body);

// The per-sample Luau transform chunk (a filter class T); `id` and `name` are escaped for the
// string literals they land in.
[[nodiscard]] std::string buildTransformScript(
    const std::string& id, const std::string& name, const std::string& global_code, const std::string& body,
    std::size_t num_extra);

// --- catalog helpers -------------------------------------------------------

// Cap on how many near misses an error names (the text is fed back to a model and re-sent on
// every later round trip, so an unbounded list would be paid for repeatedly).
inline constexpr std::size_t kMaxCandidates = 10;

// Dataset name for each topic INDEX, or empty when fewer than two data sources are loaded
// (one source adds no information).
using TopicDatasetMap = std::map<std::uint32_t, std::string>;
[[nodiscard]] TopicDatasetMap datasetByTopicIndex(std::span<const PJ_data_source_info_t> sources);
[[nodiscard]] TopicDatasetMap datasetByTopicIndex(const PJ::sdk::CatalogSnapshot& catalog);

// True for a marker set's own object topic ("__markers__/...") -- drawn, not read.
[[nodiscard]] bool isMarkerObjectTopic(std::string_view name);

// The dataset an object topic's `source` handle belongs to, or empty when it matches none.
[[nodiscard]] std::string objectTopicDatasetName(
    std::span<const PJ_data_source_info_t> sources, PJ_data_source_handle_t source);

// "dataset:name" when `dataset` is non-empty, else `name` unchanged.
[[nodiscard]] std::string qualifyWithDataset(const std::string& dataset, const std::string& name);

// A curve or topic path's dataset-qualifier prefix and the topic index range it narrows the
// search to. The qualifier is matched against the KNOWN source names (longest match wins).
struct QualifierMatch {
  std::string bare;
  std::uint32_t topic_lo = 0;
  std::uint32_t topic_hi = 0;
};
[[nodiscard]] QualifierMatch matchDatasetQualifier(
    std::span<const PJ_data_source_info_t> sources, std::size_t topic_count, std::string_view series);

// A field read_series counts among a topic's signals: numeric (not string or bool) and not a header/* field (a ROS
// header's stamp and seq are metadata). The one predicate every listing of a topic's fields uses.
[[nodiscard]] bool isSignalField(PJ::PrimitiveType type, std::string_view field_name);

// Join a topic name and a field path into the canonical curve path (tolerates a leading '/'
// on the field).
[[nodiscard]] std::string joinSeriesPath(std::string_view topic, std::string_view field);

// Collapse '/' runs in a series path.
[[nodiscard]] std::string canonicalSeriesPath(std::string_view s);

// A resolved "topic/field" curve path: the field handle plus the owning topic name. `path` is
// the canonical form the lookup settled on (carries the "dataset:" qualifier when several
// datasets are loaded); `host_path` is the bare topic/field form.
struct ResolvedSeries {
  PJ::sdk::FieldHandle handle;
  std::string topic;
  std::string path;
  std::string host_path;
  // Source name of the dataset this resolved to, empty when only one dataset is loaded.
  std::string dataset;
};

// Outcome of a path lookup. When nothing resolves, `candidates` carries the near misses.
struct SeriesLookup {
  std::optional<ResolvedSeries> resolved;
  std::vector<std::string> candidates;
  bool ambiguous = false;  // several paths matched; refusing to guess between them
};

// Resolve one curve path against the catalog. Accepts the host's "dataset:topic/field"
// qualifier; an unqualified path whose exact topic/field exists in several datasets is
// refused as ambiguous with the qualified candidates.
[[nodiscard]] SeriesLookup resolveSeriesPath(
    const PJ::sdk::CatalogSnapshot& catalog, const std::string& series, const TopicDatasetMap& topic_dataset);
[[nodiscard]] SeriesLookup resolveSeriesPath(const PJ::sdk::CatalogSnapshot& catalog, const std::string& series);

// A bare topic path ("topic" or "dataset:topic", no field) resolved against a catalog's topics.
struct TopicLookup {
  std::optional<std::uint32_t> topic_index;  // into the topics span
  std::vector<std::string> candidates;       // qualified names, set only when ambiguous
  bool ambiguous = false;
};
// Exact topic name, leading '/' optional; an unqualified name on several datasets is ambiguous, like a
// series. Takes the caller's datasetByTopicIndex so a caller resolving several topics builds it once.
[[nodiscard]] TopicLookup resolveTopicPath(
    std::span<const PJ_topic_info_t> topics, std::span<const PJ_data_source_info_t> sources, const std::string& series,
    const TopicDatasetMap& topic_dataset);

// The error a failed lookup should produce, worded for the model (names the tools to check with).
[[nodiscard]] std::string seriesLookupError(const std::string& series, const SeriesLookup& lookup);

// The DataSourceHandle a resolved series' dataset qualifies to. An empty name with exactly one
// loaded source is that source.
[[nodiscard]] std::optional<PJ::sdk::DataSourceHandle> dataSourceHandleFor(
    const PJ::sdk::CatalogSnapshot& catalog, const std::string& dataset_name);

// --- on-demand inputs / outputs -------------------------------------------

// One resolved input: either a scalar series or an object topic. `host_path` is the literal
// name the script's `inputs["<host_path>"]` addresses; `display_path` is what gets echoed
// back; `request_path` is the dataset-qualified ABI input. `source`/`has_source` carry the
// dataset this input resolved to, for the display<->raw time conversion.
struct ResolvedEvalInput {
  std::string host_path;
  std::string display_path;
  std::string request_path;
  std::vector<std::string> aliases;
  bool is_object = false;
  std::string object_type;
  PJ::sdk::DataSourceHandle source{};
  bool has_source = false;
  // The catalog row of an object input (zero for a scalar series): how many entries the topic holds
  // and the raw ns of the first one, so a caller needs no second scan of the catalog. `info` is the row
  // itself (its string views stay valid while the snapshot lives).
  std::uint64_t entry_count = 0;
  std::int64_t time_min_ns = 0;
  std::int64_t time_max_ns = 0;
  PJ_object_topic_info_t info{};
};

// Outcome of resolving one input against the object-topic half of the v2 catalog. An
// unqualified name that exists on several datasets is refused with the qualified candidates.
struct ObjectLookup {
  std::optional<ResolvedEvalInput> resolved;
  std::vector<std::string> candidates;
  bool ambiguous = false;
};

[[nodiscard]] ObjectLookup resolveObjectTopic(const PJ::sdk::CatalogSnapshotV2& v2, const std::string& want);
[[nodiscard]] std::string objectLookupError(const std::string& want, const ObjectLookup& lookup);

// Resolution result for a whole inputs array: each entry tried as an object topic first, then
// as a scalar series. Every input must share a dataset. `anchor_source` is the FIRST resolved
// input's dataset, used to anchor the display<->raw time conversion (see toRawNs). `error` is
// non-empty on any failure.
struct ResolvedEvalInputs {
  std::vector<ResolvedEvalInput> inputs;
  std::optional<PJ::sdk::DataSourceHandle> anchor_source;
  std::map<std::string, std::string> aliases;  // script key -> qualified ABI key
  std::string error;
};

[[nodiscard]] ResolvedEvalInputs resolveEvalInputs(
    PJ::sdk::ToolboxHostView& host, const PJ::sdk::CatalogSnapshotV2& v2, const std::vector<std::string>& raw_inputs);

// Build the on-demand Luau chunk for resolved inputs: keeps both the requested and the historical
// bare script aliases without textual substitution in user code.
[[nodiscard]] std::string buildResolvedOnDemandChunk(const std::string& body, const ResolvedEvalInputs& resolved);

// Declared outputs ("name:type" strings) split for DataProcessorRequest.outputs.
struct ParsedOutputs {
  std::vector<PJ::sdk::DataProcessorOutput> outputs;
  std::string error;
};
[[nodiscard]] ParsedOutputs parseTypedOutputs(const nlohmann::json& arr);

// Inverse of toDisplaySeconds: DISPLAY seconds -> raw dataset ns, from one forward conversion
// of raw 0. Empty when no playback view is bound or per-source conversion is unsupported.
[[nodiscard]] std::optional<std::int64_t> toRawNs(
    PJ::sdk::PlaybackHostView& playback, PJ::sdk::DataSourceHandle source, double display_s);

}  // namespace PJ::agent_tools
