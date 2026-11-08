// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Private to pj_agent_tools (not installed): the helpers and types the tool
// executor translation units share, plus the executors tool_registry.cpp
// registers. Everything used by only one file stays in that file's anonymous
// namespace.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/type_tree.hpp>
#include <span>
#include <string>
#include <vector>

#include "pj_runtime/agent_tools/derived_recipes.hpp"
#include "pj_runtime/agent_tools/series_stats.hpp"
#include "pj_runtime/agent_tools/tool_context.hpp"

namespace PJ::agent_tools::detail {

using nlohmann::json;

// --- series reading (read_series.cpp; also derived_tools.cpp, view_tools.cpp) --

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

// Reads every input and counts the timestamps they all share. checked=false
// when any input fails to resolve or read.
JoinForecast forecastJoin(
    const ToolContext& ctx, const PJ::sdk::CatalogSnapshot& catalog, const std::vector<std::string>& inputs);

// Canonicalized paths from the first of `keys` that yields any, as a bare
// string or an array of strings (non-strings are skipped). `series` is an
// undocumented alias for `paths`.
std::vector<std::string> requestedPaths(
    const json& args, std::initializer_list<const char*> keys = {"paths", "series"});

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

// statsToJson plus best-effort display-time anchor (t_start_display_s) and the
// whole-series flat-run facts, each with its fixed interpretation note.
json statsWithDisplayStart(
    const SeriesStats& stats, ToolContext& ctx, const std::string& topic, const FlatRunSummary& flat);

// Resolves and reads `want` in full (no window); failure is reported in the
// result's ok/error, never thrown.
SeriesRead readOne(
    const PJ::sdk::CatalogSnapshot& catalog, ToolContext& ctx, const std::string& want,
    const TopicDatasetMap& topic_dataset);

// `base` with a "buckets" array attached, coarsened until the payload fits the
// response cap (plus a "note" when even the coarsest resolution does not fit).
json withCoarsenedBuckets(
    json base, std::span<const std::int64_t> ts, std::span<const double> vals, std::size_t max_points);

// --- tool executors (registered by tool_registry.cpp) -----------------------
//
// Each is a pure function of (args, host views) and reports every failure as
// data (ok=false); none throws on its own, and ToolRegistry::execute catches a
// throwing JSON getter on model-supplied arguments.

// read_series.cpp
ToolResult readSeriesTool(const json& args, ToolContext& ctx);

// view_tools.cpp
ToolResult listTopics(const json& args, ToolContext& ctx);
ToolResult describeTopic(const json& args, ToolContext& ctx);
ToolResult playbackTool(const json& args, ToolContext& ctx);
ToolResult plotTabTool(const json& args, ToolContext& ctx);
ToolResult sceneViewTool(const json& args, ToolContext& ctx);
ToolResult reportStatus(const json& args, ToolContext& ctx);

// derived_tools.cpp
ToolResult createDerivedSeries(const json& args, ToolContext& ctx);
ToolResult evaluateSeries(const json& args, ToolContext& ctx);
ToolResult createDerivedObject(const json& args, ToolContext& ctx);
ToolResult createMarkers(const json& args, ToolContext& ctx);
ToolResult removeMarkers(const json& args, ToolContext& ctx);
ToolResult listCreated(const json& args, ToolContext& ctx);
ToolResult removeDerivedSeries(const json& args, ToolContext& ctx);

}  // namespace PJ::agent_tools::detail
