// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "pj_runtime/agent_tools/derived_recipes.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <pj_base/builtin/builtin_object.hpp>

namespace PJ::agent_tools {

std::string luaStringEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char ch : s) {
    if (ch == '\\' || ch == '"') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (ch == '\n') {
      out += "\\n";
    } else if (ch == '\r') {
      out += "\\r";
    } else if (ch == '\t') {
      out += "\\t";
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::string buildOnDemandChunk(const std::string& body) {
  std::string src = "-- pj-script: luau\n";
  src += "local inputs, params = ...\n";
  src += body + "\n";
  return src;
}

// Dataset name for each topic INDEX, or empty when the host reports no data
// sources (the SDK's test store is one such host, so every unit test exercises
// the degraded path). Topics are laid out contiguously per source, so this is a
// table build rather than a search.
//
// It matters because PJ4 can hold several datasets at once — two runs of the
// same robot is the ordinary case — and a flat topic list makes them
// indistinguishable. Without it the model can neither offer to compare two runs
// nor avoid mixing them, for the same reason: it does not know there are two.
std::map<std::uint32_t, std::string> datasetByTopicIndex(std::span<const PJ_data_source_info_t> sources) {
  std::map<std::uint32_t, std::string> out;
  if (sources.size() < 2) {
    return out;  // one source (or none) adds no information worth the tokens
  }
  for (const auto& src : sources) {
    const std::string name(PJ::sdk::toStringView(src.name));
    for (std::uint32_t i = 0; i < src.topic_count; ++i) {
      out[src.first_topic + i] = name;
    }
  }
  return out;
}

std::map<std::uint32_t, std::string> datasetByTopicIndex(const PJ::sdk::CatalogSnapshot& catalog) {
  return datasetByTopicIndex(catalog.dataSources());
}

// True for a marker set's own object topic ("__markers__/<series>" or
// "__markers__/__global__") -- excluded everywhere a model would otherwise
// see it as a readable object.
bool isMarkerObjectTopic(std::string_view name) {
  return name.substr(0, PJ::sdk::kMarkerObjectTopicPrefix.size()) == PJ::sdk::kMarkerObjectTopicPrefix;
}

// The dataset an object topic's `source` handle belongs to, or empty when it
// matches none (a host reporting no sources, same degraded case
// datasetByTopicIndex documents for scalar topics).
std::string objectTopicDatasetName(std::span<const PJ_data_source_info_t> sources, PJ_data_source_handle_t source) {
  for (const auto& src : sources) {
    if (src.handle.id == source.id) {
      return std::string(PJ::sdk::toStringView(src.name));
    }
  }
  return {};
}

// "dataset:name" when `dataset` is non-empty, else `name` unchanged — the
// host's qualifier convention, applied everywhere a catalog-derived name is
// handed back to the model.
std::string qualifyWithDataset(const std::string& dataset, const std::string& name) {
  return dataset.empty() ? name : dataset + ":" + name;
}

QualifierMatch matchDatasetQualifier(
    std::span<const PJ_data_source_info_t> sources, std::size_t topic_count, std::string_view series) {
  const auto topics_size = static_cast<std::uint32_t>(topic_count);
  QualifierMatch out{std::string(series), 0, topics_size};
  std::size_t qualifier_len = 0;
  for (const auto& src : sources) {
    const std::string name(PJ::sdk::toStringView(src.name));
    if (name.empty() || name.size() <= qualifier_len || series.size() <= name.size() || series[name.size()] != ':' ||
        series.compare(0, name.size(), name) != 0) {
      continue;
    }
    qualifier_len = name.size();
    out.topic_lo = src.first_topic;
    out.topic_hi = std::min(src.first_topic + src.topic_count, topics_size);
  }
  if (qualifier_len != 0) {
    out.bare = std::string(series.substr(qualifier_len + 1));
  }
  return out;
}

// A topic path: the topic-only analogue of resolveSeriesPath's qualifier handling, tolerating a
// leading '/' on either side (topic names carry one by host convention, a model may or may not
// echo it back).
TopicLookup resolveTopicPath(
    std::span<const PJ_topic_info_t> topics, std::span<const PJ_data_source_info_t> sources, const std::string& series,
    const TopicDatasetMap& topic_dataset) {
  TopicLookup out;
  const QualifierMatch qm = matchDatasetQualifier(sources, topics.size(), series);
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

bool isSignalField(PJ::PrimitiveType type, std::string_view field_name) {
  if (type == PJ::PrimitiveType::kString || type == PJ::PrimitiveType::kBool) {
    return false;
  }
  while (!field_name.empty() && field_name.front() == '/') {
    field_name.remove_prefix(1);
  }
  return field_name.rfind("header/", 0) != 0;
}

// Join a topic name and a field path into the canonical curve path. Hosts
// differ on whether leaf field names carry a leading '/' (the plot-markers
// host does, main does not), so tolerate both — a naive '+ "/" +' join emits
// "topic//field", which the marker engine's series() lookup rejects.
std::string joinSeriesPath(std::string_view topic, std::string_view field) {
  std::string path(topic);
  if (field.empty()) {
    return path;
  }
  if (field.front() != '/') {
    path.push_back('/');
  }
  path.append(field);
  return path;
}

// Collapse '/' runs in a model-supplied series path. The model echoes paths
// verbatim from earlier tool output (possibly from an older, doubling build),
// so accept "topic//field" as "topic/field" everywhere a path comes in.
std::string canonicalSeriesPath(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char ch : s) {
    if (ch == '/' && !out.empty() && out.back() == '/') {
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

namespace {

// Split a curve path into its '/'-separated segments, ignoring empty ones so
// leading or doubled slashes don't produce phantom segments.
std::vector<std::string_view> pathSegments(std::string_view path) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') {
      ++i;
    }
    const std::size_t start = i;
    while (i < path.size() && path[i] != '/') {
      ++i;
    }
    if (i > start) {
      out.push_back(path.substr(start, i - start));
    }
  }
  return out;
}

// True when `want`'s segments appear as a contiguous run inside `have`'s. This
// is what makes an abbreviated path resolvable: "test/sin" and "sin/value" and
// bare "sin" all match "test/sin/value". Matching whole segments (rather than
// substrings) is deliberate — "test/si" must not resolve to "test/sin/value".
bool segmentsContain(const std::vector<std::string_view>& have, const std::vector<std::string_view>& want) {
  if (want.empty() || want.size() > have.size()) {
    return false;
  }
  for (std::size_t off = 0; off + want.size() <= have.size(); ++off) {
    bool all = true;
    for (std::size_t k = 0; k < want.size(); ++k) {
      if (have[off + k] != want[k]) {
        all = false;
        break;
      }
    }
    if (all) {
      return true;
    }
  }
  return false;
}

}  // namespace

// Resolve one "topic/field" curve path (joinSeriesPath convention, the same the
// rest of PJ4 uses) by scanning the catalog — models call read_series
// repeatedly per turn, so avoid materializing a full path index.
//
// An exact match wins over abbreviations. An abbreviated path resolves only
// when exactly one series matches: with several the answer is the candidate
// list, never a guess, because silently picking one would attach a transform to
// the wrong signal and look like it worked.
//
// Datasets: the path may carry the host's qualifier convention,
// "dataset:topic/field". The qualifier is matched against the KNOWN source
// names (longest match wins) rather than parsed at ':', so a name like
// "[stream] UDP Server" needs no escaping. An unqualified path whose exact
// topic/field exists in SEVERAL datasets is refused as ambiguous — two runs of
// the same robot share every topic name, and silently taking the first-loaded
// one reads (or worse, installs onto) whichever file happened to load first.
// With several sources loaded, every path this returns is in qualified form, so
// results disclose which dataset they came from and candidates can be copied
// back verbatim.
SeriesLookup resolveSeriesPath(
    const PJ::sdk::CatalogSnapshot& catalog, const std::string& series, const TopicDatasetMap& topic_dataset) {
  SeriesLookup out;
  auto topics = catalog.topics();
  auto fields = catalog.fields();

  const QualifierMatch qm = matchDatasetQualifier(catalog.dataSources(), topics.size(), series);
  const std::string& bare = qm.bare;

  const auto want = pathSegments(bare);
  auto dataset_of = [&](std::uint32_t ti) {
    const auto it = topic_dataset.find(ti);
    return it == topic_dataset.end() ? std::string() : it->second;
  };
  auto qualified = [&](std::uint32_t ti, const std::string& full) { return qualifyWithDataset(dataset_of(ti), full); };

  std::optional<ResolvedSeries> exact;
  bool exact_ambiguous = false;
  std::vector<std::string> exact_candidates;
  std::optional<ResolvedSeries> fuzzy;
  for (std::uint32_t ti = qm.topic_lo; ti < qm.topic_hi; ++ti) {
    const auto& topic = topics[ti];
    const auto topic_name = PJ::sdk::toStringView(topic.name);
    for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
      const std::size_t idx = topic.first_field + fi;
      if (idx >= fields.size()) {
        break;
      }
      const std::string full = joinSeriesPath(topic_name, PJ::sdk::toStringView(fields[idx].name));
      if (full == bare) {
        if (!exact) {
          exact =
              ResolvedSeries{fields[idx].handle, std::string(topic_name), qualified(ti, full), full, dataset_of(ti)};
        } else {
          exact_ambiguous = true;
        }
        if (exact_candidates.size() < kMaxCandidates) {
          exact_candidates.push_back(qualified(ti, full));
        }
        continue;
      }
      if (segmentsContain(pathSegments(full), want)) {
        if (out.candidates.size() < kMaxCandidates) {
          out.candidates.push_back(qualified(ti, full));
        }
        if (!fuzzy) {
          fuzzy =
              ResolvedSeries{fields[idx].handle, std::string(topic_name), qualified(ti, full), full, dataset_of(ti)};
        } else {
          out.ambiguous = true;
        }
      }
    }
  }
  if (exact) {
    if (exact_ambiguous) {
      out.ambiguous = true;
      out.candidates = std::move(exact_candidates);
      return out;
    }
    out.resolved = std::move(exact);
    out.candidates.clear();
    out.ambiguous = false;
    return out;
  }
  if (!out.ambiguous && fuzzy) {
    out.resolved = std::move(fuzzy);
    out.candidates.clear();
  }
  return out;
}

SeriesLookup resolveSeriesPath(const PJ::sdk::CatalogSnapshot& catalog, const std::string& series) {
  return resolveSeriesPath(catalog, series, datasetByTopicIndex(catalog));
}

// The error a failed lookup should produce: self-contained, so the model can
// fix the path from the message alone.
std::string seriesLookupError(const std::string& series, const SeriesLookup& lookup) {
  if (lookup.ambiguous) {
    std::string msg = "'" + series + "' is ambiguous; it matches ";
    for (std::size_t i = 0; i < lookup.candidates.size(); ++i) {
      msg += (i != 0 ? ", " : "");
      msg += "'" + lookup.candidates[i] + "'";
    }
    msg += ". Use the full path.";
    return msg;
  }
  return "unknown series '" + series +
         "'. If you expected it to exist, call list_topics (with a filter) or describe_topic once to check before "
         "concluding it is missing.";
}

// The DataSourceHandle a resolved series' dataset qualifies to, for
// toDisplayTimeForSource — which needs a handle, while ResolvedSeries only
// carries the dataset's NAME (empty when at most one dataset is loaded, see
// tool_registry.hpp). Matches the name against the catalog's data sources; an
// empty name with exactly one loaded source is that source, unambiguously.
std::optional<PJ::sdk::DataSourceHandle> dataSourceHandleFor(
    const PJ::sdk::CatalogSnapshot& catalog, const std::string& dataset_name) {
  const auto sources = catalog.dataSources();
  if (!dataset_name.empty()) {
    for (const auto& src : sources) {
      if (PJ::sdk::toStringView(src.name) == dataset_name) {
        return src.handle;
      }
    }
    return std::nullopt;
  }
  if (sources.size() == 1) {
    return sources.front().handle;
  }
  return std::nullopt;
}

// Resolve one input against the object half of the v2 catalog: an optional
// "dataset:" qualifier (matched against known source names, longest match
// wins, same convention matchDatasetQualifier uses for scalar paths) narrows
// the search; the bare remainder is matched by exact name against
// objectTopics(), excluding marker topics (drawn, not read). Ambiguous only
// when an UNQUALIFIED bare name matches object topics on several datasets.
ObjectLookup resolveObjectTopic(const PJ::sdk::CatalogSnapshotV2& v2, const std::string& want) {
  ObjectLookup out;
  const auto sources = v2.dataSources();
  std::string bare = want;
  std::size_t qualifier_len = 0;
  PJ_data_source_handle_t qual_source{};
  bool has_qualifier = false;
  for (const auto& src : sources) {
    const std::string name(PJ::sdk::toStringView(src.name));
    if (name.empty() || name.size() <= qualifier_len || want.size() <= name.size() || want[name.size()] != ':' ||
        want.compare(0, name.size(), name) != 0) {
      continue;
    }
    qualifier_len = name.size();
    qual_source = src.handle;
    has_qualifier = true;
  }
  if (has_qualifier) {
    bare = want.substr(qualifier_len + 1);
  }

  std::optional<ResolvedEvalInput> found;
  bool ambiguous = false;
  for (const auto& obj : v2.objectTopics()) {
    const std::string name(PJ::sdk::toStringView(obj.name));
    if (isMarkerObjectTopic(name) || name != bare) {
      continue;
    }
    if (has_qualifier && obj.source.id != qual_source.id) {
      continue;
    }
    ResolvedEvalInput r;
    r.host_path = name;
    r.display_path = qualifyWithDataset(objectTopicDatasetName(sources, obj.source), name);
    r.is_object = true;
    r.object_type = std::string(PJ::sdk::toStringView(obj.builtin_object_type));
    r.source = obj.source;
    r.has_source = true;
    r.entry_count = obj.entry_count;
    r.time_min_ns = obj.time_min_ns;
    r.time_max_ns = obj.time_max_ns;
    r.info = obj;
    if (out.candidates.size() < kMaxCandidates) {
      out.candidates.push_back(r.display_path);
    }
    if (!found) {
      found = std::move(r);
    } else {
      ambiguous = true;
    }
  }
  if (ambiguous) {
    out.ambiguous = true;
    return out;
  }
  if (found) {
    out.resolved = std::move(found);
    out.candidates.clear();
  }
  return out;
}

std::string objectLookupError(const std::string& want, const ObjectLookup& lookup) {
  std::string msg = "'" + want + "' is ambiguous; it matches ";
  for (std::size_t i = 0; i < lookup.candidates.size(); ++i) {
    msg += (i != 0 ? ", " : "") + ("'" + lookup.candidates[i] + "'");
  }
  msg += ". Use the full path.";
  return msg;
}

ResolvedEvalInputs resolveEvalInputs(
    PJ::sdk::ToolboxHostView& host, const PJ::sdk::CatalogSnapshotV2& v2, const std::vector<std::string>& raw_inputs) {
  ResolvedEvalInputs out;
  for (const auto& in : raw_inputs) {
    ObjectLookup obj_lookup = resolveObjectTopic(v2, in);
    if (obj_lookup.ambiguous) {
      out.error = objectLookupError(in, obj_lookup);
      return out;
    }
    if (obj_lookup.resolved) {
      if (!out.anchor_source) {
        out.anchor_source = obj_lookup.resolved->source;
      }
      obj_lookup.resolved->aliases = {in, obj_lookup.resolved->host_path};
      out.inputs.push_back(std::move(*obj_lookup.resolved));
      continue;
    }
    auto catalog = host.catalogSnapshot();
    if (!catalog) {
      out.error = "catalog unavailable: " + catalog.error();
      return out;
    }
    auto lookup = resolveSeriesPath(*catalog, in);
    if (!lookup.resolved) {
      out.error = seriesLookupError(in, lookup);
      return out;
    }
    ResolvedEvalInput r;
    r.host_path = lookup.resolved->host_path;
    r.display_path = lookup.resolved->path;
    r.aliases = {in, r.host_path};
    r.is_object = false;
    if (auto handle = dataSourceHandleFor(*catalog, lookup.resolved->dataset)) {
      r.source = *handle;
      r.has_source = true;
      if (!out.anchor_source) {
        out.anchor_source = *handle;
      }
    }
    out.inputs.push_back(std::move(r));
  }
  for (auto& input : out.inputs) {
    if (input.has_source && out.anchor_source && input.source.id != out.anchor_source->id) {
      out.error = "on-demand inputs must belong to one dataset; cannot combine different datasets";
      return out;
    }
    const std::string dataset = objectTopicDatasetName(v2.dataSources(), input.source);
    input.request_path = qualifyWithDataset(dataset, input.host_path);
    input.aliases.push_back(input.request_path);
    for (const auto& alias : input.aliases) {
      const auto [existing, inserted] = out.aliases.emplace(alias, input.request_path);
      if (!inserted && existing->second != input.request_path) {
        out.error = "ambiguous input alias '" + alias + "'; use distinct input paths";
        return out;
      }
    }
  }
  return out;
}

namespace {

// The table that rebuilds `inputs` under every alias a script may use, so a dataset-qualified key and
// the historical bare one both work without textual substitution in user code. Building a fresh table
// also avoids overwriting another input key.
std::string buildAliasTable(const ResolvedEvalInputs& resolved) {
  std::string table = "inputs = {\n";
  for (const auto& [alias, key] : resolved.aliases) {
    table += "[\"" + luaStringEscape(alias) + "\"] = inputs[\"" + luaStringEscape(key) + "\"],\n";
  }
  table += "}\n";
  return table;
}

}  // namespace

std::string buildResolvedOnDemandChunk(const std::string& body, const ResolvedEvalInputs& resolved) {
  return buildOnDemandChunk(buildAliasTable(resolved) + body);
}

// Build a complete self-describing Luau FILTER CLASS the host can compile and run
// live as a DerivedEngine node (via createTransform). The user's global code runs
// once per instance inside a factory closure, so its locals persist across calls
// (PJ3 global-variable semantics); the body becomes the per-sample function with
// `time`/`value` (and `v1..vN` for the additional sources) in scope.
//
// `num_extra` is the count of additional sources: the body function takes
// `(time, value, v1, …, v<num_extra>)`, matching the host's MIMO calculate contract
// (calculate(self, t, v, v1..vN-1) — see pj_scripting FILTER_CLASS.md). `:calculate`
// forwards its args with `...`, so any arity works; the named params just give the
// body the v1..vN identifiers. Output count is decided host-side by the `outputs`
// passed to createTransform — `:calculate` returns the body's results unchanged
// (MULTRET), so a body that `return`s M values feeds M output topics.
std::string buildTransformScript(
    const std::string& raw_id, const std::string& raw_name, const std::string& global_code, const std::string& body,
    std::size_t num_extra) {
  const std::string id = luaStringEscape(raw_id);
  const std::string name = luaStringEscape(raw_name);
  std::string params = "time, value";
  for (std::size_t k = 0; k < num_extra; ++k) {
    params += ", v" + std::to_string(k + 1);
  }
  // The header declares the backend; the host's inferTransformBackend reads it.
  std::string src = "-- pj-script: luau\n";
  src += "local function _pj_make()\n";
  src += global_code + "\n";
  src += "  return function(" + params + ")\n";
  src += body + "\n";
  src += "  end\n";
  src += "end\n";
  src += "local T = { id = \"" + id + "\", name = \"" + name + "\", output = \"double\" }\n";
  src += "T.__index = T\n";
  src += "function T.create(_) return setmetatable({ fn = _pj_make() }, T) end\n";
  src += "function T:calculate(t, v, ...) return self.fn(t, v, ...) end\n";
  src += "return T\n";
  return src;
}

ParsedOutputs parseTypedOutputs(const nlohmann::json& arr) {
  ParsedOutputs out;
  for (const auto& o : arr) {
    if (!o.is_string()) {
      out.error = "'outputs' entries must be strings \"name:type\"";
      return out;
    }
    const std::string spec = o.get<std::string>();
    const std::size_t colon = spec.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == spec.size()) {
      out.error =
          "'outputs' entries must be \"name:type\" (type is 'number', 'string', or a builtin object type "
          "like 'kPointCloud'), got '" +
          spec + "'";
      return out;
    }
    out.outputs.push_back({spec.substr(0, colon), spec.substr(colon + 1)});
  }
  if (out.outputs.empty()) {
    out.error = "'outputs' must declare at least one \"name:type\"";
  }
  return out;
}

// Inverse of toDisplaySeconds: DISPLAY seconds -> raw dataset ns. The SDK
// exposes only the forward direction (toDisplayTimeForSource); this derives
// the same per-source AFFINE offset applyDisplayWindow computes
// (display(raw) = raw*1e-9 + offset, a per-DATASET constant, never a
// per-sample fact) from one forward conversion of a known raw instant — raw
// 0 always qualifies, so no sample lookup is needed — then inverts it.
// Empty when no playback view is bound or the source-scoped conversion is
// not supported by this host.
std::optional<std::int64_t> toRawNs(
    PJ::sdk::PlaybackHostView& playback, PJ::sdk::DataSourceHandle source, double display_s) {
  if (!playback.valid()) {
    return std::nullopt;
  }
  const auto offset_s = playback.toDisplayTimeForSource(source, 0);
  if (!offset_s) {
    return std::nullopt;
  }
  const double raw_s = display_s - *offset_s;
  return static_cast<std::int64_t>(std::llround(raw_s * 1e9));
}

}  // namespace PJ::agent_tools
