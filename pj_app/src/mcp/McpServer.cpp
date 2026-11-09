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
#include <QTcpSocket>
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
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
/// The socket property naming the token its request was accepted with, so a revoke can close it.
constexpr char kTokenProperty[] = "pj_mcp_token";
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
  context.scan_values = [&engine](sdk::FieldHandle field, const agent_tools::ValueBlockSink& on_block) {
    return scanSeriesValues(engine, field, on_block);
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
}

McpServer::~McpServer() {
  if (!discovery_path_.isEmpty()) {
    QFile::remove(discovery_path_);
  }
  server_.close();
  // Sockets are children of server_; their destroyed() handlers touch this object's members, which
  // are still alive here.
  for (QTcpSocket* socket : server_.findChildren<QTcpSocket*>()) {
    delete socket;
  }
  // A pending evaluation releases itself through the session's services: drop it first.
  resume_tick_.stop();
  deferred_.clear();
  agent_sessions_.clear();
}

void McpServer::openPublicAccess() {
  if (publicAccess()) {
    return;
  }
  public_token_ = randomToken();
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
}

void McpServer::closePublicAccess() {
  if (!publicAccess()) {
    return;
  }
  if (!discovery_path_.isEmpty()) {
    QFile::remove(discovery_path_);
    discovery_path_.clear();
  }
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
  return [self = QPointer<McpServer>(this), caller] { return self != nullptr && self->authorized(caller); };
}

void McpServer::dropCallsOf(const QByteArray& token) {
  // Each queued result releases itself through the token's services, which may die right after this.
  std::erase_if(deferred_, [&token](const Deferred& item) { return item.caller.token == token; });
  if (deferred_.empty()) {
    resume_tick_.stop();
  }
  for (QTcpSocket* socket : server_.findChildren<QTcpSocket*>()) {
    if (socket->property(kTokenProperty).toByteArray() == token) {
      socket->abort();
    }
  }
}

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
  caller.socket = guard;
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
  const auto send = [respond](const json& response) {
    respond(200, QByteArray::fromStdString(response.dump(-1, ' ', false, json::error_handler_t::replace)));
  };
  const mcp::RpcMessage message = mcp::parseRpcMessage(request.body.toStdString());
  try {
    if (message.error) {
      send(*message.error);
      return;
    }
    if (message.notification) {
      respond(202);
      return;
    }
    const mcp::ToolSet tool_set = isPublic(caller) ? mcp::ToolSet::kFull : mcp::ToolSet::kAgentOnly;
    // Only a public request takes the client's name: a plugin token acts as its plugin, whatever
    // name the client claims.
    if (tool_set == mcp::ToolSet::kFull) {
      // The bridge names its client in a header; a direct 2026 client names itself in _meta.
      QString client =
          QString::fromStdString(mcp::printableClientName(request.headers.value(mcp::kClientHeader).toStdString()));
      if (client.isEmpty()) {
        client = QString::fromStdString(mcp::printableClientName(mcp::clientName(message.params)));
      }
      if (message.method != "ping") {
        noteClient(client);
      }
      caller.owner = QString::fromStdString(mcp::ownerFor(client.toStdString()));
    }
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
      callTool(tool, args, caller, [result, tool, timer](json r) {
        qCDebug(mcp::lcMcp).noquote() << QString::fromStdString(tool) << "answered in" << timer.elapsed() << "ms";
        result(r);
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
  try {
    // A modal dialog belongs to the user: acting under it would change state they are editing.
    const QWidget* modal = QApplication::activeModalWidget();
    const bool read_only = mcp::isReadOnlyTool(name) || (name == "screenshot" && !args.contains("tab"));
    if (modal != nullptr && !read_only) {
      reply(userDialogOpen(*modal));
      return;
    }
    if (mcp::agentTools().find(name) != nullptr) {
      std::optional<agent_tools::ToolContext> context = contextFor(caller);
      if (!context) {
        reply(mcp::textResult(mcp::kNoServicesError, true));
        return;
      }
      runAgentTool(name, caller, mcp::agentTools().execute(name, args, *context), std::move(reply));
      return;
    }
    const auto tool = isPublic(caller) ? appTools().find(name) : appTools().end();
    if (tool == appTools().end()) {
      reply(mcp::textResult("unknown tool " + name, true));
      return;
    }
    (this->*tool->second)(args, caller, reply);
  } catch (const std::exception& e) {
    // A wrongly typed argument throws out of nlohmann; escaping a Qt slot would abort the app.
    reply(mcp::textResult("bad arguments for " + name + ": " + e.what(), true));
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

void McpServer::runAgentTool(
    const std::string& tool, const Caller& caller, agent_tools::ToolResult result, Reply reply) {
  if (!authorized(caller)) {
    return;  // revoked while it ran: a follow-up would point into services that may die next
  }
  if (result.resume) {
    deferred_.push_back(
        {.tool = tool,
         .caller = caller,
         .resume = std::move(result.resume),
         .reply = std::move(reply),
         .round = resume_round_});
    resume_tick_.start();
    return;
  }
  reply(mcp::textResult(result.content, !result.ok));
}

void McpServer::resumeDeferred() {
  if (draining_) {
    return;  // a reply or continuation re-entered: the outer loop and the next tick get its items
  }
  const QScopedValueRollback<bool> draining(draining_, true);
  const quint64 round = ++resume_round_;
  for (;;) {
    // One at a time, the rest left in deferred_ for a revocation this one causes to drop.
    const auto due =
        std::find_if(deferred_.begin(), deferred_.end(), [round](const Deferred& item) { return item.round != round; });
    if (due == deferred_.end()) {
      break;
    }
    Deferred item = std::move(*due);
    deferred_.erase(due);
    if (item.caller.socket == nullptr || !authorized(item.caller)) {
      continue;  // nobody would read the result
    }
    if (QApplication::activeModalWidget() != nullptr && !mcp::isReadOnlyTool(item.tool)) {
      item.round = round;
      deferred_.push_back(std::move(item));  // the user's dialog first; this waits, it does not fail
      continue;
    }
    std::optional<agent_tools::ToolContext> context = contextFor(item.caller);
    if (!context) {
      item.reply(mcp::textResult(mcp::kNoServicesError, true));
      continue;
    }
    agent_tools::ToolResult next;
    try {
      // The continuation dies right after it ran, while the services it may release into live.
      next = std::exchange(item.resume, nullptr)(*context);
    } catch (const std::exception& e) {
      item.reply(mcp::textResult(std::string("tool failed: ") + e.what(), true));
      continue;
    }
    runAgentTool(item.tool, item.caller, std::move(next), std::move(item.reply));
  }
  if (deferred_.empty()) {
    resume_tick_.stop();
  }
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

}  // namespace PJ
