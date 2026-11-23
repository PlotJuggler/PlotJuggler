// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "mcp/McpProtocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)
using namespace Qt::StringLiterals;

namespace PJ::mcp {

Q_LOGGING_CATEGORY(lcMcp, "pj.mcp")

namespace {

#ifdef PJ_TARGET_WASM
#define PJ_MCP_WHERE "in their web browser"
#else
#define PJ_MCP_WHERE "on their desktop"
#endif
/// What every client is told: the agent tools, which every token serves.
constexpr auto kAgentToolInstructions =
    "PlotJuggler is the user's time-series and 3D data viewer, running " PJ_MCP_WHERE
    ". Find topics with "
    "list_topics / describe_topic (exact paths; a unique suffix also resolves; \"dataset:topic/field\" picks "
    "one dataset). For numbers use read_series (statistics, min/max buckets, raw samples) and evaluate (a Luau "
    "computation over series or objects that leaves nothing behind) instead of decoding files yourself. "
    "create_derived_series, create_derived_object and create_markers install live results the user sees and "
    "keeps in their layout. plot_tab and scene_view draw only in your own tabs. playback moves the shared time cursor. "
    "If a tool says the user has a dialog open, wait and try "
    "again. A long call (a file import, an export, an object evaluation, a question to the user) that outlasts "
    "its wait_s returns a job_id and carries on: job_wait for its result, job_kill to stop it, jobs_list for "
    "yours; meanwhile other calls work.";
#undef PJ_MCP_WHERE
/// What a client with the app's own tools is told after kAgentToolInstructions.
#ifdef PJ_TARGET_WASM
// No export_series nor about in the browser (McpServerTools.cpp).
constexpr auto kAppToolInstructions =
    " Start with get_status (it also shows whether the user's dialog is still open); open_file loads any "
    "format PlotJuggler reads (the relay hands the page the file), start_stream starts a live source. "
    "add_script_series (Luau or Python) and add_filter install live derived series too. widget_state reads "
    "the settings of any dock, the user's too (a scene's point size, colours, camera; a plot's styles and "
    "ranges) with the values each one accepts, and set_widget_state changes them. Take a screenshot to "
    "check what the user sees.";
#else
constexpr auto kAppToolInstructions =
    " Start with get_status (it also shows whether the user's dialog is still open); open_file loads any "
    "format PlotJuggler reads, start_stream starts a live source. export_series gives aligned CSV for your own "
    "tools; add_script_series (Luau or Python) and add_filter install live derived series too. widget_state "
    "reads the settings of any dock, the user's too (a scene's point size, colours, camera; a plot's styles and "
    "ranges) with the values each one accepts, and set_widget_state changes them. Take a screenshot to "
    "check what the user sees. Read about before explaining how to do something in the "
    "PlotJuggler UI.";
#endif

}  // namespace

json schema(json properties, json required) {
  return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}};
}

const ToolHints* toolHints(std::string_view name) {
  struct Row {
    std::string_view name;
    ToolHints hints;
  };
  // {title, read_only, destructive, idempotent, open_world}. A read-only tool is never destructive
  // and always idempotent. destructive: the call can remove or overwrite state that existed before
  // it (the user's or the agent's own, a same-name replace included); additive and navigation calls
  // are not.
  static constexpr Row kRows[] = {
      // Agent tools (pj_agent_tools).
      {"list_topics", {"List topics", true, false, true, false}},
      {"describe_topic", {"Describe a topic", true, false, true, false}},
      {"read_series", {"Read series", true, false, true, false}},
      {"wait_for", {"Wait for a live value", true, false, true, false}},
      {"evaluate", {"Evaluate a computation", true, false, true, false}},
      {"create_derived_series", {"Create a derived series", false, false, true, false}},
      {"create_derived_object", {"Create a derived object", false, false, true, false}},
      {"create_markers", {"Create markers", false, true, true, false}},
      {"remove_markers", {"Remove your markers", false, true, true, false}},
      {"list_created", {"List what you created", true, false, true, false}},
      {"remove_derived_series", {"Remove a derived series", false, true, true, false}},
      {"playback", {"Playback", false, false, true, false}},
      {"plot_tab", {"Your plot tabs", false, true, false, false}},
      {"scene_view", {"Your scene views", false, true, false, false}},
      {"report_status", {"Loaded data counts", true, false, true, false}},
      // App tools (McpServerTools.cpp, McpWidgetState.cpp).
      {"get_status", {"PlotJuggler status", true, false, true, false}},
      {"about", {"About PlotJuggler", true, false, true, false}},
      {"open_file", {"Open a file", false, false, false, false}},
      {"unload_file", {"Unload a dataset", false, true, true, false}},
      {"reload_file", {"Reload a dataset", false, true, false, false}},
      {"start_stream", {"Start a live stream", false, false, false, true}},
      {"stop_stream", {"Stop a live stream", false, true, true, false}},
      {"export_series", {"Export series to a file", false, true, false, false}},
      {"add_script_series", {"Add a script series", false, true, true, false}},
      {"add_filter", {"Add a filtered series", false, true, true, false}},
      {"remove_script_series", {"Remove a script series", false, true, true, false}},
      {"screenshot", {"Screenshot", true, false, true, false}},
      {"widget_state", {"Read dock settings", true, false, true, false}},
      // Each call is a new undo step of the user's.
      {"set_widget_state", {"Change dock settings", false, true, false, false}},
      // Job tools (McpServer).
      {"jobs_list", {"List your jobs", true, false, true, false}},
      {"job_wait", {"Wait for a job", true, false, true, false}},
      {"job_kill", {"Stop a job", false, true, true, false}},
      // UI tools (McpUiTools): they act as the user, who can do anything.
      {"ui_tree", {"Read the UI", true, false, true, false}},
      {"ui_click", {"Click", false, true, false, false}},
      {"ui_drag", {"Drag", false, true, false, false}},
      {"ui_wheel", {"Mouse wheel", false, true, false, false}},
      {"ui_key", {"Press keys", false, true, false, false}},
      {"ui_type", {"Type text", false, true, false, false}},
      {"ui_plot", {"Read the plots", true, false, true, false}},
      // Bridge tools (McpBridge.cpp).
      {"list_instances", {"List PlotJuggler windows", true, false, true, false}},
      {"select_instance", {"Select a PlotJuggler window", false, false, true, false}},
      {"launch", {"Launch PlotJuggler", false, false, false, false}},
  };
  for (const Row& row : kRows) {
    if (row.name == name) {
      return &row.hints;
    }
  }
  return nullptr;
}

json toolEntry(const std::string& name, const std::string& description, json input_schema) {
  json entry = {{"name", name}, {"description", description}, {"inputSchema", std::move(input_schema)}};
  if (const ToolHints* hints = toolHints(name)) {
    entry["title"] = hints->title;
    entry["annotations"] = {
        {"title", hints->title},
        {"readOnlyHint", hints->read_only},
        {"destructiveHint", hints->destructive},
        {"idempotentHint", hints->idempotent},
        {"openWorldHint", hints->open_world}};
  }
  return entry;
}

json textResult(const std::string& text, bool is_error) {
  json result = {{"content", json::array({{{"type", "text"}, {"text", text}}})}};
  if (is_error) {
    result["isError"] = true;
  }
  return result;
}

std::string dumpJson(const json& value) {
  return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

json jsonResult(const json& payload) {
  return textResult(dumpJson(payload));
}

bool isReadOnlyTool(const std::string& name) {
  const ToolHints* hints = toolHints(name);
  return hints != nullptr && hints->read_only;
}

bool runsUnderUsersDialog(const std::string& name) {
  return isReadOnlyTool(name) || name == "job_kill" || isUiTool(name);
}

namespace {

bool isSecretKey(const std::string& key) {
  std::string folded;
  for (const unsigned char c : key) {
    if (c != '_' && c != '-') {
      folded.push_back(static_cast<char>(std::tolower(c)));
    }
  }
  static constexpr std::string_view kWords[] = {"password", "passwd",     "secret",    "token",
                                                "apikey",   "privatekey", "credential"};
  return std::any_of(std::begin(kWords), std::end(kWords), [&folded](std::string_view word) {
    return folded.find(word) != std::string::npos;
  });
}

void redactIn(json& value, bool secret) {
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      redactIn(it.value(), secret || isSecretKey(it.key()));
    }
  } else if (value.is_array()) {
    for (json& item : value) {
      redactIn(item, secret);
    }
  } else if (secret && value.is_string() && !value.get_ref<const std::string&>().empty()) {
    value = kRedacted;
  }
}

/// `value` without the keys naming a secret, at any depth.
json withoutSecrets(const json& value) {
  if (value.is_object()) {
    json out = json::object();
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (!isSecretKey(it.key())) {
        out[it.key()] = withoutSecrets(it.value());
      }
    }
    return out;
  }
  if (value.is_array()) {
    json out = json::array();
    for (const json& item : value) {
      out.push_back(withoutSecrets(item));
    }
    return out;
  }
  return value;
}

/// Fills each kRedacted in `given` from `saved` at the same key path; the first path with no saved string, or "".
std::string restoreIn(json& given, const json* saved, const std::string& path, bool& found) {
  if (given.is_object()) {
    for (auto it = given.begin(); it != given.end(); ++it) {
      const json* next =
          saved != nullptr && saved->is_object() && saved->contains(it.key()) ? &(*saved)[it.key()] : nullptr;
      if (std::string error = restoreIn(it.value(), next, path + "/" + it.key(), found); !error.empty()) {
        return error;
      }
    }
  } else if (given.is_array()) {
    for (std::size_t i = 0; i < given.size(); ++i) {
      const json* next = saved != nullptr && saved->is_array() && i < saved->size() ? &(*saved)[i] : nullptr;
      if (std::string error = restoreIn(given[i], next, path + "/" + std::to_string(i), found); !error.empty()) {
        return error;
      }
    }
  } else if (given.is_string() && given.get_ref<const std::string&>() == kRedacted) {
    found = true;
    if (saved == nullptr || !saved->is_string()) {
      return path.empty() ? std::string("/") : path;
    }
    given = *saved;
  }
  return {};
}

}  // namespace

json redactSecrets(json config) {
  redactIn(config, false);
  return config;
}

std::string restoreSecrets(json& given, const json& saved) {
  json filled = given;
  bool found = false;
  if (const std::string missing = restoreIn(filled, &saved, "", found); !missing.empty()) {
    return "no saved value for " + missing + " to replace \"" + kRedacted + "\" with; pass the value itself";
  }
  if (!found) {
    return {};
  }
  // A saved secret goes only to what it was saved for: the same document, its secrets aside. Anything else (another
  // host, server or ICE entry) would carry it somewhere the user never sent it.
  const json changes = json::diff(withoutSecrets(saved), withoutSecrets(given));
  if (!changes.empty()) {
    std::set<std::string> paths;
    for (const json& change : changes) {
      const std::string path = change.value("path", "");
      paths.insert(path.empty() ? std::string("/") : path);
    }
    std::string named;
    for (const std::string& path : paths) {
      named += (named.empty() ? "" : ", ") + path;
    }
    return "this changes " + named + " while keeping a saved secret: pass the secret itself, or use ask_user=true";
  }
  given = std::move(filled);
  return {};
}

bool isJobCapableTool(const std::string& name) {
  static const std::set<std::string> job_capable = {"evaluate",     "create_derived_object", "wait_for",
                                                    "open_file",    "reload_file",           "unload_file",
                                                    "start_stream", "export_series"};
  return job_capable.contains(name);
}

bool isTasksMethod(const std::string& method) {
  return method == "tasks/get" || method == "tasks/update" || method == "tasks/cancel";
}

bool declaresTasks(const json& params) {
  const auto meta = params.find("_meta");
  if (meta == params.end() || !meta->is_object()) {
    return false;
  }
  const auto capabilities = meta->find("io.modelcontextprotocol/clientCapabilities");
  if (capabilities == meta->end() || !capabilities->is_object()) {
    return false;
  }
  const auto extensions = capabilities->find("extensions");
  return extensions != capabilities->end() && extensions->is_object() && extensions->contains(kTasksExtension);
}

double waitSeconds(const json& args, const char* key, double max) {
  const auto it = args.find(key);
  const double wait = it != args.end() && it->is_number() ? it->get<double>() : kDefaultWaitS;
  return std::isfinite(wait) ? std::clamp(wait, 0.0, max) : kDefaultWaitS;
}

namespace {

#ifdef PJ_TARGET_WASM
#define PJ_MCP_ABOUT_SOURCES ""
// A page's file has no saved configuration an agent's call can match, so no secret to keep.
#define PJ_MCP_OPEN_FILE_SECRETS "; pass a secret itself, never \"<redacted>\"."
#else
#define PJ_MCP_ABOUT_SOURCES "; about says what each one is"
#define PJ_MCP_OPEN_FILE_SECRETS                                                                               \
  "; a \"<redacted>\" passed back in config keeps the value this file was loaded with, only when the rest of " \
  "config is that one unchanged (else pass the secret itself)."
#endif

/// The tools/list entries of the app's own tools (McpServerTools.cpp).
json appToolEntries() {
  const json str = {{"type", "string"}};
  const json paths = {
      {"type", "array"},
      {"items", str},
      {"description", "series paths as read_series takes them: topic/field, a unique suffix, or dataset:topic/field"}};
  const json dataset = {{"type", {"string", "integer"}}, {"description", "dataset id, name or path (get_status)"}};
  const json ask_user = {{"type", "boolean"}, {"default", false}};
  const auto described = [](json property, const char* description) {
    property["description"] = description;
    return property;
  };
  return json::array({
      toolEntry(
          "get_status",
          "Start here. Loaded datasets (id, name, path, loader config; live streams carry stream), the stream "
          "sources start_stream can start, time range, current time, your agent id and tabs, whether "
          "PlotJuggler has the user's focus and any dialog the user has open. A config's secrets (passwords, "
          "tokens) read as \"<redacted>\".",
          schema(json::object())),
#ifndef PJ_TARGET_WASM
      toolEntry(
          "about",
          "What this PlotJuggler can do: built-in features with where to find them in the UI, known limitations, "
          "and the plugins that are loaded, installed or installable. Read it before telling the user how to do "
          "something in PlotJuggler. query keeps only the entries and plugins that mention it.",
          schema(
              {{"query",
                {{"type", "string"},
                 {"description",
                  "case-insensitive text to look for (e.g. \"csv\", \"zoom\"); without it, everything"}}}})),
#endif
      toolEntry(
          "open_file",
          "Load a data file (MCAP, CSV, ULog, Parquet...). Returns when the import has finished, or after wait_s "
          "with a job_id while a large file still loads: its topics are readable as they arrive, job_wait returns "
          "the outcome and job_kill stops the import. By default the loader's own defaults apply (e.g. every "
          "topic). Returns the config the loader used, its secrets as \"<redacted>\"" PJ_MCP_OPEN_FILE_SECRETS,
          schema(
              {{"path", {{"type", "string"}, {"description", "absolute path"}}},
               {"ask_user",
                described(ask_user, "open PlotJuggler's loader dialog so the user picks topics and options")},
               {"config",
                {{"type", "object"},
                 {"description",
                  "loader settings JSON; get_status shows the config each loaded dataset used, as a template"}}}},
              {"path"})),
      toolEntry(
          "unload_file",
          "Remove a loaded dataset and its data. If derived series depend on it, the user is asked to confirm in "
          "PlotJuggler and the call returns their answer (or a job_id after wait_s while they decide; job_kill "
          "withdraws the question).",
          schema({{"dataset", dataset}}, {"dataset"})),
      toolEntry(
          "reload_file", "Reload a dataset from its file with the same loader settings (e.g. after it changed).",
          schema({{"dataset", dataset}}, {"dataset"})),
      toolEntry(
          "start_stream",
          "Start a live streaming source (get_status lists stream_sources" PJ_MCP_ABOUT_SOURCES "). By "
          "default it uses the settings the user last confirmed in that source's dialog; config passes your own; "
          "ask_user=true opens the source's dialog so the user sets it up. Returns dataset_id once the stream "
          "runs, with the job_id of the job that lives as long as the stream: job_kill (or stop_stream) stops it, "
          "job_wait returns when it ends. Topics appear as the source advertises them. Configs come back with "
          "their secrets as \"<redacted>\"; a \"<redacted>\" passed back in config keeps the value the user "
          "confirmed in the dialog, only when the rest of config is that confirmed config unchanged (else pass the "
          "secret itself, or ask_user=true).",
          schema(
              {{"source", {{"type", "string"}, {"description", "stream source name, from stream_sources"}}},
               {"ask_user", described(ask_user, "open the source's dialog so the user sets it up")},
               {"config",
                {{"type", "object"},
                 {"description", "source settings JSON; get_status shows a live stream's config, as a template"}}}},
              {"source"})),
      toolEntry(
          "stop_stream", "Stop a live stream; its data stays loaded. dataset: a live stream from get_status.",
          schema({{"dataset", dataset}}, {"dataset"})),
#ifndef PJ_TARGET_WASM
      toolEntry(
          "export_series",
          "Write several series to ONE time-aligned file for your own analysis (numpy/pandas): fits, "
          "correlations, lags across signals. Works for every format PlotJuggler opened, so prefer it to "
          "decoding files yourself. Rows at every sample time of any series, or on a uniform rate_hz grid; "
          "values sample-and-hold; time in display seconds (as playback). Returns the path. format \"npy\" "
          "writes a numpy structured array that numpy.load reads in well under a second, where numpy.loadtxt "
          "takes seconds per million rows of CSV: use it beyond ~100k rows. A named path keeps a file already "
          "there unless overwrite is true.",
          schema(
              {{"paths", paths},
               {"path",
                {{"type", "string"},
                 {"description", "absolute path ending in .csv or .npy, as format (default: a new temp file)"}}},
               {"overwrite",
                {{"type", "boolean"}, {"default", false}, {"description", "replace a file already at path"}}},
               {"format",
                {{"type", "string"}, {"enum", {"csv", "npy"}}, {"default", "csv"}, {"description", "file format"}}},
               {"rate_hz",
                {{"type", "number"},
                 {"minimum", 0},
                 {"default", 0},
                 {"description", "rows on a uniform grid at this rate; 0: a row at every sample time of any series"}}},
               {"t0",
                {{"type", "number"}, {"description", "window start, display seconds (default: the first sample)"}}},
               {"t1", {{"type", "number"}, {"description", "window end, display seconds (default: the last sample)"}}}},
              {"paths"})),
#endif
      toolEntry(
          "add_script_series",
          "Create your own derived series with a Luau or Python script: for Python, or a script class that keeps "
          "state in its instance (create_derived_series takes a formula or a short Luau body; add_filter a "
          "built-in filter). Python: first line '# pj-script: python', "
          "one top-level class T (its id is set for you), @staticmethod create(params) returning T(), and "
          "calculate(self, time, value, *more_inputs); imports limited to math, cmath, statistics. A Luau script "
          "returns a class: return { name=\"X\", create = function(params) local s = {}; return { calculate = "
          "function(t, v) ... return y end } end } (the id is set for you). t is seconds, v the input; keep state "
          "in the instance (integrators, bias removal). Several inputs are joined on equal timestamps (fields of "
          "one message always are; inputs that share none are refused): calculate(t, v, v1, v2, ...). Several "
          "outputs: pass `outputs` [names] and return that many numbers; they become topics <name>/<output>. "
          "Listed under Custom Topics, saved in layouts, untouched by the user's undo; the same name replaces "
          "it. Returns each output topic with its fields.",
          schema(
              {{"name", described(str, "series name; your series of this name is replaced")},
               {"inputs", paths},
               {"outputs",
                {{"type", "array"},
                 {"items", str},
                 {"description", "output names, one number each from calculate (default: one output, `name`)"}}},
               {"script", described(str, "Luau or Python source, as above")},
               {"params", {{"type", "object"}, {"description", "passed to create(params)"}}}},
              {"name", "inputs", "script"})),
      toolEntry(
          "add_filter",
          "Create a derived series by applying one of PlotJuggler's built-in filters to one series; params "
          "override the filter's defaults (for a formula of your own use create_derived_series; for Python or a "
          "class with state, add_script_series). Like add_script_series: listed under Custom Topics, removable with "
          "remove_script_series, outside the user's undo, the same name replaces it. Returns the output topic and "
          "field to plot.",
          schema(
              {{"input", {{"type", "string"}, {"description", "series path, as read_series takes it"}}},
               {"filter",
                described(str, "built-in filter id; an unknown one is refused with the list of available filters")},
               {"params",
                {{"type", "object"}, {"description", "filter parameters; the ones left out take their defaults"}}},
               {"name", described(str, "output series name (default <input>[<filter>])")}},
              {"input", "filter"})),
      toolEntry(
          "remove_script_series",
          "Delete one of your series made with add_script_series or add_filter, all its outputs included, by the "
          "name it was created with. Refused while someone else's series (the user's, a plugin's) is built on it, "
          "since that would be deleted too.",
          schema({{"name", described(str, "the name given to add_script_series or add_filter")}}, {"name"})),
      toolEntry(
          "screenshot",
          "PNG of the PlotJuggler window as the user sees it, including GPU-rendered plots and 3D views, and any "
          "dialog the user has open. To show one of your tabs, focus it first with plot_tab or scene_view.",
          schema(
              {{"max_width",
                {{"type", "integer"},
                 {"default", kDefaultScreenshotWidth},
                 {"description", "scale down to at most this many pixels wide; 0: full size"}}}})),
      toolEntry(
          "widget_state",
          "Read the settings of any dock, the user's included: plots, 3D and 2D scenes, Log panels, state "
          "strips. A dock's settings are the XML element a layout saves for it (a scene's camera, grid and each "
          "layer's point size, colour mode, colormap, range...; a plot's curves, colours, styles and axis ranges); "
          "set_widget_state changes them.\n"
          "action 'list': every dock (id, tab, title, kind). 'get' (dock): its element as `xml`, and `options`: per "
          "element tag, each attribute the dock accepts with its type, allowed `values` or `min`/`max`, and "
          "`default` (the value an absent attribute takes); empty for a plugin's widget.",
          schema(
              {{"action",
                {{"type", "string"},
                 {"enum", json::array({"list", "get"})},
                 {"description", "list: every dock; get: one dock's settings and options"}}},
               {"dock", {{"type", "string"}, {"description", "get: a dock id from list"}}}},
              {"action"})),
      toolEntry(
          "set_widget_state",
          "Replace a dock's settings (widget_state reads them) with your XML element: get it right before, change "
          "attributes, send the whole element back with the same root tag; keep what you do not change as get "
          "returned it (it holds the camera and the view: an older get moves them back). A refused element leaves "
          "the dock as it was, and the error names the attribute and what it accepts; an attribute not in "
          "options, or a series or topic that is not loaded, is refused too; a dock whose get lists no options (a "
          "plugin's widget) takes any element, unchecked. Each call is one undo step for the user. Returns the "
          "dock's element as applied.",
          schema(
              {{"dock", {{"type", "string"}, {"description", "a dock id from widget_state list"}}},
               {"xml", {{"type", "string"}, {"description", "the dock's whole element"}}}},
              {"dock", "xml"})),
  });
}

}  // namespace

#undef PJ_MCP_ABOUT_SOURCES
#undef PJ_MCP_OPEN_FILE_SECRETS

namespace {

bool g_ui_tools_enabled = false;

/// The tools/list entries of the UI tools (McpUiTools).
json uiToolEntries() {
  const auto tool = [](const char* name, const std::string& desc, json input) {
    return toolEntry(name, desc, std::move(input));
  };
  const json target = {
      {"ref", {{"type", "string"}, {"description", "a ref from ui_tree, e.g. w12"}}},
      {"name", {{"type", "string"}, {"description", "or the objectName of exactly one visible widget"}}}};
  const json wait = {
      {"type", "integer"},
      {"minimum", 0},
      {"maximum", kMaxUiSettleMs},
      {"default", kDefaultUiSettleMs},
      {"description", "ms to let the app handle the input before answering"}};
  const json point = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}};
  const json button = {
      {"type", "string"}, {"enum", {"left", "right", "middle"}}, {"default", "left"}, {"description", "mouse button"}};
  const json x = {{"type", "number"}, {"description", "x in the target's pixels (default: its center)"}};
  const json y = {{"type", "number"}, {"description", "y in the target's pixels (default: its center)"}};
  const json mods = {{"type", "string"}, {"description", "held modifiers, e.g. Ctrl+Shift"}};
  const auto described = [](json property, const char* description) {
    property["description"] = description;
    return property;
  };
  const auto with = [&](json properties) {
    properties.update(target);
    properties["wait_ms"] = wait;
    return properties;
  };
  return json::array({
      tool(
          "ui_tree",
          "The visible windows as a user sees them, for driving the UI like a user (QA). One line per widget or "
          "item: [ref] Class#objectName \"text\" (flags) WxH; menu actions, tabs and the visible rows of lists and "
          "trees are items with their own ref. The open popup and the modal dialog come first; the windows a "
          "modal dialog blocks get one line each and cannot be acted on. Refs are those of the "
          "latest ui_tree: call it again after anything changed. These tools act as the user, also under "
          "their dialogs.",
          schema(json::object())),
      tool(
          "ui_click",
          "Click a widget or item like the mouse does (press and release through the window system), at its "
          "center or at x,y inside it. Returns what the press landed on. Open menus by clicking the menu bar "
          "item, then the item in the popup ui_tree lists.",
          schema(with(
              {{"button", button},
               {"double", {{"type", "boolean"}, {"default", false}, {"description", "a double click"}}},
               {"x", x},
               {"y", y},
               {"modifiers", mods}}))),
      tool(
          "ui_drag",
          "Press at `from` (default: the center) of the target, move to `to` (inside the target) or to the center "
          "of `to_ref`, and release: zoom rectangles, panning, splitters, sliders. Drag-and-drop between widgets "
          "runs the platform's drag loop and may not complete.",
          schema(with(
              {{"from", described(point, "[x, y] to press at, in the target's pixels (default: its center)")},
               {"to", described(point, "[x, y] to release at, in the target's pixels")},
               {"to_ref", {{"type", "string"}, {"description", "or a ref from ui_tree to release at the center of"}}},
               {"button", button},
               {"modifiers", mods}}))),
      tool(
          "ui_wheel", "Turn the mouse wheel over the target (at its center or x,y): notches > 0 scrolls up / zooms in.",
          schema(with(
              {{"notches", {{"type", "integer"}, {"description", "wheel notches; > 0 scrolls up"}}},
               {"x", x},
               {"y", y},
               {"modifiers", mods}}))),
      tool(
          "ui_key",
          "Press keys, as a key sequence (\"Ctrl+Z\", \"Return\", \"Escape\", \"Ctrl+S, Ctrl+Q\"), into the "
          "target after focusing it, or into whatever has focus. Window shortcuts fire only in the active "
          "window: the target's is activated first.",
          schema(with({{"keys", {{"type", "string"}, {"description", "a key sequence, e.g. Ctrl+Z"}}}}), {"keys"})),
      tool(
          "ui_type",
          "Type text key by key into the target after focusing it, or into whatever has focus. \\n presses "
          "Return; text after a Return that closes the window goes nowhere, so send it in a second call. At most " +
              std::to_string(kMaxUiTypedChars) + " characters per call.",
          schema(
              with(
                  {{"text",
                    {{"type", "string"}, {"maxLength", kMaxUiTypedChars}, {"description", "the text to type"}}}}),
              {"text"})),
      tool(
          "ui_plot",
          "Read what the visible plots show (for QA asserts): per plot its title, X and Y ranges (3 decimals), "
          "curves with their visibility, and the tracker value box rows as the user reads them (empty when "
          "hidden). `title` selects one plot.",
          schema({{"title", {{"type", "string"}, {"description", "only the plot with this title"}}}})),
  });
}

/// The job tools, which every token serves (McpServer's job table).
json jobToolEntries() {
  const json job_id = {{"type", "integer"}, {"description", "a job_id a long call returned"}};
  return json::array({
      toolEntry(
          "jobs_list",
          "Your calls still running in the background, and those that ended in the last minutes: job_id, tool, "
          "summary, state (running, waiting_for_user, completed, failed, killed), elapsed_s, progress.",
          schema(json::object())),
      toolEntry(
          "job_wait",
          "Wait for a background job: returns its result once it ends (again on every call, until it expires "
          "minutes later), or its job_id, state and progress if it is still running after timeout_s.",
          schema(
              {{"job_id", job_id},
               {"timeout_s",
                {{"type", "number"},
                 {"minimum", 0},
                 {"maximum", kMaxJobWaitS},
                 {"default", kDefaultWaitS},
                 {"description", "seconds to wait; 0 only looks"}}}},
              {"job_id"})),
      toolEntry(
          "job_kill",
          "Stop a background job: an evaluation stops computing, a file import stops (a stopped reload always "
          "restores the data it was replacing), a stream stops (its data stays), an export writes "
          "nothing, and a dialog the job opened for the user closes. Returns its final state: killed, or "
          "completed/failed if it ended first; when the stop takes longer to act, its state with stopping=true "
          "(job_wait then returns when it has ended).",
          schema(
              {{"job_id", job_id},
               {"keep_partial",
                {{"type", "boolean"},
                 {"default", true},
                 {"description", "a stopped file import keeps the rows loaded so far; false removes them"}}}},
              {"job_id"})),
  });
}

/// Adds the wait_s argument to the schema of `entry`, a job-capable tool.
void addWaitS(json& entry) {
  json& input = entry["inputSchema"];
  if (!input.is_object()) {
    input = schema(json::object());
  }
  input["properties"]["wait_s"] = {
      {"type", "number"},
      {"minimum", 0},
      {"maximum", kMaxWaitS},
      {"default", kDefaultWaitS},
      {"description",
       "seconds to wait for the result. A call still running then returns {job_id, state, progress} and carries "
       "on in the background: job_wait gets its result, job_kill stops it. 0 returns the job_id at once."}};
}

}  // namespace

void setUiToolsEnabled(bool enabled) {
  g_ui_tools_enabled = enabled;
}

bool uiToolsEnabled() {
  return g_ui_tools_enabled;
}

bool isUiTool(const std::string& name) {
  static const std::set<std::string> ui_tools = {"ui_tree", "ui_click", "ui_drag", "ui_wheel",
                                                 "ui_key",  "ui_type",  "ui_plot"};
  return ui_tools.contains(name);
}

int uiSettleMs(const std::string& name, const json& args) {
  if (name == "ui_tree" || name == "ui_plot") {
    return 0;
  }
  // Read before the input is queued, so it never throws: a call must not fail after acting.
  const auto wait = args.find("wait_ms");
  if (wait == args.end() || !wait->is_number()) {
    return kDefaultUiSettleMs;
  }
  return static_cast<int>(std::clamp(wait->get<double>(), 0.0, double(kMaxUiSettleMs)));
}

json toolList(ToolSet set) {
  json tools = set == ToolSet::kFull ? appToolEntries() : json::array();
  if (set == ToolSet::kFull && uiToolsEnabled()) {
    for (json& entry : uiToolEntries()) {
      tools.push_back(std::move(entry));
    }
  }
  for (const json& entry : agentTools().toMcpToolsList()) {
    const std::string name = entry.value("name", "");
    // get_status names the datasets and the instructions count their topics: report_status serves plugin
    // tokens, which have neither.
    if (set == ToolSet::kFull && name == "report_status") {
      continue;
    }
    tools.push_back(toolEntry(name, entry.value("description", ""), entry.value("inputSchema", json())));
  }
  for (json& entry : tools) {
    if (isJobCapableTool(entry.value("name", ""))) {
      addWaitS(entry);
    }
  }
  for (json& entry : jobToolEntries()) {
    tools.push_back(std::move(entry));
  }
  return tools;
}

json bridgeToolEntries() {
  return json::array({
      toolEntry(
          "list_instances",
          "PlotJuggler windows running with MCP on this computer: id, loaded datasets, whether it has the user's "
          "focus and whether this session is using it. Only needed when several run.",
          schema(json::object())),
      toolEntry(
          "select_instance", "Use the PlotJuggler instance with this id (from list_instances) for every later call.",
          schema({{"id", {{"type", "integer"}, {"description", "an instance id from list_instances"}}}}, {"id"})),
      toolEntry(
          "launch",
          "Start a new PlotJuggler window with MCP on (when none runs, or the user wants a separate one) and use "
          "it. Returns once it is ready for other tools.",
          schema(json::object())),
  });
}

const agent_tools::ToolRegistry& agentTools() {
  static const agent_tools::ToolRegistry registry;
  return registry;
}

std::optional<json> lifecycleResult(const std::string& method, const json& params, ToolSet set) {
  // What both handshakes add to their own fields.
  const auto describe = [set](json result) {
    static const std::string full_instructions = std::string(kAgentToolInstructions) + kAppToolInstructions;
    result["capabilities"] = {{"tools", json::object()}};
    result["serverInfo"] = {
        {"name", "plotjuggler"},
        {"title", "PlotJuggler"},
        {"version", QCoreApplication::applicationVersion().toStdString()}};
    result["instructions"] = set == ToolSet::kFull ? full_instructions : std::string(kAgentToolInstructions);
    return result;
  };
  if (method == "initialize") {
    static const std::set<std::string> known = {"2025-03-26", "2025-06-18", "2025-11-25"};
    const auto asked = params.find("protocolVersion");
    const bool echo = asked != params.end() && asked->is_string() && known.contains(asked->get<std::string>());
    return describe({{"protocolVersion", echo ? asked->get<std::string>() : std::string("2025-06-18")}});
  }
  if (method == "server/discover") {
    json result = describe({{"supportedVersions", {"2026-07-28", "2025-11-25", "2025-06-18"}}});
    // Offered where a client can take it up: it declares extensions per request, which only the
    // stateless protocol has (nothing here remembers an initialize).
    result["capabilities"]["extensions"] = {{kTasksExtension, json::object()}};
    return result;
  }
  if (method == "ping") {
    return json::object();
  }
  return std::nullopt;
}

std::string clientName(const json& params) {
  const auto name_of = [](const json& info) {
    const auto name = info.find("name");
    return info.is_object() && name != info.end() && name->is_string() ? name->get<std::string>() : std::string();
  };
  if (const auto info = params.find("clientInfo"); info != params.end()) {
    return name_of(*info);
  }
  if (const auto meta = params.find("_meta"); meta != params.end() && meta->is_object()) {
    if (const auto info = meta->find("io.modelcontextprotocol/clientInfo"); info != meta->end()) {
      return name_of(*info);
    }
  }
  return {};
}

std::string ownerFor(const std::string& client) {
  std::string name;
  for (const char c : client) {
    const auto u = static_cast<unsigned char>(c);
    const char mapped = std::isalnum(u) != 0 && u < 128 ? static_cast<char>(std::tolower(u)) : '-';
    if (mapped != '-' || (!name.empty() && name.back() != '-')) {
      name += mapped;
    }
  }
  name = name.substr(0, 32);
  while (!name.empty() && name.back() == '-') {
    name.pop_back();
  }
  return kAgentOwnerPrefix + (name.empty() ? std::string("mcp") : name);
}

std::string printableClientName(const std::string& client) {
  std::string out;
  for (const char c : client) {
    if (c >= 0x20 && c < 0x7f && out.size() < 64) {
      out += c;
    }
  }
  return out;
}

RpcMessage parseRpcMessage(const std::string& text) {
  RpcMessage out;
  bool too_deep = false;
  // Elements past the limit are discarded as they parse, so the document never gets deep.
  const json message = json::parse(
      text,
      [&too_deep](int depth, json::parse_event_t, json&) {
        too_deep = too_deep || depth > kMaxJsonDepth;
        return depth <= kMaxJsonDepth;
      },
      false);
  if (message.is_discarded()) {
    out.error = errorResponse(nullptr, -32700, "parse error");
    return out;
  }
  if (const auto id = message.is_object() ? message.find("id") : message.end();
      id != message.end() && (id->is_string() || id->is_number_integer())) {
    out.id = *id;
  }
  const auto refuse = [&out](const std::string& why) {
    out.error = errorResponse(out.id, -32600, why);
    return out;
  };
  if (too_deep) {
    return refuse("JSON nested deeper than " + std::to_string(kMaxJsonDepth) + " levels");
  }
  if (!message.is_object()) {
    return refuse("a JSON-RPC message must be an object");
  }
  if (message.value("jsonrpc", json()) != "2.0") {
    return refuse("jsonrpc must be \"2.0\"");
  }
  if (message.contains("id") && out.id.is_null()) {
    return refuse("id must be a string or an integer");
  }
  const auto method = message.find("method");
  if (method == message.end() || !method->is_string()) {
    return refuse("method must be a string");
  }
  if (const auto params = message.find("params"); params != message.end()) {
    if (!params->is_object()) {
      return refuse("params must be an object");
    }
    out.params = *params;
  }
  out.method = method->get<std::string>();
  out.notification = !message.contains("id");
  return out;
}

json errorResponse(const json& id, int code, const std::string& message, const json& data) {
  json response = {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
  if (!data.is_null()) {
    response["error"]["data"] = data;
  }
  return response;
}

int parseHttpRequest(const QByteArray& buffer, HttpRequest& out) {
  const qsizetype header_end = buffer.indexOf("\r\n\r\n");
  if (header_end < 0) {
    return buffer.size() > kMaxHeaderBytes ? 431 : 0;
  }
  if (header_end > kMaxHeaderBytes) {
    return 431;
  }
  HttpRequest request;
  const QList<QByteArray> lines = buffer.left(header_end).split('\n');
  const QList<QByteArray> request_line = lines.value(0).trimmed().split(' ');
  if (request_line.size() != 3 || !request_line[2].startsWith("HTTP/1.")) {
    return 400;
  }
  request.method = request_line[0];
  request.path = request_line[1];
  for (qsizetype i = 1; i < lines.size(); ++i) {
    const qsizetype colon = lines[i].indexOf(':');
    if (colon <= 0) {
      return 400;
    }
    request.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
  }
  if (request.headers.contains("transfer-encoding")) {
    return 400;  // chunked bodies are not spoken here; every client sends Content-Length
  }
  qsizetype length = 0;
  if (const auto it = request.headers.constFind("content-length"); it != request.headers.cend()) {
    bool ok = false;
    const qlonglong declared = it->toLongLong(&ok);
    if (!ok || declared < 0 || it->startsWith('+')) {
      return 400;
    }
    if (declared > kMaxBodyBytes) {
      return 413;
    }
    length = static_cast<qsizetype>(declared);  // checked above: fits a 32-bit qsizetype too
  }
  if (buffer.size() - header_end - 4 < length) {
    return 0;
  }
  request.body = buffer.mid(header_end + 4, length);
  out = std::move(request);
  return 200;
}

int checkAccess(const HttpRequest& request, const QByteArray& token) {
  // Agents send no Origin; a browser page always does (cross-site requests and DNS rebinding).
  if (request.headers.contains("origin")) {
    return 403;
  }
  const QByteArray given = request.headers.value("authorization");
  const QByteArray expected = "Bearer " + token;
  if (given.size() != expected.size()) {
    return 401;  // the token's length is no secret: it is always 64 hex digits
  }
  unsigned char difference = 0;
  for (qsizetype i = 0; i < given.size(); ++i) {
    difference |= static_cast<unsigned char>(given[i] ^ expected[i]);
  }
  return difference == 0 ? 0 : 401;
}

QByteArray httpResponse(int status, const QByteArray& body, const QByteArray& content_type) {
  static const std::map<int, QByteArray> reasons = {
      {200, "OK"},
      {202, "Accepted"},
      {400, "Bad Request"},
      {401, "Unauthorized"},
      {403, "Forbidden"},
      {404, "Not Found"},
      {405, "Method Not Allowed"},
      {413, "Content Too Large"},
      {431, "Request Header Fields Too Large"},
      {503, "Service Unavailable"}};
  const auto reason = reasons.find(status);
  QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' +
                    (reason != reasons.end() ? reason->second : QByteArray("Error")) + "\r\n";
  if (!body.isEmpty()) {
    head += "Content-Type: " + content_type + "\r\n";
  }
  return head + "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

QString instancesDir() {
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + u"/mcp-instances"_s;
  if (QDir().mkpath(dir)) {
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
  }
  return dir;
}

QString selfExecutable() {
  const QString appimage = qEnvironmentVariable("APPIMAGE");
  return appimage.isEmpty() ? QCoreApplication::applicationFilePath() : appimage;
}

void recordExecutable() {
  const QByteArray exe = QDir::toNativeSeparators(selfExecutable()).toUtf8();
  QFile file(instancesDir() + u"/executable"_s);
  if (file.open(QIODevice::ReadOnly) && file.readAll() == exe) {
    return;
  }
  file.close();
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    file.write(exe);
  }
}

}  // namespace PJ::mcp
