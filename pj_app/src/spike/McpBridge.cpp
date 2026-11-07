// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// SPIKE: `plotjuggler4 --mcp-bridge`, MCP over stdio in front of the running instances'
// HTTP servers. One bridge process per agent session, so its instance choice is per session.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QProcess>
#include <QTcpSocket>
#include <QThread>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "spike/McpProtocol.h"

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <signal.h>  // kill(pid, 0)
#endif

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)
using namespace Qt::StringLiterals;

namespace PJ::mcp {
namespace {

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
    if (entry.is_object()) {
      out.push_back(
          {.pid = entry.value("pid", qint64{0}),
           .port = entry.value("port", quint16{0}),
           .token = entry.value("token", ""),
           .started_ms = entry.value("started_ms", qint64{0}),
           .file = info.absoluteFilePath()});
    }
  }
  std::sort(out.begin(), out.end(), [](const Instance& a, const Instance& b) { return a.started_ms < b.started_ms; });
  return out;
}

/// POSTs one JSON-RPC message and returns the reply, or nullopt when the instance is gone
/// (refused, closed without a reply, or no longer accepting this token). Blocks until the
/// instance answers: a held reply (a long import, a dialog the user is filling in) holds the bridge.
std::optional<json> post(const Instance& instance, const json& message, const std::string& owner) {
  QTcpSocket socket;
  socket.connectToHost(QHostAddress::LocalHost, instance.port);
  if (!socket.waitForConnected(2000)) {
    return std::nullopt;
  }
  const QByteArray body = QByteArray::fromStdString(message.dump());
  socket.write(
      "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
      "Accept: application/json\r\nAuthorization: Bearer " +
      QByteArray::fromStdString(instance.token) + "\r\nX-PJ-Owner: " + QByteArray::fromStdString(owner) +
      "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
  QByteArray response;
  while (socket.waitForReadyRead(-1)) {  // false once the server closed (Connection: close)
    response += socket.readAll();
  }
  response += socket.readAll();
  const qsizetype header_end = response.indexOf("\r\n\r\n");
  if (header_end < 0 || !response.startsWith("HTTP/1.1 200")) {
    return std::nullopt;
  }
  json reply = json::parse(response.mid(header_end + 4).toStdString(), nullptr, false);
  return reply.is_object() ? std::optional<json>(std::move(reply)) : std::nullopt;
}

bool processAlive(qint64 pid) {
#ifdef Q_OS_WIN
  HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  DWORD code = 0;
  const bool alive = handle != nullptr && GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
  if (handle != nullptr) {
    CloseHandle(handle);
  }
  return alive;
#else
  return ::kill(static_cast<pid_t>(pid), 0) == 0;  // the detached child is init's, never our zombie
#endif
}

/// The start of a launch log, for an error the agent can act on: the cause comes first, the
/// crash handler's backtrace after it.
std::string logHead(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.read(1200).toStdString() : std::string();
}

json textResult(const std::string& text, bool error = false) {
  json result = {{"content", json::array({{{"type", "text"}, {"text", text}}})}};
  if (error) {
    result["isError"] = true;
  }
  return result;
}

json bridgeTools() {
  const auto annotations = [](bool read_only) { return json{{"readOnlyHint", read_only}, {"openWorldHint", false}}; };
  return json::array({
      {{"name", "list_instances"},
       {"description",
        "PlotJuggler windows running with MCP on this computer: id, loaded datasets, whether it has the user's "
        "focus and whether this session is using it. Only needed when several run."},
       {"inputSchema", {{"type", "object"}, {"properties", json::object()}}},
       {"annotations", annotations(true)}},
      {{"name", "select_instance"},
       {"description", "Use the PlotJuggler instance with this id (from list_instances) for every later call."},
       {"inputSchema",
        {{"type", "object"}, {"properties", {{"id", {{"type", "integer"}}}}}, {"required", json::array({"id"})}}},
       {"annotations", annotations(false)}},
      {{"name", "launch"},
       {"description",
        "Start a new PlotJuggler window (when none runs, or the user wants a separate one) and use it. Returns "
        "once it is ready for other tools."},
       {"inputSchema", {{"type", "object"}, {"properties", json::object()}}},
       {"annotations", annotations(false)}},
  });
}

class Bridge {
 public:
  /// Every client request may name the client (initialize, or a 2026 request's _meta).
  void noteClient(const json& params) {
    if (const std::string client = clientName(params); !client.empty()) {
      owner_ = ownerFor(client);
    }
  }

  json handle(const std::string& method, const json& params) {
    noteClient(params);
    if (method == "tools/list") {
      json tools = toolList();
      for (json& tool : bridgeTools()) {
        tools.push_back(std::move(tool));
      }
      return {{"tools", tools}};
    }
    const std::string name = params.value("name", "");
    const json args = params.value("arguments", json::object());
    if (name == "list_instances") {
      return listInstances();
    }
    if (name == "select_instance") {
      const qint64 id = args.value("id", qint64{0});
      for (const Instance& instance : readInstances()) {
        if (instance.pid == id && post(instance, ping(), owner_)) {
          selected_ = id;
          return textResult(json({{"selected", id}}).dump());
        }
      }
      return textResult("no running PlotJuggler with id " + std::to_string(id) + " (see list_instances)", true);
    }
    if (name == "launch") {
      return launch();
    }
    return forward(params);
  }

 private:
  static json ping() {
    return {{"jsonrpc", "2.0"}, {"id", 0}, {"method", "ping"}};
  }

  /// Live instances; a discovery file whose instance no longer answers is a crash leftover.
  std::vector<Instance> liveInstances() const {
    std::vector<Instance> live;
    for (const Instance& instance : readInstances()) {
      if (post(instance, ping(), owner_)) {
        live.push_back(instance);
      } else {
        QFile::remove(instance.file);
      }
    }
    return live;
  }

  json listInstances() const {
    json out = json::array();
    for (const Instance& instance : liveInstances()) {
      json entry = {{"id", instance.pid}, {"selected", selected_ == instance.pid}};
      const json call = {
          {"jsonrpc", "2.0"},
          {"id", 0},
          {"method", "tools/call"},
          {"params", {{"name", "get_status"}, {"arguments", json::object()}}}};
      if (auto reply = post(instance, call, owner_); reply && reply->contains("result")) {
        const json status = json::parse((*reply)["result"]["content"][0].value("text", "{}"), nullptr, false);
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
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments({u"--mcp"_s});
    // stdout is this bridge's MCP channel: the child must never write to it.
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(log);
    process.setStandardErrorFile(log);
    qint64 pid = 0;
    if (!process.startDetached(&pid)) {
      return textResult("could not start " + QCoreApplication::applicationFilePath().toStdString(), true);
    }
    // ponytail: poll the discovery folder; a file watcher if startup latency ever matters.
    for (int waited_ms = 0; waited_ms < 60000; waited_ms += 200) {
      QThread::msleep(200);
      for (const Instance& instance : readInstances()) {
        if (instance.pid == pid && post(instance, ping(), owner_)) {
          selected_ = pid;
          return textResult(json({{"launched", pid}, {"selected", pid}}).dump());
        }
      }
      if (!processAlive(pid)) {
        return textResult("PlotJuggler exited during startup. Its output:\n" + logHead(log), true);
      }
    }
    return textResult(
        "PlotJuggler started (pid " + std::to_string(pid) + ") but did not open its MCP port in 60 s. Its output:\n" +
            logHead(log),
        true);
  }

  json forward(const json& params) {
    std::optional<Instance> target;
    if (selected_ != 0) {
      for (const Instance& instance : readInstances()) {
        if (instance.pid == selected_) {
          target = instance;
        }
      }
      if (!target) {
        selected_ = 0;
        return textResult("the PlotJuggler this session used has closed; call list_instances or launch", true);
      }
    } else {
      const std::vector<Instance> live = liveInstances();
      if (live.empty()) {
        return textResult(
            "PlotJuggler is not running with MCP enabled. Call launch to start it, or ask the user to start "
            "PlotJuggler with --mcp.",
            true);
      }
      if (live.size() > 1) {
        return textResult(
            std::to_string(live.size()) +
                " PlotJuggler windows are running: call list_instances, then select_instance (ask "
                "the user which one when it is not obvious).",
            true);
      }
      target = live.front();
      selected_ = target->pid;  // stay on it even if another one starts later
    }
    const json call = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", params}};
    const std::optional<json> reply = post(*target, call, owner_);
    if (!reply) {
      // Forget it, so the next call picks again (the only live one, or asks to choose).
      QFile::remove(target->file);
      selected_ = 0;
      return textResult(
          "the PlotJuggler this session used did not answer (closed or crashed); its state is gone. Call "
          "list_instances or launch",
          true);
    }
    if (reply->contains("error")) {
      return textResult((*reply)["error"].value("message", "error"), true);
    }
    return reply->value("result", json::object());
  }

  std::string owner_ = "mcp-agent";
  qint64 selected_ = 0;
};

}  // namespace

int runBridge(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setOrganizationName(u"PlotJuggler"_s);  // same discovery folder as the app
  QCoreApplication::setApplicationName(u"PlotJuggler4"_s);
  Bridge bridge;
  std::string line;
  while (std::getline(std::cin, line)) {
    const json message = json::parse(line, nullptr, false);
    if (!message.is_object() || !message.contains("id")) {
      continue;  // notifications and garbage get no reply
    }
    const std::string method = message.value("method", "");
    const json params = message.value("params", json::object());
    json reply = {{"jsonrpc", "2.0"}, {"id", message["id"]}};
    if (auto result = lifecycleResult(method, params)) {
      bridge.noteClient(params);
      reply["result"] = std::move(*result);
    } else if (method == "tools/list" || method == "tools/call") {
      reply["result"] = bridge.handle(method, params);
    } else {
      reply["error"] = {{"code", -32601}, {"message", "method not found: " + method}};
    }
    std::cout << reply.dump(-1, ' ', false, json::error_handler_t::replace) << '\n' << std::flush;
  }
  return 0;
}

}  // namespace PJ::mcp
