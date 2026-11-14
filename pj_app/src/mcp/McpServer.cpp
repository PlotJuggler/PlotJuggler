// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "mcp/McpServer.h"

#include <QApplication>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QPointer>
#include <QRandomGenerator>
#include <QRhiWidget>
#include <QSaveFile>
#include <QScopedValueRollback>
#ifndef PJ_TARGET_WASM
#include <QTcpSocket>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "MainWindow.h"
#include "PluginPlotTabsController.h"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_runtime/ActiveWidgetsMarker.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/PluginRuntimeCatalog.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/agent_tools/derived_recipes.hpp"
#include "pj_widgets/GraphicsApi.h"

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)
using namespace Qt::StringLiterals;

namespace PJ {

static_assert(std::string_view(mcp::kAgentOwnerPrefix) == kAgentIdPrefix, "agents and plugins must not share ids");

namespace {

/// The indicator goes away this long after a client's last request (HTTP here is one request per
/// connection, so there is no "disconnected").
constexpr int kClientIdleMs = 120'000;
/// How often a deferred result (an object evaluation) is polled, as the Assistant's GUI tick does.
constexpr int kResumeIntervalMs = 50;
#ifndef PJ_TARGET_WASM
/// The socket property naming the token its request was accepted with, so a revoke can close it.
constexpr char kTokenProperty[] = "pj_mcp_token";
#endif
/// At most this many running jobs per owner (the next call is refused), and this many ended ones
/// kept for job_wait, each for kJobExpiry after it ended.
constexpr int kMaxRunningJobs = 8;
constexpr int kMaxEndedJobs = 32;
/// A job handed out as an MCP task advertised how long its result is kept (ttlMs), so the count above
/// does not evict it; only this many of a client's are kept, a bound no well-behaved client meets.
constexpr int kMaxEndedTasks = 256;
constexpr auto kJobExpiry = std::chrono::minutes(10);
/// Seconds job_kill waits for a stop to take effect (a load winding down) before it answers.
constexpr double kKillWaitS = 20.0;
/// A job's summary in jobs_list: its tool and arguments, cut here.
constexpr std::size_t kSummaryChars = 120;
/// loadedSummary's caps: topics listed, numeric field names per topic, and bytes (where the topics stop).
constexpr int kSummaryTopics = 60;
constexpr std::size_t kSummaryFields = 40;
constexpr std::size_t kSummaryBytes = 6000;

/// 64 hex digits from the system's secure generator.
QByteArray randomToken() {
  std::array<quint32, 8> random{};
  QRandomGenerator::system()->fillRange(random.data(), random.size());
  return QByteArray(reinterpret_cast<const char*>(random.data()), sizeof(random)).toHex();
}

/// The agent tool context over a toolbox session's services (the views a toolbox plugin gets), plus windowed reads of
/// `engine`, the session's data: the SDK's read_series_arrow has no time window.
agent_tools::ToolContext contextFrom(PJ_service_registry_t registry, const DataEngine& engine) {
  const sdk::ServiceRegistry services(registry);
  agent_tools::ToolContext context;
  context.read_window = [&engine](
                            sdk::FieldHandle field, std::int64_t first,
                            std::int64_t last) -> Expected<sdk::MaterializedSeriesView> {
    sdk::ArrowSchemaHolder schema;
    sdk::ArrowArrayHolder array;
    if (auto status = readSeriesArrowWindow(engine, field, first, last, schema.out(), array.out()); !status) {
      return unexpected(status.error());
    }
    return sdk::MaterializedSeriesView(std::move(schema), std::move(array));
  };
  context.scan_values = [&engine](
                            sdk::FieldHandle field, std::int64_t first, const agent_tools::ValueBlockSink& on_block) {
    return scanSeriesValues(engine, field, on_block, first);
  };
  context.host = services.get<sdk::ToolboxHostService>().value_or(sdk::ToolboxHostView{});
  context.dp = services.get<sdk::DataProcessorsHostService>().value_or(sdk::DataProcessorsHostView{});
  context.objects = services.get<sdk::ToolboxObjectReadHostService>().value_or(sdk::ToolboxObjectReadHostView{});
  context.playback = services.get<sdk::PlaybackHostService>().value_or(sdk::PlaybackHostView{});
  context.plot_tabs = services.get<sdk::PlotTabHostService>().value_or(sdk::PlotTabHostView{});
  context.viewport = services.get<sdk::ViewportHostService>().value_or(sdk::ViewportHostView{});
  if (const auto runtime = services.get<sdk::ToolboxRuntimeHostService>()) {
    context.notify_data_changed = [runtime = *runtime]() { runtime.notifyDataChanged(); };
  }
  return context;
}

/// Whether a job of `tool` answers its call before it ends (McpServer::answerCall): a stream's call is
/// answered when the stream runs, and the job then lives as long as the stream. Such a job is never
/// handed out as an MCP task, whose result must be the call's own.
bool answersBeforeItEnds(const std::string& tool) {
  return tool == "start_stream";
}

/// Whether a job of `tool` dies with the call that started it: an agent tool's does (nothing would
/// read its evaluation), and so does a question whose answer would delete data for a client that
/// left; other app work (an import, a stream, an export) carries on for its client to find.
bool diesWithItsCall(const std::string& tool) {
  return mcp::agentTools().find(tool) != nullptr || tool == "unload_file";
}

}  // namespace

McpServer::McpServer(MainWindow& window) : QObject(nullptr), window_(window) {
  idle_.setSingleShot(true);
  idle_.setInterval(kClientIdleMs);
  connect(&idle_, &QTimer::timeout, this, [this] {
    active_client_.clear();
    emit activeClientChanged({});
  });
  resume_tick_.setInterval(kResumeIntervalMs);
  connect(&resume_tick_, &QTimer::timeout, this, &McpServer::resumeDeferred);
#ifndef PJ_TARGET_WASM
  connect(&server_, &QTcpServer::newConnection, this, [this] {
    while (QTcpSocket* socket = server_.nextPendingConnection()) {
      if (open_sockets_ >= mcp::kMaxOpenSockets) {
        socket->abort();
        socket->deleteLater();
        continue;
      }
      ++open_sockets_;
      pending_.insert(socket, {});
      connect(socket, &QObject::destroyed, this, [this, socket] {
        --open_sockets_;
        pending_.remove(socket);
      });
      connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
      connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      // A client that never finishes its request must not hold a slot.
      QTimer::singleShot(mcp::kIncompleteRequestTimeoutMs, socket, [this, socket] {
        if (pending_.contains(socket)) {
          socket->abort();
        }
      });
    }
  });
  if (!server_.listen(QHostAddress::LocalHost)) {
    qCWarning(mcp::lcMcp).noquote() << "listen failed:" << server_.errorString();
    return;
  }
  qCInfo(mcp::lcMcp).noquote() << "listening on http://127.0.0.1:" + QString::number(server_.serverPort()) + "/mcp";
#endif
}

McpServer::~McpServer() {
#ifndef PJ_TARGET_WASM
  if (!discovery_path_.isEmpty()) {
    QFile::remove(discovery_path_);
  }
  server_.close();
  // Sockets are children of server_; their destroyed() handlers touch this object's members, which
  // are still alive here.
  for (QTcpSocket* socket : server_.findChildren<QTcpSocket*>()) {
    delete socket;
  }
#endif
  // A pending evaluation releases itself through the session's services: drop it first. A load's
  // receiver dies with this object, so its terminal reaches nothing.
  resume_tick_.stop();
  jobs_.clear();
  agent_sessions_.clear();
}

void McpServer::openPublicAccess() {
  if (publicAccess()) {
    return;
  }
  public_token_ = randomToken();
#ifndef PJ_TARGET_WASM
  // A crashed instance leaves its file (and token) behind: the bridge would skip it, but nothing
  // else ever removes it.
  const QDir instances(mcp::instancesDir());
  for (const QFileInfo& info : instances.entryInfoList({u"*.json"_s}, QDir::Files)) {
    bool numeric = false;
    const qint64 pid = info.completeBaseName().toLongLong(&numeric);
    if (numeric && !ActiveWidgetsMarker::isProcessAlive(pid)) {
      QFile::remove(info.absoluteFilePath());
    }
  }
  // The bridge's discovery file: owner-only, removed with public access.
  discovery_path_ = instances.filePath(u"%1.json"_s.arg(QCoreApplication::applicationPid()));
  QSaveFile file(discovery_path_);
  if (file.open(QIODevice::WriteOnly)) {
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    file.write(
        QByteArray::fromStdString(json({{"pid", QCoreApplication::applicationPid()},
                                        {"port", server_.serverPort()},
                                        {"token", public_token_.toStdString()},
                                        {"started_ms", QDateTime::currentMSecsSinceEpoch()}})
                                      .dump()));
  }
  if (!file.commit()) {
    qCWarning(mcp::lcMcp).noquote() << "cannot write the discovery file" << discovery_path_;
    discovery_path_.clear();
  }
#endif
}

void McpServer::closePublicAccess() {
  if (!publicAccess()) {
    return;
  }
#ifndef PJ_TARGET_WASM
  if (!discovery_path_.isEmpty()) {
    QFile::remove(discovery_path_);
    discovery_path_.clear();
  }
#endif
  dropCallsOf(std::exchange(public_token_, {}));
  agent_sessions_.clear();  // after the results pointing into them
  idle_.stop();
  active_client_.clear();
  emit activeClientChanged({});
}

QByteArray McpServer::addPluginToken(PJ_service_registry_t services, const QString& owner) {
  const QByteArray token = randomToken();
  plugin_tokens_.insert(token, {.services = services, .owner = owner});
  return token;
}

void McpServer::revoke(const QByteArray& token) {
  if (plugin_tokens_.remove(token)) {
    dropCallsOf(token);
  }
}

std::function<bool()> McpServer::stillAuthorized(const Caller& caller) {
  return [self = QPointer<McpServer>(this), caller] {
    if (self == nullptr || !self->authorized(caller)) {
      return false;
    }
    if (caller.job == 0) {
      return true;
    }
    const auto job = self->jobs_.find(caller.job);
    if (job == self->jobs_.end() || ended(job->second) || job->second.killing) {
      return false;
    }
    // A job that dies with its call is not wanted from the moment that call hung up, before the next
    // tick has noticed.
    const std::vector<Waiter>& waiters = job->second.waiters;
    return !diesWithItsCall(job->second.tool) || std::none_of(waiters.begin(), waiters.end(), [](const Waiter& w) {
      return w.kind == Waiter::Kind::kCall && w.connection == nullptr;
    });
  };
}

void McpServer::dropCallsOf(const QByteArray& token) {
  std::vector<quint64> ids;
  for (const auto& [id, job] : jobs_) {
    if (job.caller.token == token) {
      ids.push_back(id);
    }
  }
  // One at a time, each looked up again: a stop may revoke another token, which drops its jobs too.
  for (const quint64 id : ids) {
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) {
      continue;
    }
    if (it->second.kill || it->second.close_dialog) {
      // App work (a load, a question to the user) stops; its terminal still ends its job, which points
      // into no services.
      killJob(id, /*keep_partial=*/true);
    } else {
      // Its waiters go unanswered, and its continuation releases its evaluation through the token's
      // services, which may die right after this.
      dropJob(id);
    }
  }
  updateTick();
#ifndef PJ_TARGET_WASM
  for (QTcpSocket* socket : server_.findChildren<QTcpSocket*>()) {
    if (socket->property(kTokenProperty).toByteArray() == token) {
      socket->abort();
    }
  }
#endif
}

#ifndef PJ_TARGET_WASM
void McpServer::onReadyRead(QTcpSocket* socket) {
  auto it = pending_.find(socket);
  if (it == pending_.end()) {
    socket->readAll();  // one request per connection; anything after it is ignored
    return;
  }
  it->append(socket->readAll());
  mcp::HttpRequest request;
  const int parsed = mcp::parseHttpRequest(*it, request);
  if (parsed == 0) {
    return;
  }
  pending_.erase(it);
  if (parsed != 200) {
    socket->write(mcp::httpResponse(parsed));
    socket->disconnectFromHost();
    return;
  }
  handle(socket, request);
}

void McpServer::handle(QTcpSocket* socket, const mcp::HttpRequest& request) {
  const QPointer<QTcpSocket> guard(socket);
  const auto respond = [guard](int status, const QByteArray& body = {}) {
    if (guard != nullptr && guard->isOpen()) {  // a revoke closes its token's connections
      guard->write(mcp::httpResponse(status, body));
      guard->disconnectFromHost();
    }
  };
  // Every token is compared, each in constant time, so how long this takes does not say which
  // one (if any) matched.
  int refused = 401;
  Caller caller;
  caller.connection = socket;
  const auto accept = [&](const QByteArray& token) {
    const int access = mcp::checkAccess(request, token);
    if (access == 0) {
      caller.token = token;
    } else {
      refused = access;
    }
  };
  if (publicAccess()) {
    accept(public_token_);
  }
  for (auto it = plugin_tokens_.cbegin(); it != plugin_tokens_.cend(); ++it) {
    accept(it.key());
  }
  if (caller.token.isEmpty()) {
    respond(refused);
    return;
  }
  socket->setProperty(kTokenProperty, caller.token);
  if (request.method != "POST") {
    respond(405);
    return;
  }
  if (request.path != "/mcp" && !request.path.startsWith("/mcp?")) {
    respond(404);
    return;
  }
  // The bridge names its client in a header; a direct 2026 client names itself in _meta.
  dispatch(
      std::move(caller), request.body, QString::fromUtf8(request.headers.value(mcp::kClientHeader)),
      [respond](int status, const QByteArray& body) { respond(status, body); });
}
#endif

void McpServer::handleRelayed(
    const QByteArray& body, const QString& client, QObject* connection, std::function<void(QByteArray)> send) {
  const auto respond = [send = std::move(send)](int, const QByteArray& response) { send(response); };
  if (!publicAccess()) {
    const mcp::RpcMessage message = mcp::parseRpcMessage(body.toStdString());
    respond(200, QByteArray::fromStdString(mcp::errorResponse(message.id, -32001, "MCP access is closed").dump()));
    return;
  }
  dispatch(
      {.token = public_token_, .owner = {}, .connection = connection, .request_id = {}, .job = 0, .tasks = false}, body,
      client, respond);
}

void McpServer::dispatch(
    Caller caller, const QByteArray& body, const QString& header_client,
    std::function<void(int status, QByteArray body)> respond) {
  const auto send = [respond](const json& response) {
    respond(200, QByteArray::fromStdString(mcp::dumpJson(response)));
  };
  const mcp::RpcMessage message = mcp::parseRpcMessage(body.toStdString());
  try {
    if (message.error) {
      send(*message.error);
      return;
    }
    const mcp::ToolSet tool_set = isPublic(caller) ? mcp::ToolSet::kFull : mcp::ToolSet::kAgentOnly;
    // Only a public request takes the client's name: a plugin token acts as its plugin, whatever
    // name the client claims.
    if (tool_set == mcp::ToolSet::kFull) {
      QString client = QString::fromStdString(mcp::printableClientName(header_client.toStdString()));
      if (client.isEmpty()) {
        client = QString::fromStdString(mcp::printableClientName(mcp::clientName(message.params)));
      }
      if (message.method != "ping" && !message.notification) {
        noteClient(client);
      }
      caller.owner = QString::fromStdString(mcp::ownerFor(client.toStdString()));
    }
    if (message.notification) {
      if (message.method == "notifications/cancelled") {
        cancelRequest(caller, message.params.value("requestId", json()));
      }
      respond(202, {});
      return;
    }
    caller.request_id = message.id;
    caller.tasks = mcp::declaresTasks(message.params);
    const auto result = [send, id = message.id](const json& value) {
      send({{"jsonrpc", "2.0"}, {"id", id}, {"result", value}});
    };
    const json& params = message.params;
    if (auto lifecycle = mcp::lifecycleResult(message.method, params, tool_set)) {
      // An agent told what is loaded skips its list_topics and describe_topic turns.
      if (lifecycle->contains("instructions")) {
        (*lifecycle)["instructions"].get_ref<std::string&>() += loadedSummary(caller);
      }
      result(*lifecycle);
    } else if (message.method == "tools/list") {
      result({{"tools", mcp::toolList(tool_set)}});
    } else if (mcp::isTasksMethod(message.method)) {
      send(taskRequest(message, caller));
    } else if (message.method == "tools/call") {
      const auto name = params.find("name");
      const json args = params.value("arguments", json::object());
      if (name == params.end() || !name->is_string() || !args.is_object()) {
        send(mcp::errorResponse(message.id, -32602, "tools/call needs a string name and object arguments"));
        return;
      }
      const std::string tool = name->get<std::string>();
      QElapsedTimer timer;
      timer.start();
      // From the event loop, not from inside the connection's own signal: a tool may run a modal loop (a
      // prompt for the user), during which that connection can close and be deleted.
      QTimer::singleShot(0, this, [this, tool, args, caller, result, timer] {
        if (caller.connection == nullptr) {
          return;
        }
        callTool(tool, args, caller, [result, tool, timer](json r) {
          qCDebug(mcp::lcMcp).noquote() << QString::fromStdString(tool) << "answered in" << timer.elapsed() << "ms";
          result(r);
        });
      });
    } else {
      send(mcp::errorResponse(message.id, -32601, "method not found: " + message.method));
    }
  } catch (const std::exception& e) {
    send(mcp::errorResponse(message.id, -32603, std::string("internal error: ") + e.what()));
  }
}

void McpServer::noteClient(const QString& client) {
  const QString shown = client.isEmpty() ? tr("an MCP client") : client;
  idle_.start();
  if (shown != active_client_) {
    active_client_ = shown;
    emit activeClientChanged(shown);
  }
}

std::string McpServer::shownTitle(const QWidget& widget) {
  return widget.windowTitle().remove(u"[*]"_s).toStdString();
}

json McpServer::userDialogOpen(const QWidget& modal) {
  return mcp::textResult(
      "the user has the dialog '" + shownTitle(modal) +
          "' open in PlotJuggler; wait for them to close it, then try again",
      true);
}

void McpServer::callTool(const std::string& name, const json& args, const Caller& caller, Reply reply) {
  quint64 job = 0;  // the job this call became, which then owns its reply
  try {
    // A modal dialog belongs to the user: acting under it would change state they are editing. Stopping
    // an agent's own work does not.
    const QWidget* modal = QApplication::activeModalWidget();
    if (modal != nullptr && !mcp::runsUnderUsersDialog(name, args)) {
      reply(userDialogOpen(*modal));
      return;
    }
    static const std::unordered_map<std::string, AppTool> job_tools = {
        {"jobs_list", &McpServer::jobsList}, {"job_wait", &McpServer::jobWait}, {"job_kill", &McpServer::jobKill}};
    if (const auto tool = job_tools.find(name); tool != job_tools.end()) {
      // `reply` is copied, not moved: when the tool throws on a wrongly typed argument (which it reads
      // before it answers), the handler below still has it to answer with.
      (this->*tool->second)(args, caller, reply);
      return;
    }
    const bool agent_tool = mcp::agentTools().find(name) != nullptr;
    const auto app_tool = !agent_tool && isPublic(caller) ? appTools().find(name) : appTools().end();
    if (!agent_tool && app_tool == appTools().end()) {
      reply(mcp::textResult("unknown tool " + name, true));
      return;
    }
    std::optional<agent_tools::ToolContext> context = agent_tool ? contextFor(caller) : std::nullopt;
    if (agent_tool && !context) {
      reply(mcp::textResult(mcp::kNoServicesError, true));
      return;
    }
    if (!mcp::isJobCapableTool(name)) {
      if (!agent_tool) {
        (this->*app_tool->second)(args, caller, reply);
        return;
      }
      const agent_tools::ToolResult result = mcp::agentTools().execute(name, args, *context);
      // Only a job can be resumed: a tool that defers belongs in mcp::isJobCapableTool.
      reply(
          result.resume ? mcp::textResult(name + " deferred its result but is not job-capable", true)
                        : mcp::textResult(result.content, !result.ok));
      return;
    }
    // The job comes first, so a call over the limit is refused before its work starts.
    job = openJob(name, args, caller, std::move(reply));
    if (job == 0) {
      return;
    }
    if (agent_tool) {
      continueJob(job, mcp::agentTools().execute(name, args, *context));
      return;
    }
    Caller as_job = caller;
    as_job.job = job;
    // Guarded: the tool may answer from a dialog that outlives this server.
    (this->*app_tool->second)(args, as_job, [self = QPointer<McpServer>(this), job](json result) {
      if (self != nullptr) {
        self->finishJob(job, std::move(result));
      }
    });
  } catch (const std::exception& e) {
    // A wrongly typed argument throws out of nlohmann; escaping a Qt slot would abort the app.
    json failure = mcp::textResult("bad arguments for " + name + ": " + e.what(), true);
    if (job != 0) {
      finishJob(job, std::move(failure));
    } else {
      reply(std::move(failure));
    }
  }
}

std::optional<agent_tools::ToolContext> McpServer::agentContext(const QString& owner) {
  std::shared_ptr<ToolboxPanelSession>& session = agent_sessions_[owner];
  if (session == nullptr) {
    const QString label = tr("AI agent (%1)").arg(owner);
    window_.appSession().extensionCatalog().registerAdHocOwner(owner, label);
    session = window_.buildAgentSession(owner, label);
    if (session == nullptr) {
      agent_sessions_.remove(owner);
      return std::nullopt;
    }
  }
  return contextFrom(MainWindow::toolboxSessionServices(*session), window_.appSession().sessionManager().dataEngine());
}

std::string McpServer::loadedSummary(const Caller& caller) {
  std::optional<agent_tools::ToolContext> context = contextFor(caller);
  if (!context) {
    return {};
  }
  // One snapshot walked here: each tool call would deep-copy the whole catalog again.
  auto catalog = context->host.catalogSnapshotV2();
  if (!catalog || catalog->dataSources().empty()) {
    return {};
  }
  const auto topics = catalog->topics();
  const auto fields = catalog->fields();
  // A name from a file lands in the agent's system prompt: keep it on its line.
  const auto one_line = [](std::string_view text) {
    std::string line(text);
    std::replace_if(
        line.begin(), line.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; }, ' ');
    return line;
  };
  std::map<std::string_view, int> name_uses;
  for (const PJ_data_source_info_t& source : catalog->dataSources()) {
    ++name_uses[PJ::sdk::toStringView(source.name)];
  }

  const SessionManager& manager = window_.appSession().sessionManager();
  std::string out =
      "\n\nLoaded when this session connected (list_topics and get_status show later changes), times in display "
      "seconds:";
  std::string example;
  int listed = 0;
  bool more = false;
  const auto room = [&] {
    more = more || listed >= kSummaryTopics || out.size() > kSummaryBytes;
    return !more;
  };
  for (const PJ_data_source_info_t& source : catalog->dataSources()) {
    // The source name, as the tools' "dataset:" qualifier takes it (the display name may carry a user's label).
    const std::string dataset = one_line(PJ::sdk::toStringView(source.name));
    const bool shared = name_uses[PJ::sdk::toStringView(source.name)] > 1;
    out += "\n- " + dataset;
    if (const auto range = manager.datasetDisplayRange(source.handle.id)) {
      out += QStringLiteral(": %1 to %2 (%3 s)")
                 .arg(range->min.value, 0, 'f', 3)
                 .arg(range->max.value, 0, 'f', 3)
                 .arg(range->max.value - range->min.value, 0, 'f', 1)
                 .toStdString();
    }
    if (shared) {
      out += " (another loaded dataset has this name, so a dataset: prefix cannot pick it)";
    }
    const std::size_t topic_end = std::min<std::size_t>(source.first_topic + source.topic_count, topics.size());
    for (std::size_t ti = source.first_topic; ti < topic_end && room(); ++ti, ++listed) {
      const PJ_topic_info_t& topic = topics[ti];
      const std::string name = one_line(PJ::sdk::toStringView(topic.name));
      std::vector<std::string> numeric;
      for (std::size_t fi = topic.first_field; fi < topic.first_field + topic.field_count && fi < fields.size(); ++fi) {
        if (agent_tools::isSignalField(PJ::sdk::fromAbiType(fields[fi].type), PJ::sdk::toStringView(fields[fi].name))) {
          numeric.push_back(one_line(PJ::sdk::toStringView(fields[fi].name)));
        }
      }
      out += "\n  " + name + ": " + std::to_string(numeric.size()) + " numeric fields";
      for (std::size_t i = 0; i < numeric.size() && i < kSummaryFields; ++i) {
        out += (i == 0 ? ": " : ", ") + numeric[i];
      }
      if (numeric.size() > kSummaryFields) {
        out += ", ...";
      }
      if (example.empty() && !numeric.empty() && !shared) {
        example = agent_tools::qualifyWithDataset(dataset, agent_tools::joinSeriesPath(name, numeric.front()));
      }
    }
    for (const PJ_object_topic_info_t& object : catalog->objectTopics()) {
      const std::string_view name = PJ::sdk::toStringView(object.name);
      if (object.source.id != source.handle.id || agent_tools::isMarkerObjectTopic(name)) {
        continue;
      }
      if (!room()) {
        break;
      }
      ++listed;
      out += "\n  " + one_line(name) + " (object)";
    }
  }
  if (more) {
    out += "\n(more topics: list_topics)";
  }
  if (!example.empty()) {
    out += "\nA series path is topic/field; prefix the dataset, as in " + example + ", when names repeat.";
  }
  return out;
}

std::optional<agent_tools::ToolContext> McpServer::contextFor(const Caller& caller) {
  std::optional<agent_tools::ToolContext> context;
  QString owner = caller.owner;
  if (isPublic(caller)) {
    context = agentContext(owner);
  } else if (const auto token = plugin_tokens_.constFind(caller.token); token != plugin_tokens_.cend()) {
    context = contextFrom(token->services, window_.appSession().sessionManager().dataEngine());
    owner = token->owner;
  }
  if (context) {
    // pj.viewport.v1 frames every tab the caller owns; plot_tab's zoom names one.
    context->zoom_tab = [this, owner](const std::string& tab, std::optional<std::pair<double, double>> range) {
      PluginPlotTabsController& tabs = window_.pluginPlotTabs();
      const QString tab_id = QString::fromStdString(tab);
      return range ? tabs.zoomToTimeRange(owner, range->first, range->second, tab_id) : tabs.zoomOut(owner, tab_id);
    };
  }
  return context;
}

int McpServer::runningJobs(const Caller& caller) const {
  return static_cast<int>(std::count_if(jobs_.begin(), jobs_.end(), [&](const auto& entry) {
    return sameClient(entry.second.caller, caller) && !ended(entry.second);
  }));
}

quint64 McpServer::openJob(const std::string& tool, const json& args, const Caller& caller, Reply reply) {
  if (runningJobs(caller) >= kMaxRunningJobs) {
    reply(
        mcp::textResult(
            "you already have " + std::to_string(kMaxRunningJobs) +
                " jobs running: jobs_list shows them; job_wait for one to end, or job_kill one",
            true));
    return 0;
  }
  Job job;
  job.id = ++last_job_id_;
  job.caller = caller;
  job.client = isPublic(caller) ? caller.owner : plugin_tokens_.value(caller.token).owner;
  job.tool = tool;
  // The tool and its arguments, each cut short: a script argument may run to a megabyte.
  job.summary = tool;
  for (const auto& [key, value] : args.items()) {
    if (key == "wait_s" || job.summary.size() > kSummaryChars) {
      continue;
    }
    const std::string text = value.is_string() ? value.get<std::string>() : mcp::dumpJson(value);
    job.summary += " " + key + "=" + text.substr(0, kSummaryChars);
  }
  if (job.summary.size() > kSummaryChars) {
    job.summary.resize(kSummaryChars);
    job.summary += "...";
  }
  job.started = Clock::now();
  job.created = QDateTime::currentDateTimeUtc();
  job.round = resume_round_;
  addWaiter(job, Waiter::Kind::kCall, caller, std::move(reply), mcp::waitSeconds(args));
  const quint64 id = job.id;
  jobs_.emplace(id, std::move(job));
  updateTick();
  emit jobsChanged();
  return id;
}

void McpServer::addWaiter(Job& job, Waiter::Kind kind, const Caller& caller, Reply reply, double seconds) {
  job.waiters.push_back(
      {.kind = kind,
       .reply = std::move(reply),
       .connection = caller.connection,
       .deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds))});
}

McpServer::Job* McpServer::liveJob(quint64 id) {
  const auto it = jobs_.find(id);
  return it != jobs_.end() && !ended(it->second) ? &it->second : nullptr;
}

void McpServer::continueJob(quint64 id, agent_tools::ToolResult result) {
  Job* job = liveJob(id);
  if (job == nullptr) {
    return;  // killed or revoked while it ran: a follow-up dies here, unqueued
  }
  if (!authorized(job->caller)) {
    dropJob(id);  // a follow-up would point into services that may die next
    return;
  }
  if (!result.resume) {
    finishJob(id, mcp::textResult(result.content, !result.ok));
    return;
  }
  job->resume = std::move(result.resume);
  job->round = resume_round_;
  updateTick();
}

void McpServer::finishJob(quint64 id, json result) {
  const auto it = jobs_.find(id);
  if (it == jobs_.end() || ended(it->second)) {
    return;
  }
  Job& job = it->second;
  // What happened decides, not what was asked: a load that completed before its stop arrived is completed.
  const bool failed = result.value("isError", false);
  job.state = !failed ? JobState::kCompleted : job.killing ? JobState::kKilled : JobState::kFailed;
  job.ended_at = Clock::now();
  job.result = std::move(result);
  // The table is settled before anything of the job runs or dies: an ended record keeps no hook
  // (they point into services), and a waiter is out of it before its reply runs.
  struct {
    std::function<agent_tools::ToolResult(agent_tools::ToolContext&)> resume;
    std::function<void(bool)> kill;
    std::function<void()> close_dialog;
    std::function<json()> progress;
  } hooks{
      std::exchange(job.resume, nullptr), std::exchange(job.kill, nullptr), std::exchange(job.close_dialog, nullptr),
      std::exchange(job.progress, nullptr)};
  std::vector<Waiter> waiters = std::exchange(job.waiters, {});
  const auto any = [&waiters](Waiter::Kind kind, bool connected) {
    return std::any_of(waiters.begin(), waiters.end(), [&](const Waiter& w) {
      return w.kind == kind && (w.connection != nullptr) == connected;
    });
  };
  // An import whose call hung up since the last tick is kept for its client, as serviceWaiters would have.
  job.retained = job.retained || (any(Waiter::Kind::kCall, false) && !diesWithItsCall(job.tool));
  const json killed = any(Waiter::Kind::kKill, true) ? killReply(job) : json();
  const json final_result = job.result;
  // Nobody knows this job's id, or its token is revoked: nobody would read the record.
  if (!job.retained || !authorized(job.caller)) {
    jobs_.erase(it);
  }
  pruneJobs();
  updateTick();
  emit jobsChanged();
  hooks = {};  // destroying the continuation releases its evaluation, which stops the work
  for (Waiter& waiter : waiters) {
    if (waiter.connection != nullptr) {
      waiter.reply(waiter.kind == Waiter::Kind::kKill ? killed : final_result);
    }
  }
}

void McpServer::dropJob(quint64 id) {
  if (const auto it = jobs_.find(id); it != jobs_.end()) {
    const Job dropped = std::move(it->second);
    jobs_.erase(it);
    emit jobsChanged();
  }
}

std::vector<McpServer::JobInfo> McpServer::unfinishedJobs() const {
  std::vector<JobInfo> jobs;
  for (const auto& [id, job] : jobs_) {
    if (!ended(job)) {
      jobs.push_back(
          {.id = id,
           .client = job.client,
           .summary = QString::fromStdString(job.summary),
           .state = QString::fromLatin1(stateName(job)),
           .elapsed_s = elapsedSeconds(job)});
    }
  }
  return jobs;
}

void McpServer::stopJob(quint64 id) {
  killJob(id, /*keep_partial=*/true);
}

void McpServer::killJob(quint64 id, bool keep_partial) {
  const auto it = jobs_.find(id);
  if (it == jobs_.end() || ended(it->second) || it->second.killing) {
    return;  // the first stop decides keep_partial; its outcome is on its way
  }
  it->second.killing = true;
  const std::function<void()> close_dialog = it->second.close_dialog;
  const std::function<void(bool)> kill = it->second.kill;
  if (!close_dialog && !kill) {
    // A continuation dies with its record; one running right now finds the job ended when it returns. App
    // work that has not started asks stillAuthorized first.
    finishJob(id, mcp::textResult("killed", true));
    return;
  }
  // The terminal result arrives through the tool's reply: the dialog's rejection, a load's loadFinished.
  if (close_dialog) {
    // From the event loop, as the user's click comes: the rejection resumes whatever waits on the dialog,
    // and is the work's whole stop (a load at its dialog is not also cancelled).
    QTimer::singleShot(0, this, close_dialog);
  } else {
    kill(keep_partial);
  }
}

dialog_presenter::DialogWatch McpServer::dialogWatch(quint64 id) {
  return [self = QPointer<McpServer>(this), id](dialog_presenter::DialogCancelFn close) {
    Job* job = self != nullptr ? self->liveJob(id) : nullptr;
    if (job == nullptr) {
      return;
    }
    job->close_dialog = std::move(close);
    if (job->killing && job->close_dialog) {
      QTimer::singleShot(0, self, job->close_dialog);  // stopped while its dialog was opening
    }
  };
}

void McpServer::answerCall(quint64 id, const json& result) {
  Job* live = liveJob(id);
  if (live == nullptr) {
    return;
  }
  Job& job = *live;
  job.retained = true;  // whoever asked knows the id from here on, or finds it in jobs_list
  std::vector<Waiter> calls;
  for (auto waiter = job.waiters.begin(); waiter != job.waiters.end();) {
    if (waiter->kind == Waiter::Kind::kCall) {
      calls.push_back(std::move(*waiter));
      waiter = job.waiters.erase(waiter);
    } else {
      ++waiter;
    }
  }
  updateTick();
  for (Waiter& call : calls) {
    if (call.connection != nullptr) {
      call.reply(result);
    }
  }
}

McpServer::Job* McpServer::ownedJob(const Caller& caller, const json& id) {
  pruneJobs();
  if (!id.is_number_integer()) {
    return nullptr;
  }
  const auto it = jobs_.find(id.get<quint64>());
  // Another client's job reads as missing: its id says nothing about it.
  return it != jobs_.end() && sameClient(it->second.caller, caller) ? &it->second : nullptr;
}

const char* McpServer::stateName(const Job& job) {
  switch (job.state) {
    case JobState::kRunning:
      return job.close_dialog ? "waiting_for_user" : "running";  // its own dialog is up
    case JobState::kWaitingForUser:
      return "waiting_for_user";
    case JobState::kCompleted:
      return "completed";
    case JobState::kFailed:
      return "failed";
    case JobState::kKilled:
      return "killed";
  }
  return "";
}

double McpServer::elapsedSeconds(const Job& job) {
  const Clock::time_point until = ended(job) ? job.ended_at : Clock::now();
  return std::round(std::chrono::duration<double>(until - job.started).count() * 10.0) / 10.0;
}

json McpServer::jobStatus(const Job& job) {
  json status = {
      {"job_id", job.id},
      {"tool", job.tool},
      {"summary", job.summary},
      {"state", stateName(job)},
      {"elapsed_s", elapsedSeconds(job)}};
  if (job.progress) {
    status["progress"] = job.progress();
  }
  if (job.killing && !ended(job)) {
    status["stopping"] = true;  // asked to stop, still winding down
  }
  return status;
}

json McpServer::killReply(const Job& job) {
  json status = jobStatus(job);
  const json content = job.result.value("content", json::array());
  if (!content.empty() && content[0].contains("text")) {
    status["result"] = content[0]["text"];
  }
  return mcp::jsonResult(status);
}

void McpServer::pruneJobs() {
  const Clock::time_point now = Clock::now();
  std::vector<std::pair<Clock::time_point, quint64>> ended_jobs;
  for (const auto& [id, job] : jobs_) {
    if (ended(job)) {
      ended_jobs.emplace_back(job.ended_at, id);
    }
  }
  // The latest to end first, so each client keeps its most recent results: kMaxEndedJobs of them, and
  // apart from those the tasks, whose expiry was promised.
  std::sort(ended_jobs.rbegin(), ended_jobs.rend());
  std::map<std::tuple<QByteArray, QString, bool>, int> kept;
  for (const auto& [ended_at, id] : ended_jobs) {
    const Job& job = jobs_.at(id);
    const bool task = !job.task_id.empty();
    if (now - ended_at > kJobExpiry ||
        ++kept[{job.caller.token, job.caller.owner, task}] > (task ? kMaxEndedTasks : kMaxEndedJobs)) {
      jobs_.erase(id);
    }
  }
}

void McpServer::updateTick() {
  const bool needed = std::any_of(jobs_.begin(), jobs_.end(), [](const auto& entry) {
    return entry.second.resume || !entry.second.waiters.empty();
  });
  if (!needed) {
    resume_tick_.stop();
  } else if (!resume_tick_.isActive()) {
    resume_tick_.start();
  }
}

void McpServer::cancelRequest(const Caller& caller, const json& request_id) {
  if (!request_id.is_string() && !request_id.is_number_integer()) {
    return;
  }
  // A client may name itself on its calls and not on its notifications: an unnamed cancel is matched
  // on the token alone.
  const bool unnamed = caller.owner.toStdString() == mcp::ownerFor({});
  std::vector<quint64> matching;
  for (const auto& [id, job] : jobs_) {
    const bool foreground = std::any_of(
        job.waiters.begin(), job.waiters.end(), [](const Waiter& w) { return w.kind == Waiter::Kind::kCall; });
    const bool theirs = unnamed ? job.caller.token == caller.token : sameClient(job.caller, caller);
    if (foreground && !ended(job) && theirs && job.caller.request_id == request_id) {
      matching.push_back(id);
    }
  }
  // Two sessions of one client may reuse an id: stopping both would stop someone else's call.
  if (matching.size() == 1) {
    killJob(matching.front(), /*keep_partial=*/true);
  }
}

json McpServer::taskOf(Job& job) {
  if (job.task_id.empty()) {
    job.task_id = randomToken().toStdString();
  }
  const auto ms = [](Clock::duration time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time).count();
  };
  // A running job's progress moves all the time: what it reports is as of now.
  const QDateTime updated =
      ended(job) ? job.created.addMSecs(ms(job.ended_at - job.started)) : QDateTime::currentDateTimeUtc();
  json task = {
      {"taskId", job.task_id},
      // A tool error is a completed task whose result says isError: "failed" is for protocol errors only.
      {"status", !ended(job)                      ? "working"
                 : job.state == JobState::kKilled ? "cancelled"
                                                  : "completed"},
      {"createdAt", job.created.toString(Qt::ISODateWithMs).toStdString()},
      {"lastUpdatedAt", updated.toString(Qt::ISODateWithMs).toStdString()},
      // Kept while it runs, then until its result expires.
      {"ttlMs", ended(job) ? json(ms(job.ended_at - job.started + kJobExpiry)) : json()},
      {"pollIntervalMs", 1000}};
  if (!ended(job)) {
    if (job.close_dialog || job.state == JobState::kWaitingForUser) {
      task["statusMessage"] = "waiting for the user to answer in PlotJuggler";
    } else if (job.progress) {
      task["statusMessage"] = mcp::dumpJson(job.progress());
    }
  }
  return task;
}

json McpServer::taskRequest(const mcp::RpcMessage& request, const Caller& caller) {
  if (!caller.tasks) {
    return mcp::errorResponse(
        request.id, mcp::kMissingClientCapability,
        request.method + " needs the client capability " + mcp::kTasksExtension,
        {{"requiredCapabilities", {{"extensions", {{mcp::kTasksExtension, json::object()}}}}}});
  }
  pruneJobs();
  const json task_id = request.params.value("taskId", json());
  const auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const auto& entry) {
    const Job& job = entry.second;
    return !job.task_id.empty() && task_id.is_string() && job.task_id == task_id.get<std::string>() &&
           sameClient(job.caller, caller);
  });
  if (it == jobs_.end()) {
    // Another client's task reads as missing, like an expired one.
    return mcp::errorResponse(request.id, -32602, "Failed to retrieve task: Task not found");
  }
  json result = {{"resultType", "complete"}};
  if (request.method == "tasks/cancel") {
    killJob(it->first, /*keep_partial=*/true);  // cooperative: tasks/get says when it took effect
  } else if (request.method == "tasks/get") {
    result.update(taskOf(it->second));
    if (ended(it->second) && it->second.state != JobState::kKilled) {
      result["result"] = it->second.result;
    }
  }  // tasks/update: a job never asks its client for input, so nothing is outstanding to answer
  return {{"jsonrpc", "2.0"}, {"id", request.id}, {"result", std::move(result)}};
}

void McpServer::resumeDeferred() {
  if (draining_) {
    return;  // a reply or continuation re-entered: the outer loop and the next tick get its items
  }
  const QScopedValueRollback<bool> draining(draining_, true);
  serviceWaiters();
  const quint64 round = ++resume_round_;
  for (;;) {
    // One continuation at a time, the other jobs left in jobs_ for a revocation this one causes to drop.
    const auto due = std::find_if(jobs_.begin(), jobs_.end(), [round](const auto& entry) {
      return entry.second.resume && entry.second.round != round;
    });
    if (due == jobs_.end()) {
      break;
    }
    const quint64 id = due->first;
    Job& job = due->second;
    job.round = round;
    if (!authorized(job.caller)) {
      dropJob(id);  // nobody would read the result
      continue;
    }
    if (QApplication::activeModalWidget() != nullptr && !mcp::isReadOnlyTool(job.tool)) {
      job.state = JobState::kWaitingForUser;  // the user's dialog first; this waits, it does not fail
      continue;
    }
    job.state = JobState::kRunning;
    std::optional<agent_tools::ToolContext> context = contextFor(job.caller);
    if (!context) {
      finishJob(id, mcp::textResult(mcp::kNoServicesError, true));
      continue;
    }
    const auto running = jobs_.find(id);  // looked up again: building a context may reach back in here
    if (running == jobs_.end() || !running->second.resume) {
      continue;
    }
    auto resume = std::exchange(running->second.resume, nullptr);
    agent_tools::ToolResult next;
    try {
      next = resume(*context);
    } catch (const std::exception& e) {
      next = agent_tools::ToolResult::failure(std::string("tool failed: ") + e.what());
    }
    resume = nullptr;  // dies right after it ran, while the services it may release into live
    continueJob(id, std::move(next));
  }
  updateTick();
}

void McpServer::serviceWaiters() {
  const Clock::time_point now = Clock::now();
  std::vector<quint64> ids;
  for (const auto& [id, job] : jobs_) {
    if (!job.waiters.empty()) {
      ids.push_back(id);
    }
  }
  for (const quint64 id : ids) {
    const auto it = jobs_.find(id);
    // A revoked token's job only awaits its terminal: its client is not handed a job id it cannot use.
    if (it == jobs_.end() || !authorized(it->second.caller)) {
      continue;
    }
    Job& job = it->second;
    const bool dies_with_call = diesWithItsCall(job.tool);
    std::vector<std::pair<Reply, json>> due;
    bool abandoned = false;
    for (auto waiter = job.waiters.begin(); waiter != job.waiters.end();) {
      const bool call = waiter->kind == Waiter::Kind::kCall;
      if (waiter->connection == nullptr) {
        // Such a job dies with its call; any other carries on, kept for its client to find.
        abandoned = abandoned || (call && dies_with_call);
        job.retained = job.retained || (call && !dies_with_call);
        waiter = job.waiters.erase(waiter);
      } else if (now >= waiter->deadline) {
        job.retained = job.retained || call;
        json status = jobStatus(job);
        if (call) {
          status["note"] =
              "still running in the background: job_wait with this job_id gets its result, job_kill stops it";
        } else if (waiter->kind == Waiter::Kind::kKill) {
          status["note"] = "the stop has not taken effect yet: job_wait with this job_id returns when it has";
        }
        json answer = mcp::jsonResult(status);
        if (call && job.caller.tasks && !answersBeforeItEnds(job.tool)) {
          // A client that speaks MCP Tasks gets the handle of this job as a task instead.
          answer = taskOf(job);
          answer["resultType"] = "task";
        }
        due.emplace_back(std::move(waiter->reply), std::move(answer));
        waiter = job.waiters.erase(waiter);
      } else {
        ++waiter;
      }
    }
    for (auto& [reply, status] : due) {
      reply(status);
    }
    if (abandoned) {
      killJob(id, /*keep_partial=*/true);  // which also answers another session still waiting on it
    }
  }
}

void McpServer::jobsList(const json&, const Caller& caller, Reply reply) {
  pruneJobs();
  json jobs = json::array();
  for (auto& [id, job] : jobs_) {
    if (sameClient(job.caller, caller)) {
      job.retained = true;  // its id is known now: its result must be there to ask for
      jobs.push_back(jobStatus(job));
    }
  }
  reply(mcp::jsonResult(jobs));
}

void McpServer::jobWait(const json& args, const Caller& caller, Reply reply) {
  Job* job = ownedJob(caller, args.value("job_id", json()));
  if (job == nullptr) {
    reply(mcp::textResult("no such job (jobs_list shows yours; an ended job's result expires after a while)", true));
    return;
  }
  if (ended(*job)) {
    reply(job->result);
    return;
  }
  const double timeout = mcp::waitSeconds(args, "timeout_s", mcp::kMaxJobWaitS);
  if (!(timeout > 0.0)) {
    reply(mcp::jsonResult(jobStatus(*job)));
    return;
  }
  addWaiter(*job, Waiter::Kind::kWait, caller, std::move(reply), timeout);
  updateTick();
}

void McpServer::jobKill(const json& args, const Caller& caller, Reply reply) {
  // Read before `reply` joins the job's waiters: a wrongly typed argument throws, and callTool then
  // answers itself.
  const bool keep_partial = args.value("keep_partial", true);
  Job* job = ownedJob(caller, args.value("job_id", json()));
  if (job == nullptr) {
    reply(mcp::textResult("no such job (jobs_list shows yours)", true));
    return;
  }
  if (ended(*job)) {
    reply(killReply(*job));
    return;
  }
  const quint64 id = job->id;
  addWaiter(*job, Waiter::Kind::kKill, caller, std::move(reply), kKillWaitS);
  updateTick();
  killJob(id, keep_partial);
}

void McpServer::grabScreenshot(int max_width, std::function<void(const QByteArray&)> done) {
  struct Shot {
    std::vector<std::pair<QPointer<QRhiWidget>, QImage>> frames;
    std::size_t waiting = 0;
    std::function<void(const QByteArray&)> done;
  };
  auto shot = std::make_shared<Shot>();
  shot->done = std::move(done);
  for (QRhiWidget* view : window_.findChildren<QRhiWidget*>()) {
    if (view->isVisible() && view->window() == &window_) {
      shot->frames.emplace_back(view, QImage());
    }
  }
  shot->waiting = shot->frames.size();
  const QPointer<McpServer> self(this);
  const auto finish = [self, shot, max_width] {
    if (self == nullptr || !shot->done) {
      return;
    }
    QImage image = self->window_.grab().toImage();
    {
      QPainter painter(&image);
      for (const auto& [view, frame] : shot->frames) {
        if (view != nullptr && !frame.isNull()) {
          painter.drawImage(QRect(view->mapTo(&self->window_, QPoint(0, 0)), view->size()), frame);
        }
      }
      // The agent should see what the user is being asked. Wayland hides real window positions, so
      // the dialog is drawn centred over a dimmed window, as most compositors place it.
      if (QWidget* modal = QApplication::activeModalWidget(); modal != nullptr && modal != &self->window_) {
        painter.fillRect(image.rect(), QColor(0, 0, 0, 90));
        QImage dialog = modal->grab().toImage();
        if (dialog.width() > image.width() || dialog.height() > image.height()) {
          dialog = dialog.scaled(image.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        painter.drawImage(QPoint((image.width() - dialog.width()) / 2, (image.height() - dialog.height()) / 2), dialog);
      }
    }
    if (max_width > 0 && image.width() > max_width) {
      image = image.scaledToWidth(max_width, Qt::SmoothTransformation);
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    std::exchange(shot->done, nullptr)(png);
  };
  if (shot->waiting == 0) {
    finish();
    return;
  }
  for (std::size_t i = 0; i < shot->frames.size(); ++i) {
    grabPresentedFrame(*shot->frames[i].first, [shot, i, finish](const QImage& frame) {
      shot->frames[i].second = frame;
      if (--shot->waiting == 0) {
        finish();
      }
    });
  }
  // A view destroyed mid-grab never delivers; the others' 2 s timeout has passed by then.
  QTimer::singleShot(3000, this, finish);
}

quint32 McpServer::resolveDataset(const json& selector) const {
  AppSession& session = window_.appSession();
  for (const auto& [id, dataset_name] : session.catalogModel().datasets()) {
    if (selector.is_number_integer() && selector.get<qint64>() == static_cast<qint64>(id)) {
      return id;
    }
    if (selector.is_string()) {
      const QString wanted = QString::fromStdString(selector.get<std::string>());
      if (wanted == dataset_name || wanted == session.sessionManager().datasetSourcePath(id) ||
          wanted == QString::number(id)) {
        return id;
      }
    }
  }
  return 0;
}

#ifndef PJ_TARGET_WASM
McpEndpointHost::McpEndpointHost(std::function<McpServer*()> server, PJ_service_registry_t services, QString owner)
    : server_on_demand_(std::move(server)), services_(services), owner_(std::move(owner)) {}

McpEndpointHost::~McpEndpointHost() {
  release();
}

PJ_mcp_endpoint_host_t McpEndpointHost::raw() noexcept {
  static constexpr PJ_mcp_endpoint_host_vtable_t kVtable = {
      .protocol_version = 1,
      .struct_size = sizeof(PJ_mcp_endpoint_host_vtable_t),
      .acquire_endpoint = [](void* ctx, PJ_mcp_endpoint_t* out_endpoint, PJ_error_t* out_error) noexcept -> bool {
        if (ctx == nullptr || out_endpoint == nullptr) {
          sdk::fillError(out_error, PJ_ERROR_CODE_REJECTED, "mcp_endpoint", "null ctx or out_endpoint");
          return false;
        }
        try {
          return static_cast<McpEndpointHost*>(ctx)->acquire(*out_endpoint, out_error);
        } catch (const std::exception& e) {
          sdk::fillError(out_error, PJ_ERROR_CODE_INTERNAL, "mcp_endpoint", e.what());
          return false;
        }
      },
      .release_endpoint =
          [](void* ctx) noexcept {
            if (ctx != nullptr) {
              static_cast<McpEndpointHost*>(ctx)->release();
            }
          },
  };
  return {this, &kVtable};
}

bool McpEndpointHost::acquire(PJ_mcp_endpoint_t& out, PJ_error_t* out_error) {
  if (server_ == nullptr) {
    McpServer* server = server_on_demand_();
    if (server == nullptr) {
      sdk::fillError(out_error, PJ_ERROR_CODE_INTERNAL, "mcp_endpoint", "PlotJuggler's MCP server is not running");
      return false;
    }
    // Whatever can throw comes before the token: once it exists only release() forgets it.
    std::string url = "http://127.0.0.1:" + std::to_string(server->port()) + "/mcp";
    QPointer<McpServer> held(server);  // the first QPointer to an object allocates
    token_ = server->addPluginToken(services_, owner_);
    url_ = std::move(url);
    server_ = std::move(held);
  }
  out = {sdk::toAbiString(url_), sdk::toAbiString({token_.constData(), static_cast<std::size_t>(token_.size())})};
  return true;
}

void McpEndpointHost::release() {
  if (server_ != nullptr) {
    server_->revoke(token_);
    server_ = nullptr;
  }
  url_.clear();
  token_.clear();
}
#endif

}  // namespace PJ
