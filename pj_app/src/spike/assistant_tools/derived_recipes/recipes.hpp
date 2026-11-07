// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

// Shared, UI-free helpers for creating persisted derived recipes (kind="transform" /
// "on_demand") through pj.data_processors.v1: catalog path resolution (scalar series and
// object topics, dataset qualifiers), input/output resolution, the generated Luau chunk, and
// the display<->raw time conversion. Used by the assistant's tools.
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

namespace derived_recipes {

// --- Luau text -------------------------------------------------------------

// Escape a string for embedding inside a double-quoted Luau or Python string literal (names, labels
// and series paths land verbatim in generated scripts): backslash, quote, line break and tab.
[[nodiscard]] std::string luaStringEscape(std::string_view s);

// Wrap a body into an on-demand Luau chunk (kind="on_demand"). `inputs` (a table keyed by each
// declared input's LITERAL name) and `params` (the node's params_json, decoded) are bound at
// the chunk's top via the vararg `...`; the body reads inputs["<topic>"] and returns a table
// of the declared outputs by name.
[[nodiscard]] std::string buildOnDemandChunk(const std::string& body);

// "    " in front of every line of `code` (an empty line stays empty); a blank body becomes `pass`.
// Python is whitespace-sensitive, so a body that becomes the inside of a `def` goes through this.
[[nodiscard]] std::string indentPython(const std::string& code);

// --- script layout ---------------------------------------------------------

// How many lines `text` takes when a line break follows it ("a\nb" -> 2, "" -> 1).
[[nodiscard]] int physicalLines(const std::string& text);

// Where the user's own code sits in a generated chunk, as 1-based line numbers of the chunk. A part
// that is absent has 0 lines.
struct ScriptLayout {
  int body_first_line = 0;
  int body_lines = 0;
  int globals_first_line = 0;
  int globals_lines = 0;
};

// A generated chunk and where the user's code is in it.
struct BuiltScript {
  std::string script;
  ScriptLayout layout;
};

// The per-sample transform chunk (Luau, or a Python module with a class T);
// `id` and `name` are escaped for the string literals they land in.
[[nodiscard]] BuiltScript buildTransformScript(
    const std::string& id, const std::string& name, const std::string& global_code, const std::string& body,
    std::size_t num_extra, std::string_view language = "luau");

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
[[nodiscard]] QualifierMatch matchDatasetQualifier(const PJ::sdk::CatalogSnapshot& catalog, std::string_view series);

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

// Who reads an error message: the assistant's model (told which of its tools to call next) or
// a person in a dialog (told what to do in the UI).
enum class Audience { kModel, kUser };

// The error a failed lookup should produce.
[[nodiscard]] std::string seriesLookupError(
    const std::string& series, const SeriesLookup& lookup, Audience audience = Audience::kModel);

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

// One non-marker object topic of the v2 catalog. `qualified` is the name to show or type back:
// "dataset:name" when several datasets are loaded, else the bare name. `dataset` is always the
// owning source's name. `info` is a copy of the catalog row (its string views stay valid while
// the snapshot lives).
struct ObjectTopicEntry {
  std::string name;
  std::string type;  // builtin object type name
  std::string dataset;
  std::string qualified;
  PJ_object_topic_info_t info{};
};
[[nodiscard]] std::vector<ObjectTopicEntry> listObjectTopics(const PJ::sdk::CatalogSnapshotV2& v2);

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
    PJ::sdk::ToolboxHostView& host, const PJ::sdk::CatalogSnapshotV2& v2, const std::vector<std::string>& raw_inputs,
    Audience audience = Audience::kModel);

// Build the on-demand chunk for resolved inputs: keeps both the requested and the historical
// bare script aliases without textual substitution in user code. `layout` says where `body` starts
// (only body_first_line and body_lines are set).
[[nodiscard]] BuiltScript buildResolvedOnDemandChunk(const std::string& body, const ResolvedEvalInputs& resolved);

// Python counterpart of buildResolvedOnDemandChunk (language="python"): a module whose top
// level defines `def evaluate(inputs, params):` (the contract of pj_scripting's
// python_object_script.h). The same alias table is rebuilt first, then `body` follows, indented
// one level.
[[nodiscard]] BuiltScript buildOnDemandChunkPython(const std::string& body, const ResolvedEvalInputs& resolved);

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

}  // namespace derived_recipes
