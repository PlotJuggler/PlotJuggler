// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// ToolRegistry: the tool catalog the model is offered (names, descriptions, input schemas) and the no-throw dispatch
// to the executors.

#include "pj_runtime/agent_tools/tool_registry.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "pj_runtime/PlaybackRate.h"
#include "tools_internal.hpp"

namespace PJ::agent_tools {

using detail::createDerivedObject;
using detail::createDerivedSeries;
using detail::createMarkers;
using detail::describeTopic;
using detail::evaluateSeries;
using detail::listCreated;
using detail::listTopics;
using detail::playbackTool;
using detail::plotTabTool;
using detail::readSeriesTool;
using detail::removeDerivedSeries;
using detail::removeMarkers;
using detail::reportStatus;
using detail::sceneViewTool;
using detail::waitForTool;

void ToolRegistry::add(ToolSpec spec) {
  tools_.push_back(std::move(spec));
}

ToolRegistry::ToolRegistry() {
  using nlohmann::json;
  const json empty_obj = {{"type", "object"}, {"properties", json::object()}};

  add(
      {"list_topics",
       "Search the loaded topics by substring: for what the conversation does not already list (none listed, "
       "or loaded since).",
       {{"type", "object"},
        {"properties",
         {{"filter", {{"type", "string"}, {"description", "case-sensitive substring to match topic names"}}},
          {"dataset",
           {{"type", "string"},
            {"description", "only topics from this dataset (substring); useful when several are loaded"}}},
          {"limit",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 500},
            {"default", 100},
            {"description", "max topics to return"}}}}}},
       &listTopics});

  add(
      {"describe_topic",
       "List the fields of one topic, with types and full paths. Not needed for a topic whose fields the "
       "conversation already lists.",
       {{"type", "object"},
        {"properties",
         {{"topic", {{"type", "string"}, {"description", "exact topic name; \"dataset:topic\" picks one dataset"}}}}},
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
          {"mode",
           {{"type", "string"},
            {"enum", json::array({"stats", "buckets", "raw"})},
            {"default", "stats"},
            {"description", "what to read, as above"}}},
          {"max_points",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 500},
            {"default", 200},
            {"description", "bucket count for mode=buckets"}}},
          {"t_start_s", {{"type", "number"}, {"description", "display-axis window start (optional)"}}},
          {"t_end_s", {{"type", "number"}, {"description", "display-axis window end (optional)"}}}}},
        {"required", json::array({"paths"})}},
       &readSeriesTool});

  add(
      {"wait_for",
       "Wait for a live series to cross a threshold: returns the first sample that ARRIVES AFTER this call "
       "and satisfies 'comparison' against 'threshold' (its value and display time), or fails after "
       "'timeout_s'. For watching a stream; samples already loaded do not count (read_series finds those).",
       {{"type", "object"},
        {"properties",
         {{"series", {{"type", "string"}, {"description", "one topic/field path"}}},
          {"comparison",
           {{"type", "string"},
            {"enum", json::array({">", "<", ">=", "<="})},
            {"description", "sample <comparison> threshold"}}},
          {"threshold", {{"type", "number"}, {"description", "the value to compare each new sample with"}}},
          {"timeout_s",
           {{"type", "number"},
            {"minimum", 0},
            {"maximum", 3600},
            {"default", 60},
            {"description", "seconds to wait for it"}}}}},
        {"required", json::array({"series", "comparison", "threshold"})}},
       &waitForTool});

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
         {{"inputs",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "topic/field paths (scalar) or object-topic paths (object)"}}},
          {"expression", {{"type", "string"}, {"description", "scalar only"}}},
          {"body",
           {{"type", "string"}, {"description", "scalar: return ...; object: reads inputs[..], returns table"}}},
          {"global", {{"type", "string"}, {"description", "scalar only"}}},
          {"buckets", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}, {"description", "scalar only"}}},
          {"outputs", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "object only, required"}}},
          {"at_s", {{"type", "number"}, {"description", "object: instant, display seconds"}}},
          {"window",
           {{"type", "object"},
            {"properties", {{"start_s", {{"type", "number"}}}, {"end_s", {{"type", "number"}}}}},
            {"description", "object: span, display seconds"}}},
          {"budget_ms", {{"type", "integer"}, {"minimum", 1}, {"default", 1000}, {"description", "object only"}}},
          {"budget_evaluations",
           {{"type", "integer"}, {"minimum", 1}, {"default", 50}, {"description", "object only"}}},
          {"debug", {{"type", "boolean"}, {"description", "object: also report raw nanoseconds"}}}}},
        {"required", json::array({"inputs"})}},
       &evaluateSeries});

  add(
      {"create_derived_series",
       "Create a new derived timeseries computed live from one or more inputs. Each sample sees "
       "'value' (first input), 'v1'..'vN' (further inputs, in order), and 'time' (seconds). Use "
       "'expression' for a stateless formula (e.g. 'value * 2'); for stateful transforms like a "
       "derivative, use 'global' (runs once, persistent locals) + 'body' (statements ending in "
       "return; return nothing to suppress a sample). For Python or a script class: add_script_series; a "
       "built-in filter: add_filter (where listed). Saved in layouts. Undo protection is "
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
       "template. There is ONE marker set of yours: calling this again REPLACES it (use "
       "remove_markers to clear it). Saved in layouts; undo protection failures are reported.\n"
       "RAW FORM: 'inputs' (series the rule reads) + 'rule' (a Luau script, run once over the "
       "full series) + optional 'output' (series path the markers attach to; default = first "
       "input; \"__global__\" renders on every plot, only for events meaningful against ANY "
       "signal).\n"
       "Write rules with when(series, condition), not loops:\n"
       "  when(\"/speed\", above(2)):backgroundColor(\"red\", {label=\"fast\"}) -- shade while true\n"
       "  when(\"/acc\", outside(-3, 3)):event({label=\"overload\", minGap=1}) -- point where it starts\n"
       "  when(\"/mode\", changes()):verticalLine({label=function(v, prev) return prev..\"->\"..v end})\n"
       "  when(\"/mode\"):backgroundColor({[0]=\"gray\", [1]=\"green\"}) -- by value; or a function\n"
       "Conditions: above, below, between(a,b), outside(a,b), equals, isTrue(), changes(), or "
       "function(v, t, prev, prev_t) -> bool (t int64 ns; other series: s:atTime(t)). "
       "Marks fire once per stretch (every=true: per sample); minDuration / minGap (s) drop short "
       "regions / close marks: use them on raw high-rate (IMU) signals; cross-check a slower signal. "
       "The reply counts markers: past ~50 the rule is wrong.\n"
       "Loops only for windows/state: series(\"topic/field\") -> s:size(), s:at(i) 0-based {t, v}, "
       "s:atTime(t); startMarker(t) / closeMarker(t, opts); createVerticalMarker(t, opts), "
       "createPointMarker(t, y, opts), createHorizontalMarker(y, opts), createBandMarker(lo, hi, opts); "
       "bandPower(s, f_lo_hz, f_hi_hz); GetSeriesNames().\n"
       "opts: label, description, category, color (\"#RRGGBB\"|Qt name), severity "
       "(\"info\"|\"warning\"|\"error\"|\"critical\"), status.\n"
       "TEMPLATE FORM (writes a when() rule): 'series' + 'comparison' (>, <, >=, <=) + 'threshold' + "
       "optional 'label', 'style' (\"region\" default | \"line\": per matching sample).",
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
          {"comparison",
           {{"type", "string"},
            {"enum", json::array({">", "<", ">=", "<="})},
            {"description", "series <comparison> threshold (template form)"}}},
          {"threshold", {{"type", "number"}, {"description", "template form"}}},
          {"style",
           {{"type", "string"},
            {"enum", json::array({"region", "line"})},
            {"default", "region"},
            {"description", "template form"}}},
          {"label", {{"type", "string"}, {"description", "marker label (optional)"}}}}}},
       &createMarkers});

  add(
      {"remove_markers",
       "Remove your one marker set (create_markers), every marker of it, from all plots. Nothing else: not "
       "the user's markers, not your series.",
       empty_obj, &removeMarkers});

  add(
      {"list_created",
       "List what YOU created, by the id remove_derived_series takes: derived series and objects, "
       "add_script_series and add_filter series (non-alphanumeric bytes of the name as _xx), and "
       "\"assistant_markers\" (your markers).",
       empty_obj, &listCreated});

  add(
      {"remove_derived_series",
       "Delete one thing you created, by its list_created id (a derived series or object with its outputs, a "
       "script or filter series, your markers), never loaded data. Refused while someone else's series is "
       "built on it.",
       {{"type", "object"},
        {"properties", {{"name", {{"type", "string"}, {"description", "an id from list_created"}}}}},
        {"required", json::array({"name"})}},
       &removeDerivedSeries});

  add(
      {"playback",
       "Drive the transport: the time cursor, shared by every plot. action: 'state' (read it), "
       "'play', 'pause', 'seek' (to 'time_s'), 'rate' (to 'rate'; 1.0 = real time). Every call returns the whole state "
       "- playing, current_time_s, range "
       "{min_s, max_s}, rate - so a clamped seek shows where the cursor actually landed. Times are "
       "DISPLAY-AXIS SECONDS, the numbers on the plot X axes: a feature found in read_series "
       "buckets sits at stats.t_start_display_s + bucket.t.",
       {{"type", "object"},
        {"properties",
         {{"action",
           {{"type", "string"},
            {"enum", json::array({"state", "play", "pause", "seek", "rate"})},
            {"description", "see above"}}},
          {"time_s", {{"type", "number"}, {"description", "target time, display-axis seconds (seek)"}}},
          {"rate",
           {{"type", "number"},
            {"minimum", kMinPlaybackRate},
            {"maximum", kMaxPlaybackRate},
            {"description", "speed multiplier (rate)"}}}}},
        {"required", json::array({"action"})}},
       &playbackTool});

  add(
      {"plot_tab",
       "Compose plot tabs of your own. A tab you create is marked with your ownership badge and is the only "
       "place you may draw: the user's tabs are not yours to fill, zoom or close, and they do not go away "
       "when you close yours. Your tabs are saved with the layout and stay out of the user's undo/redo.\n"
       "action: 'create' (optional 'tab' name and 'title') | 'add'/'remove' ('curves', topic/field "
       "paths) | 'zoom' ('start_s'..'end_s' display-axis seconds; omit both to fit) | 'focus' (bring it "
       "forward, e.g. before a screenshot) | 'close' | 'list'. Every action answers with the tab as the host holds it, "
       "so a curve that did not "
       "land shows as missing instead of being reported as drawn. A series you made with "
       "create_derived_series is addressable here as 'name/value'.",
       {{"type", "object"},
        {"properties",
         {{"action",
           {{"type", "string"},
            {"enum", json::array({"create", "add", "remove", "zoom", "focus", "close", "list"})},
            {"description", "see above"}}},
          {"tab", {{"type", "string"}, {"description", "your name for the tab"}}},
          {"title", {{"type", "string"}, {"description", "tab title shown to the user (create)"}}},
          {"curves", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "topic/field paths"}}},
          {"start_s", {{"type", "number"}, {"description", "zoom start, display-axis seconds"}}},
          {"end_s", {{"type", "number"}, {"description", "zoom end, display-axis seconds"}}}}},
        {"required", json::array({"action"})}},
       &plotTabTool});

  add(
      {"scene_view",
       "Open 3D/2D scene views of your own, the counterpart of plot_tab: same watermark and ownership, the "
       "user's scene docks unreachable.\n"
       "action: 'create' (optional 'view', 'kind' \"3d\"|\"2d\" default \"3d\", 'title') | "
       "'attach'/'detach' ('topics', object-topic paths) | 'focus' | 'close' | 'list'. Every action "
       "reads back what the view actually holds, so a topic that did not land shows as missing "
       "instead of attached.",
       {{"type", "object"},
        {"properties",
         {{"action",
           {{"type", "string"},
            {"enum", json::array({"create", "attach", "detach", "focus", "close", "list"})},
            {"description", "see above"}}},
          {"view", {{"type", "string"}, {"description", "your name for the view"}}},
          {"kind", {{"type", "string"}, {"enum", json::array({"3d", "2d"})}, {"description", "create only"}}},
          {"title", {{"type", "string"}, {"description", "view title shown to the user (create)"}}},
          {"topics", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "object-topic paths"}}}}},
        {"required", json::array({"action"})}},
       &sceneViewTool});

  add(
      {"report_status",
       "Counts of loaded sources/topics/fields. Only when the user asks for an overview — not as "
       "an opening step: list_topics, or a listing already in the conversation, covers that.",
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
  // on MODEL-controlled arguments, and a wrong-typed value throws. The caller (a
  // Qt slot on the GUI thread) must never see an exception, so convert anything
  // thrown into a failure the model can react to.
  try {
    return spec->executor(args.is_object() ? args : nlohmann::json::object(), ctx);
  } catch (const std::exception& e) {
    return ToolResult::failure(std::string(name) + ": invalid arguments: " + e.what());
  }
}

nlohmann::json ToolRegistry::toMcpToolsList() const {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& spec : tools_) {
    arr.push_back({{"name", spec.name}, {"description", spec.description}, {"inputSchema", spec.input_schema}});
  }
  return arr;
}

}  // namespace PJ::agent_tools
