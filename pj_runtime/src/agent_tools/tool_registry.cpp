// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// ToolRegistry: the tool catalog the model is offered (names, descriptions,
// input schemas) and the no-throw dispatch to the executors.

#include "pj_runtime/agent_tools/tool_registry.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

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

// --- registry --------------------------------------------------------------

void ToolRegistry::add(ToolSpec spec) {
  tools_.push_back(std::move(spec));
}

ToolRegistry::ToolRegistry() {
  using nlohmann::json;
  const json empty_obj = {{"type", "object"}, {"properties", json::object()}};

  add(
      {"list_topics",
       "Search the loaded topics by substring. If the conversation already lists the loaded topics, "
       "use that and call this only for what the listing does not show (it was truncated, or the "
       "data changed); otherwise call it to find what is loaded. Optional 'filter' and 'limit'.",
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
       "List the fields of one topic, with types and full paths. Not needed for a topic whose "
       "fields the conversation already lists — do not use it to confirm a path you can already "
       "see.",
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
          {"mode", {{"type", "string"}, {"enum", json::array({"stats", "buckets", "raw"})}}},
          {"max_points", {{"type", "integer"}, {"description", "bucket count for mode=buckets (<=500)"}}},
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
          {"comparison", {{"type", "string"}, {"enum", json::array({">", "<", ">=", "<="})}}},
          {"threshold", {{"type", "number"}}},
          {"timeout_s", {{"type", "number"}, {"description", "default 60, at most 3600"}}}}},
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
       "template. There is ONE marker set of yours: calling this again REPLACES it (use "
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
       "Remove your marker set from all plots. Only affects markers you created; cannot delete "
       "user data.",
       empty_obj, &removeMarkers});

  add(
      {"list_created",
       "List the derived series and marker sets YOU have created in this session. Use it "
       "to find the name to remove. A name that is already taken is refused by create_derived_series "
       "itself, so there is no need to call this first.",
       empty_obj, &listCreated});

  add(
      {"remove_derived_series",
       "Delete a derived series or object you created, by its name. Only your own creations — "
       "loaded data cannot be touched. Use it to withdraw a series or object that turned out wrong "
       "instead of leaving it in the user's panel. Refused while someone else's series is built on it.",
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
       "Compose plot tabs of your own. A tab you create is marked with your ownership badge and is the only "
       "place you may draw: the user's tabs are not yours to fill, zoom or close, and they do not go away "
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
