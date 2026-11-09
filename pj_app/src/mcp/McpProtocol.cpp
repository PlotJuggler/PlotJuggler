// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "mcp/McpProtocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <cctype>
#include <map>
#include <set>

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)
using namespace Qt::StringLiterals;

namespace PJ::mcp {

Q_LOGGING_CATEGORY(lcMcp, "pj.mcp")

namespace {

/// What every client is told: the agent tools, which every token serves.
constexpr auto kAgentToolInstructions =
    "PlotJuggler is the user's time-series and 3D data viewer, running on their desktop. Find topics with "
    "list_topics / describe_topic (exact paths; a unique suffix also resolves; \"dataset:topic/field\" picks "
    "one dataset). For numbers use read_series (statistics, min/max buckets, raw samples) and evaluate (a Luau "
    "computation over series or objects that leaves nothing behind) instead of decoding files yourself. "
    "create_derived_series, create_derived_object and create_markers install live results the user sees and "
    "keeps in their layout. Draw only in your own tabs (plot_tab, scene_view); you cannot see or change the "
    "user's. playback moves the shared time cursor. If a tool says the user has a dialog open, wait and try "
    "again.";
/// What a client with the app's own tools is told after kAgentToolInstructions.
constexpr auto kAppToolInstructions =
    " Start with get_status (it also shows whether the user's dialog is still open); open_file loads any "
    "format PlotJuggler reads, start_stream starts a live source. export_series gives aligned CSV for your own "
    "tools; add_script_series (Luau or Python) and add_filter install live derived series too. Take a "
    "screenshot to check what the user sees. Read about before explaining how to do something in the "
    "PlotJuggler UI.";

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

bool isReadOnlyTool(const std::string& name) {
  static const std::set<std::string> read_only = {"get_status",  "about",          "list_instances",
                                                  "list_topics", "describe_topic", "read_series",
                                                  "evaluate",    "list_created",   "report_status"};
  return read_only.contains(name);
}

namespace {

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
      tool(
          "about",
          "What this PlotJuggler can do: built-in features with where to find them in the UI, known limitations, "
          "and the plugins that are loaded, installed or installable. Read it before telling the user how to do "
          "something in PlotJuggler.",
          schema(json::object())),
      tool(
          "open_file",
          "Load a data file (MCAP, CSV, ULog, Parquet...). Returns when the import has finished (a client tool "
          "timeout does not stop it: get_status shows the dataset once it lands). By default the loader's own "
          "defaults apply (e.g. every topic). ask_user=true opens PlotJuggler's loader dialog so the user picks "
          "topics/options; the call returns when they finish or cancel. config: loader settings JSON (get_status "
          "shows the config each loaded dataset used, as a template).",
          schema(
              {{"path", {{"type", "string"}, {"description", "absolute path"}}},
               {"ask_user", {{"type", "boolean"}}},
               {"config", {{"type", "object"}}}},
              {"path"})),
      tool(
          "unload_file",
          "Remove a loaded dataset and its data. If derived series depend on it, the user is asked to confirm in "
          "PlotJuggler and the call returns their answer.",
          schema({{"dataset", dataset}}, {"dataset"})),
      tool(
          "reload_file", "Reload a dataset from its file with the same loader settings (e.g. after it changed).",
          schema({{"dataset", dataset}}, {"dataset"})),
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

json toolList(ToolSet set) {
  json tools = set == ToolSet::kFull ? appToolEntries() : json::array();
  for (const json& entry : agentTools().toMcpToolsList()) {
    const std::string name = entry.value("name", "");
    tools.push_back(
        toolEntry(name, entry.value("description", ""), entry.value("inputSchema", json()), isReadOnlyTool(name)));
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
    return describe({{"supportedVersions", {"2026-07-28", "2025-11-25", "2025-06-18"}}});
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

json errorResponse(const json& id, int code, const std::string& message) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
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
    length = it->toLongLong(&ok);
    if (!ok || length < 0 || it->startsWith('+')) {
      return 400;
    }
    if (length > kMaxBodyBytes) {
      return 413;
    }
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
