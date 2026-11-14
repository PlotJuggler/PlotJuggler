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
    "keeps in their layout. Draw only in your own tabs (plot_tab, scene_view); you cannot see or change the "
    "user's. playback moves the shared time cursor. If a tool says the user has a dialog open, wait and try "
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
    "add_script_series (Luau or Python) and add_filter install live derived series too. Take a screenshot to "
    "check what the user sees.";
#else
constexpr auto kAppToolInstructions =
    " Start with get_status (it also shows whether the user's dialog is still open); open_file loads any "
    "format PlotJuggler reads, start_stream starts a live source. export_series gives aligned CSV for your own "
    "tools; add_script_series (Luau or Python) and add_filter install live derived series too. Take a "
    "screenshot to check what the user sees. Read about before explaining how to do something in the "
    "PlotJuggler UI.";
#endif

}  // namespace

json schema(json properties, json required) {
  return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}};
}

json toolEntry(const std::string& name, const std::string& description, json input_schema, bool read_only) {
  return {
      {"name", name},
      {"description", description},
      {"inputSchema", std::move(input_schema)},
      {"annotations", {{"readOnlyHint", read_only}, {"openWorldHint", false}}}};
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
  static const std::set<std::string> read_only = {"get_status",     "about",         "list_instances", "list_topics",
                                                  "describe_topic", "read_series",   "evaluate",       "wait_for",
                                                  "list_created",   "report_status", "jobs_list",      "job_wait"};
  return read_only.contains(name);
}

bool runsUnderUsersDialog(const std::string& name, const json& args) {
  return isReadOnlyTool(name) || name == "job_kill" || (name == "screenshot" && !args.contains("tab"));
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
#else
#define PJ_MCP_ABOUT_SOURCES "; about says what each one is"
#endif

/// The tools/list entries of the app's own tools (McpServerTools.cpp).
json appToolEntries() {
  const auto tool = [](const char* name, const char* desc, json input) {
    return toolEntry(name, desc, std::move(input), isReadOnlyTool(name));
  };
  const json str = {{"type", "string"}};
  const json paths = {
      {"type", "array"},
      {"items", str},
      {"description", "series paths as read_series takes them: topic/field, a unique suffix, or dataset:topic/field"}};
  const json dataset = {{"type", {"string", "integer"}}, {"description", "dataset id, name or path (get_status)"}};
  return json::array({
      tool(
          "get_status",
          "Start here. Loaded datasets (id, name, path, loader config; live streams carry stream), the stream "
          "sources start_stream can start, time range, current time, your agent id and tabs, whether "
          "PlotJuggler has the user's focus and any dialog the user has open.",
          schema(json::object())),
#ifndef PJ_TARGET_WASM
      tool(
          "about",
          "What this PlotJuggler can do: built-in features with where to find them in the UI, known limitations, "
          "and the plugins that are loaded, installed or installable. Read it before telling the user how to do "
          "something in PlotJuggler.",
          schema(json::object())),
#endif
      tool(
          "open_file",
          "Load a data file (MCAP, CSV, ULog, Parquet...). Returns when the import has finished, or after wait_s "
          "with a job_id while a large file still loads: its topics are readable as they arrive, job_wait returns "
          "the outcome and job_kill stops the import. By default the loader's own defaults apply (e.g. every "
          "topic). ask_user=true opens PlotJuggler's loader dialog so the user picks topics/options. config: "
          "loader settings JSON (get_status shows the config each loaded dataset used, as a template).",
          schema(
              {{"path", {{"type", "string"}, {"description", "absolute path"}}},
               {"ask_user", {{"type", "boolean"}}},
               {"config", {{"type", "object"}}}},
              {"path"})),
      tool(
          "unload_file",
          "Remove a loaded dataset and its data. If derived series depend on it, the user is asked to confirm in "
          "PlotJuggler and the call returns their answer (or a job_id after wait_s while they decide; job_kill "
          "withdraws the question).",
          schema({{"dataset", dataset}}, {"dataset"})),
      tool(
          "reload_file", "Reload a dataset from its file with the same loader settings (e.g. after it changed).",
          schema({{"dataset", dataset}}, {"dataset"})),
      tool(
          "start_stream",
          "Start a live streaming source (get_status lists stream_sources" PJ_MCP_ABOUT_SOURCES "). By "
          "default it uses the settings the user last confirmed in that source's dialog; config passes your own "
          "(get_status shows a live stream's config, as a template); ask_user=true opens the source's dialog so "
          "the user sets it up. Returns dataset_id once the stream runs, with the job_id of the job that lives "
          "as long as the stream: job_kill (or stop_stream) stops it, job_wait returns when it ends. Topics "
          "appear as the source advertises them.",
          schema(
              {{"source", {{"type", "string"}, {"description", "stream source name, from stream_sources"}}},
               {"ask_user", {{"type", "boolean"}}},
               {"config", {{"type", "object"}}}},
              {"source"})),
      tool(
          "stop_stream", "Stop a live stream; its data stays loaded. dataset: a live stream from get_status.",
          schema({{"dataset", dataset}}, {"dataset"})),
#ifndef PJ_TARGET_WASM
      tool(
          "export_series",
          "Write several series to ONE time-aligned file for your own analysis (numpy/pandas): fits, "
          "correlations, lags across signals. Works for every format PlotJuggler opened, so prefer it to "
          "decoding files yourself. Rows at every sample time of any series, or on a uniform rate_hz grid; "
          "values sample-and-hold; time in display seconds (as playback). Returns the path. format \"npy\" "
          "writes a numpy structured array that numpy.load reads in well under a second, where numpy.loadtxt "
          "takes seconds per million rows of CSV: use it beyond ~100k rows.",
          schema(
              {{"paths", paths},
               {"path", {{"type", "string"}, {"description", "absolute .csv or .npy path (default: a temp file)"}}},
               {"format", {{"type", "string"}, {"enum", {"csv", "npy"}}, {"description", "csv (default) or npy"}}},
               {"rate_hz", {{"type", "number"}}},
               {"t0", {{"type", "number"}}},
               {"t1", {{"type", "number"}}}},
              {"paths"})),
#endif
      tool(
          "add_script_series",
          "Create your own derived series with a Luau or Python script. Python: first line '# pj-script: python', "
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
              {{"name", str},
               {"inputs", paths},
               {"outputs", {{"type", "array"}, {"items", str}}},
               {"script", str},
               {"params", {{"type", "object"}}}},
              {"name", "inputs", "script"})),
      tool(
          "add_filter",
          "Create a derived series by applying a built-in filter (integral, derivative, absolute, scale, "
          "moving_average, moving_rms, moving_variance, outlier_removal, binary_filter, samples_counter, "
          "time_since_previous) to one series; params override the filter's defaults. Output name defaults to "
          "<input>[filter]. Like add_script_series: listed under Custom Topics, removable with "
          "remove_script_series, outside the user's undo. Returns the output topic and field to plot.",
          schema(
              {{"input", {{"type", "string"}, {"description", "series path, as read_series takes it"}}},
               {"filter", str},
               {"params", {{"type", "object"}}},
               {"name", str}},
              {"input", "filter"})),
      tool(
          "remove_script_series",
          "Delete a series made with add_script_series or add_filter. Refused while someone else's series (the "
          "user's, a plugin's) is built on it, since that would be deleted too.",
          schema({{"name", str}}, {"name"})),
      tool(
          "screenshot",
          "PNG of the PlotJuggler window, including GPU-rendered plots and 3D views, and any dialog the user has "
          "open. tab: one of your plot_tab/scene_view tabs to bring to the front first.",
          schema({{"tab", str}, {"max_width", {{"type", "integer"}}}})),
  });
}

}  // namespace

#undef PJ_MCP_ABOUT_SOURCES

namespace {

/// `seconds` as the whole number a tool description quotes.
std::string whole(double seconds) {
  return std::to_string(static_cast<int>(seconds));
}

/// The job tools, which every token serves (McpServer's job table).
json jobToolEntries() {
  const json job_id = {{"type", "integer"}, {"description", "a job_id a long call returned"}};
  return json::array({
      toolEntry(
          "jobs_list",
          "Your calls still running in the background, and those that ended in the last minutes: job_id, tool, "
          "summary, state (running, waiting_for_user, completed, failed, killed), elapsed_s, progress.",
          schema(json::object()), true),
      toolEntry(
          "job_wait",
          "Wait for a background job: returns its result once it ends (again on every call, until it expires "
          "minutes later), or its job_id, state and progress if it is still running after timeout_s (default " +
              whole(kDefaultWaitS) + ", at most " + whole(kMaxJobWaitS) + "; 0 only looks).",
          schema({{"job_id", job_id}, {"timeout_s", {{"type", "number"}}}}, {"job_id"}), true),
      toolEntry(
          "job_kill",
          "Stop a background job: an evaluation stops computing, a file import stops (keep_partial, default true, "
          "keeps the rows loaded so far; false removes them; a stopped reload always restores the data it was "
          "replacing), a stream stops (its data stays), an export writes "
          "nothing, and a dialog the job opened for the user closes. Returns its final state: killed, or "
          "completed/failed if it ended first; when the stop takes longer to act, its state with stopping=true "
          "(job_wait then returns when it has ended).",
          schema({{"job_id", job_id}, {"keep_partial", {{"type", "boolean"}}}}, {"job_id"}), false),
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
      {"description",
       "seconds to wait for the result (default " + whole(kDefaultWaitS) + ", at most " + whole(kMaxWaitS) +
           "). A call still running then returns {job_id, state, progress} and carries on in the background: "
           "job_wait gets its result, job_kill stops it. 0 returns the job_id at once."}};
}

}  // namespace

json toolList(ToolSet set) {
  json tools = set == ToolSet::kFull ? appToolEntries() : json::array();
  for (const json& entry : agentTools().toMcpToolsList()) {
    const std::string name = entry.value("name", "");
    tools.push_back(
        toolEntry(name, entry.value("description", ""), entry.value("inputSchema", json()), isReadOnlyTool(name)));
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
