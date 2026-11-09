// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// `plotjuggler4 --mcp-bridge`: MCP over stdio in front of the running instances' HTTP servers. One
// bridge process per agent session, so its instance choice is per session.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QProcess>
#include <QTcpSocket>
#include <QThread>
#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "mcp/McpProtocol.h"
#include "pj_runtime/ActiveWidgetsMarker.h"

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)
using namespace Qt::StringLiterals;

namespace PJ::mcp {
namespace {

/// Short waits for liveness probes; a tool call may legitimately take long (an import, a dialog the
/// user is answering), but never forever: a hung window must not hang the agent.
constexpr int kConnectTimeoutMs = 2000;
constexpr int kProbeTimeoutMs = 5000;
constexpr int kCallTimeoutMs = 300'000;

struct Instance {
  qint64 pid = 0;
  quint16 port = 0;
  std::string token;
  qint64 started_ms = 0;
  QString file;
};

std::vector<Instance> readInstances() {
  std::vector<Instance> out;
  for (const QFileInfo& info : QDir(instancesDir()).entryInfoList({u"*.json"_s}, QDir::Files)) {
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
      continue;
    }
    const json entry = json::parse(file.readAll().toStdString(), nullptr, false);
    if (entry.is_object() && entry.value("pid", json()).is_number_integer() &&
        entry.value("port", json()).is_number_integer() && entry.value("token", json()).is_string()) {
      out.push_back(
          {.pid = entry["pid"].get<qint64>(),
           .port = entry["port"].get<quint16>(),
           .token = entry["token"].get<std::string>(),
           .started_ms = entry.value("started_ms", qint64{0}),
           .file = info.absoluteFilePath()});
    }
  }
  std::sort(out.begin(), out.end(), [](const Instance& a, const Instance& b) { return a.started_ms < b.started_ms; });
  return out;
}

/// The outcome of one HTTP exchange. Only kRefused (nothing listens) says the instance is gone;
/// a timeout, a dropped connection or an error status leaves its discovery file alone.
struct Exchange {
  enum Kind { kOk, kRefused, kTimeout, kClosed, kBadReply } kind = kBadReply;
  json reply = {};
  QByteArray status = {};  ///< the status line of a kBadReply
};

Exchange post(const Instance& instance, const json& message, const std::string& client, int timeout_ms) {
  QTcpSocket socket;
  socket.connectToHost(QHostAddress::LocalHost, instance.port);
  if (!socket.waitForConnected(kConnectTimeoutMs)) {
    return {.kind = socket.error() == QAbstractSocket::SocketTimeoutError ? Exchange::kTimeout : Exchange::kRefused};
  }
  const QByteArray body = QByteArray::fromStdString(message.dump());
  socket.write(
      "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nAccept: application/json\r\n"
      "Authorization: Bearer " +
      QByteArray::fromStdString(instance.token) + "\r\nX-PJ-Client: " + QByteArray::fromStdString(client) +
      "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
  QByteArray response;
  QElapsedTimer waited;
  waited.start();
  // The server closes after its reply (Connection: close).
  while (socket.state() == QAbstractSocket::ConnectedState) {
    const qint64 left = timeout_ms - waited.elapsed();
    if (left <= 0) {
      return {.kind = Exchange::kTimeout};
    }
    if (socket.waitForReadyRead(static_cast<int>(left))) {
      response += socket.readAll();
    } else if (socket.error() == QAbstractSocket::SocketTimeoutError) {
      return {.kind = Exchange::kTimeout};
    }
  }
  response += socket.readAll();
  if (response.isEmpty()) {
    return {.kind = Exchange::kClosed};
  }
  const qsizetype header_end = response.indexOf("\r\n\r\n");
  if (header_end < 0 || !response.startsWith("HTTP/1.1 200")) {
    return {.kind = Exchange::kBadReply, .status = response.left(response.indexOf('\r'))};
  }
  json reply = json::parse(response.mid(header_end + 4).toStdString(), nullptr, false);
  if (!reply.is_object()) {
    return {.kind = Exchange::kBadReply, .status = "unparsable reply"};
  }
  return {.kind = Exchange::kOk, .reply = std::move(reply)};
}

json request(const std::string& method, json params = json::object()) {
  return {{"jsonrpc", "2.0"}, {"id", 0}, {"method", method}, {"params", std::move(params)}};
}

/// The start of a launch log, for an error the agent can act on: the cause comes first, the crash
/// handler's backtrace after it.
std::string logHead(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.read(1200).toStdString() : std::string();
}

json bridgeTools() {
  return json::array({
      toolEntry(
          "list_instances",
          "PlotJuggler windows running with MCP on this computer: id, loaded datasets, whether it has the user's "
          "focus and whether this session is using it. Only needed when several run.",
          schema(json::object()), true),
      toolEntry(
          "select_instance", "Use the PlotJuggler instance with this id (from list_instances) for every later call.",
          schema({{"id", {{"type", "integer"}}}}, {"id"}), false),
      toolEntry(
          "launch",
          "Start a new PlotJuggler window with MCP on (when none runs, or the user wants a separate one) and use "
          "it. Returns once it is ready for other tools.",
          schema(json::object()), false),
  });
}

class Bridge {
 public:
  /// Every client request may name the client (initialize, or a 2026 request's _meta).
  void noteClient(const json& params) {
    if (const std::string name = printableClientName(clientName(params)); !name.empty()) {
      client_ = name;
    }
  }

  /// The current instance's result object for `method`; nullopt when none answers.
  std::optional<json> askInstance(const std::string& method, const json& params = json::object()) {
    const std::optional<Instance> target = currentInstance();
    if (!target) {
      return std::nullopt;
    }
    const Exchange answer = post(*target, request(method, params), client_, kProbeTimeoutMs);
    const json result = answer.kind == Exchange::kOk ? answer.reply.value("result", json()) : json();
    return result.is_object() ? std::optional<json>(result) : std::nullopt;
  }

  json listTools() {
    json tools;
    if (const std::optional<json> result = askInstance("tools/list")) {
      tools = result->value("tools", json());
    }
    if (!tools.is_array()) {
      tools = toolList(ToolSet::kFull);  // nothing answers: this binary's own list
    }
    for (json& tool : bridgeTools()) {
      tools.push_back(std::move(tool));
    }
    return {{"tools", tools}};
  }

  json callTool(const json& params) {
    const std::string name = params.value("name", "");
    const json args = params.value("arguments", json::object());
    if (name == "list_instances") {
      return listInstances();
    }
    if (name == "select_instance") {
      const json id = args.value("id", json());
      for (const Instance& instance : liveInstances()) {
        if (id.is_number_integer() && instance.pid == id.get<qint64>()) {
          selected_ = instance.pid;
          return textResult(json({{"selected", selected_}}).dump());
        }
      }
      return textResult("no running PlotJuggler with id " + id.dump() + " (see list_instances)", true);
    }
    if (name == "launch") {
      return launch();
    }
    return forward(params);
  }

 private:
  /// Instances that answer a ping now. A file whose process is gone, or whose port refuses, is a
  /// crash leftover and is deleted (the pid check comes first: a later instance may reuse the dead
  /// one's port). One that is slow or answers oddly is skipped but kept.
  std::vector<Instance> liveInstances() const {
    std::vector<Instance> live;
    for (const Instance& instance : readInstances()) {
      if (!ActiveWidgetsMarker::isProcessAlive(instance.pid)) {
        QFile::remove(instance.file);
        continue;
      }
      const Exchange answer = post(instance, request("ping"), client_, kProbeTimeoutMs);
      if (answer.kind == Exchange::kOk) {
        live.push_back(instance);
      } else if (answer.kind == Exchange::kRefused) {
        QFile::remove(instance.file);
      }
    }
    return live;
  }

  /// The selected instance while its file exists, else the only live one; nullopt otherwise.
  std::optional<Instance> currentInstance() const {
    for (const Instance& instance : readInstances()) {
      if (selected_ != 0 && instance.pid == selected_) {
        return instance;
      }
    }
    if (selected_ != 0) {
      return std::nullopt;
    }
    const std::vector<Instance> live = liveInstances();
    return live.size() == 1 ? std::optional<Instance>(live.front()) : std::nullopt;
  }

  json listInstances() const {
    json out = json::array();
    for (const Instance& instance : liveInstances()) {
      json entry = {{"id", instance.pid}, {"selected", selected_ == instance.pid}};
      const Exchange answer = post(
          instance, request("tools/call", {{"name", "get_status"}, {"arguments", json::object()}}), client_,
          kProbeTimeoutMs);
      if (answer.kind == Exchange::kOk && answer.reply.contains("result")) {
        const json content = answer.reply["result"].value("content", json::array());
        const json status = content.empty() ? json() : json::parse(content[0].value("text", ""), nullptr, false);
        if (status.is_object()) {
          json datasets = json::array();
          for (const json& dataset : status.value("datasets", json::array())) {
            datasets.push_back(dataset.value("name", ""));
          }
          entry["datasets"] = datasets;
          entry["focused"] = status.value("focused", false);
          entry["user_dialog"] = status.value("user_dialog", json());
        }
      }
      out.push_back(std::move(entry));
    }
    return textResult(out.dump());
  }

  json launch() {
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    // Agents start MCP servers with a filtered environment (Codex passes only a few variables).
    if (qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
      return textResult(
          "cannot start PlotJuggler from here: this MCP server has no display (DISPLAY and WAYLAND_DISPLAY are "
          "unset in its environment). Ask the user to start PlotJuggler with --mcp, or to pass the variables to "
          "the server (Codex: mcp_servers.<name>.env_vars = [\"DISPLAY\", \"WAYLAND_DISPLAY\", "
          "\"XDG_RUNTIME_DIR\"]).",
          true);
    }
#endif
    const QString log = instancesDir() + u"/launch.log"_s;
    QProcess process;
    process.setProgram(selfExecutable());
    process.setArguments({u"--mcp"_s});
    // stdout is this bridge's MCP channel: the child must never write to it.
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(log);
    process.setStandardErrorFile(log);
    qint64 pid = 0;
    if (!process.startDetached(&pid)) {
      return textResult("could not start " + selfExecutable().toStdString(), true);
    }
    // ponytail: polls the discovery folder; a file watcher if startup latency ever matters.
    for (int waited_ms = 0; waited_ms < 60000; waited_ms += 200) {
      QThread::msleep(200);
      for (const Instance& instance : readInstances()) {
        if (instance.pid == pid && post(instance, request("ping"), client_, kProbeTimeoutMs).kind == Exchange::kOk) {
          selected_ = pid;
          return textResult(json({{"launched", pid}, {"selected", pid}}).dump());
        }
      }
      if (!ActiveWidgetsMarker::isProcessAlive(pid)) {
        return textResult("PlotJuggler exited during startup. Its output:\n" + logHead(log), true);
      }
    }
    return textResult(
        "PlotJuggler started (pid " + std::to_string(pid) + ") but did not open its MCP port in 60 s. Its output:\n" +
            logHead(log),
        true);
  }

  json forward(const json& params) {
    std::optional<Instance> target = currentInstance();
    if (!target && selected_ != 0) {
      selected_ = 0;
      return textResult("the PlotJuggler this session used has closed; call list_instances or launch", true);
    }
    if (!target) {
      const std::size_t live = liveInstances().size();
      if (live == 0) {
        return textResult(
            "PlotJuggler is not running with MCP enabled. Call launch to start it, or ask the user to start "
            "PlotJuggler with --mcp (or turn on Preferences → Appearance → \"Let AI agents control PlotJuggler\").",
            true);
      }
      return textResult(
          std::to_string(live) +
              " PlotJuggler windows are running: call list_instances, then select_instance (ask the user which one "
              "when it is not obvious).",
          true);
    }
    selected_ = target->pid;  // stay on it even if another one starts later
    const Exchange answer = post(*target, request("tools/call", params), client_, kCallTimeoutMs);
    switch (answer.kind) {
      case Exchange::kOk:
        if (answer.reply.contains("error")) {
          return textResult(answer.reply["error"].value("message", "error"), true);
        }
        return answer.reply.value("result", json::object());
      case Exchange::kRefused:
        if (!ActiveWidgetsMarker::isProcessAlive(target->pid)) {
          QFile::remove(target->file);
        }
        selected_ = 0;
        return textResult(
            "the PlotJuggler this session used is gone (closed or crashed); its state is lost. Call list_instances "
            "or launch",
            true);
      case Exchange::kTimeout:
        return textResult(
            "PlotJuggler did not answer within " + std::to_string(kCallTimeoutMs / 1000) +
                " s (a long import, or the user is answering a dialog). It keeps working: poll get_status.",
            true);
      case Exchange::kClosed:
        return textResult("connection closed (PlotJuggler quit or MCP was turned off); call list_instances", true);
      case Exchange::kBadReply:
        break;
    }
    return textResult("PlotJuggler refused the call: " + answer.status.toStdString(), true);
  }

  std::string client_;
  qint64 selected_ = 0;
};

}  // namespace

int runBridge(int argc, char* argv[], const QString& version) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationVersion(version);
  QCoreApplication::setOrganizationName(u"PlotJuggler"_s);  // the same discovery folder as the app
  QCoreApplication::setApplicationName(u"PlotJuggler4"_s);
  Bridge bridge;
  std::string line;
  bool too_long = false;
  // One message per line, at most kMaxBodyBytes: the rest of a longer line is read and dropped.
  const auto read_line = [&line, &too_long] {
    line.clear();
    too_long = false;
    int c = 0;
    while ((c = std::cin.get()) != EOF && c != '\n') {
      if (static_cast<qsizetype>(line.size()) < kMaxBodyBytes) {
        line.push_back(static_cast<char>(c));
      } else {
        too_long = true;
      }
    }
    return c != EOF || !line.empty();
  };
  while (read_line()) {
    const RpcMessage message =
        too_long ? RpcMessage{.error = errorResponse(nullptr, -32600, "message over 1 MiB")} : parseRpcMessage(line);
    json reply;
    try {
      if (message.error) {
        reply = *message.error;
      } else if (message.notification) {
        continue;
      } else {
        bridge.noteClient(message.params);
        const json& params = message.params;
        reply = {{"jsonrpc", "2.0"}, {"id", message.id}};
        if (auto result = lifecycleResult(message.method, params, ToolSet::kFull)) {
          // The instance's own instructions say what it has loaded. Both eras carry them: a 2026 client (Claude
          // Code) reads them from server/discover alone.
          if (result->contains("instructions")) {
            if (const std::optional<json> own = bridge.askInstance(message.method, params);
                own && own->value("instructions", json()).is_string()) {
              (*result)["instructions"] = (*own)["instructions"];
            }
          }
          reply["result"] = std::move(*result);
        } else if (message.method == "tools/list") {
          reply["result"] = bridge.listTools();
        } else if (message.method == "tools/call" && params.value("name", json()).is_string()) {
          reply["result"] = bridge.callTool(params);
        } else if (message.method == "tools/call") {
          reply = errorResponse(message.id, -32602, "tools/call needs a string name");
        } else {
          reply = errorResponse(message.id, -32601, "method not found: " + message.method);
        }
      }
    } catch (const std::exception& e) {
      reply = errorResponse(message.id, -32603, std::string("internal error: ") + e.what());
    }
    std::cout << reply.dump(-1, ' ', false, json::error_handler_t::replace) << '\n' << std::flush;
  }
  return 0;
}

}  // namespace PJ::mcp
