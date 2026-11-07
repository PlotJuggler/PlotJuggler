// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// SPIKE: see McpProtocol.h.

#include "spike/McpProtocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <cctype>
#include <set>

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)
using namespace Qt::StringLiterals;

namespace PJ::mcp {
namespace {

json schema(json props, json required = json::array()) {
  return {{"type", "object"}, {"properties", std::move(props)}, {"required", std::move(required)}};
}

constexpr auto kInstructions =
    "PlotJuggler is the user's time-series and 3D data viewer, running on their desktop. Call get_status "
    "first; open_file loads any format PlotJuggler reads, start_stream starts a live source. Find topics with "
    "list_topics / describe_topic (exact "
    "paths; a unique suffix also resolves). For numbers use read_series (statistics, min/max buckets, raw "
    "samples) and evaluate (a Luau computation over series or objects that leaves nothing behind) instead of "
    "decoding files yourself; export_series gives aligned CSV for your own tools. create_derived_series, "
    "create_derived_object and create_markers install live results the user sees and keeps in their layout. "
    "Draw only in your own tabs (plot_tab, scene_view); you cannot see or change the user's. playback moves the "
    "shared time cursor. Take a screenshot to check what the user sees. If a tool says the user has a dialog "
    "open, wait and poll get_status. Read about before explaining how to do something in the PlotJuggler UI.";

}  // namespace

bool isReadOnlyTool(const std::string& name) {
  static const std::set<std::string> read_only = {
      "get_status",  "list_catalog",   "screenshot",  "about",    "get_values",   "statistics",   "list_instances",
      "list_topics", "describe_topic", "read_series", "evaluate", "list_created", "report_status"};
  return read_only.contains(name);
}

json toolList() {
  const auto tool = [](const char* name, const char* desc, json input) {
    json entry = {{"name", name}, {"description", desc}, {"inputSchema", std::move(input)}};
    entry["annotations"] = {{"readOnlyHint", isReadOnlyTool(name)}, {"openWorldHint", false}};
    return entry;
  };
  const json str = {{"type", "string"}};
  const json dataset = {
      {"type", {"string", "integer"}},
      {"description", "dataset id, name or path (get_status); needed when several hold the topic"}};
  json tools = json::array({
      tool(
          "get_status",
          "Start here. Loaded datasets (id, name, path, loader config; live streams carry stream), the stream "
          "sources start_stream can start, time range, current time, the agent's "
          "tabs, whether PlotJuggler has the user's focus and any dialog the user has open.",
          schema(json::object())),
      tool(
          "list_catalog",
          "Per dataset: scalar topics with their plottable fields and types, and object topics (point clouds, "
          "images, poses, TF, derived...) with type, sample count and time range. Use these exact names in "
          "other tools.",
          schema(
              {{"prefix", {{"type", "string"}, {"description", "only topics starting with this"}}},
               {"dataset", dataset},
               {"max_fields", {{"type", "integer"}, {"description", "fields listed per topic (default 40)"}}}})),
      tool(
          "open_file",
          "Load a data file (MCAP, CSV, ULog, Parquet...). Returns when the import has finished. By default the "
          "loader's own defaults apply (e.g. every topic). ask_user=true opens PlotJuggler's loader dialog so the "
          "user picks topics/options; the call returns when they finish or cancel. config: loader settings JSON "
          "(get_status shows the config each loaded dataset used, as a template).",
          schema(
              {{"path", {{"type", "string"}, {"description", "absolute path"}}},
               {"ask_user", {{"type", "boolean"}}},
               {"config", {{"type", "object"}}}},
              {"path"})),
      tool(
          "add_filter",
          "Create a derived series by applying a built-in filter (integral, derivative, absolute, scale, "
          "moving_average, moving_rms, moving_variance, outlier_removal, binary_filter, samples_counter, "
          "time_since_previous) to topic/field; params override the filter's defaults. Output name defaults to "
          "topic/field[filter]. Like add_script_series: listed under Custom Topics, removable with "
          "remove_script_series, outside the user's undo. Returns the output topic and field to plot.",
          schema(
              {{"topic", str},
               {"field", str},
               {"filter", str},
               {"params", {{"type", "object"}}},
               {"dataset", dataset},
               {"name", str}},
              {"topic", "field", "filter"})),
      tool(
          "add_script_series",
          "Create your own derived series with a Luau or Python script. Python: first line '# pj-script: python', one "
          "top-level class T (its id is set for you), @staticmethod create(params) returning T(), and calculate(self, "
          "time, value, *more_inputs); imports limited to math, cmath, statistics. A Luau script returns a class: "
          "return { name=\"X\", "
          "create = function(params) local s = {}; return { calculate = function(t, v) ... return y end } end } "
          "(the id is set for you). t is seconds, v the input; keep state in the instance (integrators, bias "
          "removal). Several inputs: pass `inputs` [{topic, field, dataset?}], joined on equal timestamps "
          "(fields of one message always are); calculate(t, v, v1, v2, ...). Several outputs: pass `outputs` "
          "[names] and return that many numbers; they become topics <name>/<output>. Listed under Custom Topics, saved "
          "in layouts, untouched by the "
          "user's undo; the same name replaces it. Returns each output topic with its fields.",
          schema(
              {{"name", str},
               {"topic", str},
               {"field", str},
               {"dataset", dataset},
               {"inputs",
                {{"type", "array"},
                 {"items",
                  {{"type", "object"},
                   {"properties", {{"topic", str}, {"field", str}, {"dataset", dataset}}},
                   {"required", json::array({"topic", "field"})}}}}},
               {"outputs", {{"type", "array"}, {"items", str}}},
               {"script", str},
               {"params", {{"type", "object"}}}},
              {"name", "script"})),
      tool(
          "remove_script_series", "Delete a series made with add_script_series or add_filter.",
          schema({{"name", str}}, {"name"})),
      tool(
          "about",
          "What this PlotJuggler can do: built-in features with where to find them in the UI, known limitations, "
          "and the plugins that are loaded, installed or installable. Read it before telling the user how to do "
          "something in PlotJuggler.",
          schema(json::object())),
      tool(
          "get_values",
          "Samples of topic/field as [time, value] pairs (display seconds, as in get_status and playback), evenly "
          "decimated to max_points (default 200). Optional t0/t1 range and dataset (needed when the topic "
          "exists in several datasets). With `at`, only the sample in force at that time.",
          schema(
              {{"topic", str},
               {"field", str},
               {"dataset", dataset},
               {"t0", {{"type", "number"}}},
               {"t1", {{"type", "number"}}},
               {"max_points", {{"type", "integer"}}},
               {"at", {{"type", "number"}}}},
              {"topic", "field"})),
      tool(
          "statistics",
          "count, min, max, mean, std, first/last time and value of topic/field over the optional t0/t1 range. "
          "Cheaper than get_values for checking signals.",
          schema(
              {{"topic", str},
               {"field", str},
               {"dataset", dataset},
               {"t0", {{"type", "number"}}},
               {"t1", {{"type", "number"}}}},
              {"topic", "field"})),
      tool(
          "export_series",
          "Write several series to ONE time-aligned CSV for your own analysis (numpy/pandas): fits, "
          "correlations, lags across signals. Works for every format PlotJuggler opened, so prefer it to "
          "decoding files yourself. Rows at every sample time of any series, or on a uniform rate_hz grid; "
          "values sample-and-hold. Returns the path.",
          schema(
              {{"series",
                {{"type", "array"},
                 {"items",
                  {{"type", "object"},
                   {"properties", {{"topic", str}, {"field", str}, {"dataset", dataset}}},
                   {"required", json::array({"topic", "field"})}}}}},
               {"path", {{"type", "string"}, {"description", "absolute .csv path (default: a temp file)"}}},
               {"rate_hz", {{"type", "number"}}},
               {"t0", {{"type", "number"}}},
               {"t1", {{"type", "number"}}}},
              {"series"})),
      tool(
          "start_stream",
          "Start a live streaming source (get_status lists stream_sources; about says what each one is). By "
          "default it uses the settings the user last confirmed in that source's dialog; config passes your own "
          "(get_status shows a live stream's config, as a template); ask_user=true opens the source's dialog so "
          "the user sets it up, and the call returns when they finish or cancel. Topics appear as the source "
          "advertises them.",
          schema(
              {{"source", {{"type", "string"}, {"description", "stream source name, from stream_sources"}}},
               {"ask_user", {{"type", "boolean"}}},
               {"config", {{"type", "object"}}}},
              {"source"})),
      tool(
          "stop_stream", "Stop a live stream; its data stays loaded. dataset: a live stream from get_status.",
          schema({{"dataset", dataset}}, {"dataset"})),
      tool(
          "unload_file",
          "Remove a loaded dataset and its data. dataset: id, name or path from get_status. If derived series "
          "depend on it, the user is asked to confirm in PlotJuggler.",
          schema({{"dataset", dataset}}, {"dataset"})),
      tool(
          "reload_file", "Reload a dataset from its file with the same loader settings (e.g. after it changed).",
          schema({{"dataset", dataset}}, {"dataset"})),
      tool(
          "screenshot",
          "PNG of the PlotJuggler window, including GPU-rendered plots and 3D views, and any dialog the user has "
          "open. tab: one of your plot_tab/scene_view tabs to bring to the front first.",
          schema({{"tab", str}, {"max_width", {{"type", "integer"}}}})),
  });
  // The Assistant Agent's tools, unchanged: the same code the in-app assistant runs.
  for (json entry : assistantTools().toMcpToolsList()) {
    entry["annotations"] = {{"readOnlyHint", isReadOnlyTool(entry.value("name", ""))}, {"openWorldHint", false}};
    tools.push_back(std::move(entry));
  }
  return tools;
}

const assistant_agent::ToolRegistry& assistantTools() {
  static const assistant_agent::ToolRegistry registry;
  return registry;
}

std::optional<json> lifecycleResult(const std::string& method, const json& params) {
  const json info = {{"name", "plotjuggler"}, {"title", "PlotJuggler"}, {"version", "4-spike"}};
  if (method == "initialize") {
    const std::string asked = params.value("protocolVersion", "2025-06-18");
    const bool known = asked == "2025-06-18" || asked == "2025-11-25" || asked == "2025-03-26";
    return json{
        {"protocolVersion", known ? asked : "2025-06-18"},
        {"capabilities", {{"tools", json::object()}}},
        {"serverInfo", info},
        {"instructions", kInstructions}};
  }
  if (method == "server/discover") {
    return json{
        {"supportedVersions", {"2026-07-28", "2025-11-25", "2025-06-18"}},
        {"capabilities", {{"tools", json::object()}}},
        {"serverInfo", info},
        {"instructions", kInstructions}};
  }
  if (method == "ping") {
    return json::object();
  }
  return std::nullopt;
}

std::string clientName(const json& params) {
  if (params.contains("clientInfo")) {
    return params["clientInfo"].value("name", "");
  }
  const json meta = params.value("_meta", json::object());
  if (const auto it = meta.find("io.modelcontextprotocol/clientInfo"); it != meta.end() && it->is_object()) {
    return it->value("name", "");
  }
  return {};
}

std::string ownerFor(const std::string& client) {
  std::string owner;
  for (const char c : client) {
    const auto u = static_cast<unsigned char>(c);
    owner += (std::isalnum(u) != 0) ? static_cast<char>(std::tolower(u)) : '-';
  }
  return owner.empty() ? std::string("mcp-agent") : "agent-" + owner;
}

QString instancesDir() {
  // Not RuntimeLocation: agents start the bridge with a stripped environment (Codex passes no
  // XDG_RUNTIME_DIR), and Qt then falls back to another folder than the GUI's. This one needs only HOME.
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
