// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// SPIKE: throwaway MCP-over-HTTP server for experiments; not the production design.

#include "McpSpikeServer.h"

#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPainter>
#include <QRhiWidget>
#include <QSaveFile>
#include <QSettings>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>

#include "FileLoader.h"
#include "LoadInput.h"
#include "MainWindow.h"
#include "PluginPlotTabsController.h"
#include "StreamingSourceManager.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_datastore/data_processor.hpp"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/FeatureCatalogHost.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/PluginRuntimeCatalog.h"
#include "pj_runtime/SavedSeriesPath.h"
#include "pj_runtime/SessionManager.h"
#include "spike/McpProtocol.h"
#include "tool_registry.hpp"  // spike/assistant_tools

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)
using namespace Qt::StringLiterals;

namespace PJ {
// MainWindow.cpp: the service registry of a toolbox panel session (SPIKE).
PJ_service_registry_t spikeToolboxServices(ToolboxPanelSession& session);

namespace {

constexpr auto kOwner = "mcp-agent";
constexpr int kMaxBody = 1 << 20;

QString qs(const json& j, const char* key, const QString& fallback = {}) {
  return j.contains(key) && j[key].is_string() ? QString::fromStdString(j[key].get<std::string>()) : fallback;
}

json text(const std::string& s, bool error = false) {
  json r = {{"content", json::array({{{"type", "text"}, {"text", s}}})}};
  if (error) {
    r["isError"] = true;
  }
  return r;
}

json ok(const json& payload) {
  return text(payload.dump());
}

json fromStatus(const Status& status, const json& on_ok) {
  return status.has_value() ? ok(on_ok) : text(status.error(), true);
}

std::string str(PJ_string_view_t view) {
  return {view.data, view.size};
}

const char* typeName(PrimitiveType type) {
  static constexpr std::array<const char*, 12> names = {"float32", "float64", "int8",   "int16",  "int32", "int64",
                                                        "uint8",   "uint16",  "uint32", "uint64", "bool",  "string"};
  const auto index = static_cast<std::size_t>(type);
  return index < names.size() ? names.at(index) : "unspecified";
}

/// The value column as doubles; empty for a column type the spike does not read (bool, string).
std::vector<double> valuesAsDouble(const sdk::MaterializedSeriesView& series) {
  std::vector<double> out(series.rowCount());
  const auto copy = [&out](const auto* values) {
    if (values == nullptr) {
      return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
      out[i] = static_cast<double>(values[i]);
    }
    return true;
  };
  if (copy(series.valuesAsFloat64()) || copy(series.valuesAsFloat32()) || copy(series.valuesAsInt64()) ||
      copy(series.valuesAsInt32()) || copy(series.valuesAsInt16()) || copy(series.valuesAsInt8()) ||
      copy(series.valuesAsUint64()) || copy(series.valuesAsUint32()) || copy(series.valuesAsUint16()) ||
      copy(series.valuesAsUint8())) {
    return out;
  }
  return {};
}

void writeResponse(QTcpSocket* socket, int code, const QByteArray& body, const QList<QByteArray>& extra = {}) {
  static const std::map<int, QByteArray> reasons = {
      {200, "OK"},        {202, "Accepted"},           {400, "Bad Request"},      {401, "Unauthorized"},
      {403, "Forbidden"}, {405, "Method Not Allowed"}, {413, "Payload Too Large"}};
  QByteArray head = "HTTP/1.1 " + QByteArray::number(code) + ' ' + reasons.at(code) + "\r\n";
  if (!body.isEmpty()) {
    head += "Content-Type: application/json\r\n";
  }
  for (const QByteArray& h : extra) {
    head += h + "\r\n";
  }
  head += "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
  socket->write(head + body);
  socket->disconnectFromHost();
}

}  // namespace

McpSpikeServer::McpSpikeServer(MainWindow& window, quint16 port, QString token)
    : QObject(&window), window_(window), token_(std::move(token)) {
  connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() { agent_sessions_.clear(); });
  connect(&server_, &QTcpServer::newConnection, this, [this]() {
    while (QTcpSocket* socket = server_.nextPendingConnection()) {
      connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
      connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        buffers_.remove(socket);
        socket->deleteLater();
      });
    }
  });
  if (!server_.listen(QHostAddress::LocalHost, port)) {
    std::fprintf(stderr, "[mcp] listen failed: %s\n", qPrintable(server_.errorString()));
    return;
  }
  std::fprintf(
      stderr, "[mcp] listening on http://127.0.0.1:%u/mcp token=%s\n", server_.serverPort(), qPrintable(token_));
  // Discovery file for the stdio bridge: owner-only, removed on exit; a crash leaves it
  // behind, and the bridge drops it when the port no longer answers with this token.
  discovery_path_ = mcp::instancesDir() + u"/%1.json"_s.arg(QCoreApplication::applicationPid());
  QSaveFile file(discovery_path_);
  if (file.open(QIODevice::WriteOnly)) {
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    file.write(
        QByteArray::fromStdString(json({{"pid", QCoreApplication::applicationPid()},
                                        {"port", server_.serverPort()},
                                        {"token", token_.toStdString()},
                                        {"started_ms", QDateTime::currentMSecsSinceEpoch()},
                                        {"exe", QCoreApplication::applicationFilePath().toStdString()}})
                                      .dump()));
    if (!file.commit()) {
      discovery_path_.clear();
    }
  }
}

McpSpikeServer::~McpSpikeServer() {
  if (!discovery_path_.isEmpty()) {
    QFile::remove(discovery_path_);
  }
}

void McpSpikeServer::onReadyRead(QTcpSocket* socket) {
  QByteArray& buf = buffers_[socket];
  buf += socket->readAll();
  const qsizetype header_end = buf.indexOf("\r\n\r\n");
  if (header_end < 0) {
    if (buf.size() > 16384) {
      writeResponse(socket, 413, {});
    }
    return;
  }
  Request req;
  const QList<QByteArray> lines = buf.left(header_end).split('\n');
  const QList<QByteArray> request_line = lines.value(0).trimmed().split(' ');
  req.method = request_line.value(0);
  req.path = request_line.value(1);
  for (qsizetype i = 1; i < lines.size(); ++i) {
    const qsizetype colon = lines[i].indexOf(':');
    if (colon > 0) {
      req.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
    }
  }
  const int length = req.headers.value("content-length", "0").toInt();
  if (length < 0 || length > kMaxBody) {
    writeResponse(socket, 413, {});
    return;
  }
  if (buf.size() - header_end - 4 < length) {
    return;  // body still arriving
  }
  req.body = buf.mid(header_end + 4, length);
  buffers_.remove(socket);
  handle(socket, req);
}

void McpSpikeServer::handle(QTcpSocket* socket, const Request& req) {
  // Agents send no Origin; a browser page always does (DNS-rebinding and cross-site requests).
  if (req.headers.contains("origin")) {
    writeResponse(socket, 403, {});
    return;
  }
  if (req.headers.value("authorization") != "Bearer " + token_.toUtf8()) {
    writeResponse(socket, 401, {});
    return;
  }
  if (req.method == "GET" && req.path.startsWith("/screenshot.png")) {
    // Plain HTTP for humans/scripts: GET /screenshot.png[?tab=<agent tab>&max_width=N]
    const QUrlQuery query(QUrl::fromEncoded("http://x" + req.path));
    if (query.hasQueryItem(u"tab"_s)) {
      const QString owner =
          query.hasQueryItem(u"owner"_s) ? query.queryItemValue(u"owner"_s) : QString::fromLatin1(kOwner);
      (void)window_.plugin_plot_tabs_->focusTab(owner, query.queryItemValue(u"tab"_s));
    }
    const int max_width = query.hasQueryItem(u"max_width"_s) ? query.queryItemValue(u"max_width"_s).toInt() : 1600;
    const QByteArray png = screenshotPng(max_width);
    socket->write(
        "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: " + QByteArray::number(png.size()) +
        "\r\nConnection: close\r\n\r\n" + png);
    socket->disconnectFromHost();
    return;
  }
  if (req.method != "POST") {
    writeResponse(socket, 405, {});
    return;
  }
  const json msg = json::parse(req.body.toStdString(), nullptr, false);
  if (msg.is_discarded() || !msg.is_object()) {
    writeResponse(socket, 400, {});
    return;
  }
  if (!msg.contains("id")) {
    writeResponse(socket, 202, {});  // notification
    return;
  }
  const json id = msg["id"];
  const std::string method = msg.value("method", "");
  const json params = msg.value("params", json::object());
  std::fprintf(stderr, "[mcp] %s %s\n", method.c_str(), method == "tools/call" ? params.value("name", "").c_str() : "");
  QPointer<QTcpSocket> guard(socket);
  const auto send = [guard, id](const json& result, QList<QByteArray> extra = {}) {
    if (guard) {
      writeResponse(
          guard, 200, QByteArray::fromStdString(json({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}).dump()),
          extra);
    }
  };
  if (auto lifecycle = mcp::lifecycleResult(method, params); lifecycle.has_value()) {
    QList<QByteArray> extra;
    if (method == "initialize") {
      extra.push_back("Mcp-Session-Id: " + QUuid::createUuid().toByteArray(QUuid::WithoutBraces));
    }
    send(*lifecycle, extra);
  } else if (method == "tools/list") {
    send({{"tools", mcp::toolList()}});
  } else if (method == "tools/call") {
    auto timer = std::make_shared<QElapsedTimer>();
    timer->start();
    const std::string name = params.value("name", "");
    // The bridge names the client; a direct 2026 client names itself in _meta; else one shared namespace.
    const QString owner = req.headers.contains("x-pj-owner")
                              ? QString::fromUtf8(req.headers.value("x-pj-owner"))
                              : QString::fromStdString(mcp::ownerFor(mcp::clientName(params)));
    // A wrongly typed argument throws out of nlohmann; escaping a Qt slot would abort the app.
    try {
      callTool(name, params.value("arguments", json::object()), owner, [send, timer, name](json result) {
        std::fprintf(stderr, "[mcp]   %s done in %lld ms\n", name.c_str(), static_cast<long long>(timer->elapsed()));
        send(result);
      });
    } catch (const std::exception& e) {
      send(text(std::string("bad arguments for ") + name + ": " + e.what(), true));
    }
  } else if (guard) {
    writeResponse(
        guard, 200,
        QByteArray::fromStdString(json({{"jsonrpc", "2.0"},
                                        {"id", id},
                                        {"error", {{"code", -32601}, {"message", "method not found: " + method}}}})
                                      .dump()));
  }
}

void McpSpikeServer::callTool(const std::string& name, const json& args, const QString& owner, Reply reply) {
  AppSession& session = *window_.session_;
  CatalogModel& catalog = session.catalogModel();
  PlaybackEngine& playback = session.playbackEngine();
  PluginPlotTabsController& tabs = *window_.plugin_plot_tabs_;

  // A modal dialog belongs to the user: acting under it would change state they are editing.
  const QWidget* modal = QApplication::activeModalWidget();
  if (modal != nullptr && !mcp::isReadOnlyTool(name)) {
    reply(text(
        "the user has the dialog '" + modal->windowTitle().toStdString() +
            "' open in PlotJuggler; wait for them to close it (poll get_status) before changing anything",
        true));
    return;
  }

  if (mcp::assistantTools().find(name) != nullptr) {
    std::optional<assistant_agent::ToolContext> context = agentContext(owner);
    if (!context) {
      reply(text("PlotJuggler could not build the services this tool needs", true));
      return;
    }
    runAssistantTool(owner, mcp::assistantTools().execute(name, args, *context), std::move(reply));
    return;
  }

  if (name == "get_status") {
    json datasets = json::array();
    for (const auto& [id, dataset_name] : catalog.datasets()) {
      const QString path = session.sessionManager().datasetSourcePath(id);
      json entry = {{"id", id}, {"name", dataset_name.toStdString()}, {"path", path.toStdString()}};
      if (const auto range = session.sessionManager().datasetDisplayRange(id)) {
        entry["time_range"] = {range->min.value, range->max.value};  // datasets may not share one clock
      }
      for (const auto& src : session.sessionManager().loadedSources()) {
        if (!path.isEmpty() && FileLoader::sameSourceIdentity(src.path, path)) {
          entry["loader"] = src.plugin_id.toStdString();
          entry["config"] = json::parse(src.plugin_config_json.toStdString(), nullptr, false);
        }
      }
      if (const QString stream_id = window_.streaming_manager_->layoutSourceId(id); !stream_id.isEmpty()) {
        for (const auto& live : window_.streaming_manager_->layoutSources()) {
          if (live.id == stream_id) {
            entry["stream"] = live.plugin_id.toStdString();
            entry["config"] = live.plugin_config_json.isEmpty()
                                  ? json()
                                  : json::parse(live.plugin_config_json.toStdString(), nullptr, false);
          }
        }
      }
      datasets.push_back(std::move(entry));
    }
    json stream_sources = json::array();
    for (const LoadedDataSource* source : session.extensionCatalog().streamSources()) {
      stream_sources.push_back(source->name);
    }
    json tab_ids = json::array();
    if (auto ids = tabs.listTabIds(owner); ids.has_value()) {
      tab_ids = *ids;
    }
    reply(
        ok({{"datasets", datasets},
            {"stream_sources", stream_sources},
            {"time_range", {playback.rangeMin().value, playback.rangeMax().value}},
            {"current_time", playback.currentTime().value},
            {"playing", playback.isPlaying()},
            {"agent_tabs", tab_ids},
            {"focused", QApplication::activeWindow() != nullptr},
            {"user_dialog", modal != nullptr ? json(modal->windowTitle().toStdString()) : json()}}));
  } else if (name == "list_catalog") {
    // The same snapshot a plugin gets (pj.toolbox_host acquire_catalog_snapshot_v2).
    SessionManager& manager = session.sessionManager();
    DatastoreToolboxHost host(manager.dataEngine(), manager.objectStore());
    auto snapshot = sdk::ToolboxHostView(host.raw()).catalogSnapshotV2();
    if (!snapshot.has_value()) {
      reply(text(snapshot.error(), true));
      return;
    }
    const std::string prefix = qs(args, "prefix").toStdString();
    const quint32 only = args.contains("dataset") ? resolveDataset(args["dataset"]) : 0;
    const std::size_t max_fields = args.value("max_fields", std::size_t{40});
    std::map<quint32, json> datasets;
    for (const auto& source : snapshot->dataSources()) {
      if (only == 0 || source.handle.id == only) {
        datasets[source.handle.id] = {{"id", source.handle.id}, {"name", str(source.name)}};
      }
    }
    const auto fields = snapshot->fields();
    for (const auto& topic : snapshot->topics()) {
      const std::string topic_name = str(topic.name);
      if (!datasets.contains(topic.source.id) || !topic_name.starts_with(prefix)) {
        continue;
      }
      json& entry = datasets[topic.source.id]["topics"][topic_name];
      entry["fields"] = json::object();
      for (uint32_t i = 0; i < topic.field_count; ++i) {
        if (i == max_fields) {
          entry["more_fields"] = topic.field_count - max_fields;
          break;
        }
        const auto& field = fields[topic.first_field + i];
        entry["fields"][str(field.name)] = typeName(static_cast<PrimitiveType>(field.type));
      }
    }
    for (const auto& object : snapshot->objectTopics()) {
      const std::string object_name = str(object.name);
      if (!datasets.contains(object.source.id) || !object_name.starts_with(prefix)) {
        continue;
      }
      json entry = {{"type", str(object.builtin_object_type)}, {"count", object.entry_count}};
      const auto t0 = manager.displayTimeForSource(object.source.id, object.time_min_ns);
      const auto t1 = manager.displayTimeForSource(object.source.id, object.time_max_ns);
      if (object.entry_count > 0 && t0 && t1) {
        entry["time_range"] = {t0->value, t1->value};
      }
      datasets[object.source.id]["objects"][object_name] = std::move(entry);
    }
    json out = json::array();
    for (auto& [id, dataset] : datasets) {
      out.push_back(std::move(dataset));
    }
    reply(ok(out));
  } else if (name == "about") {
    if (!window_.feature_catalog_) {
      reply(text("no feature catalog in this build", true));
      return;
    }
    reply(ok(json::parse(window_.feature_catalog_->document(), nullptr, false)));
  } else if (name == "get_values" || name == "statistics") {
    auto series = readSeries(args);
    if (!series.has_value()) {
      reply(text(series.error(), true));
      return;
    }
    const double t0 = args.value("t0", -std::numeric_limits<double>::infinity());
    const double t1 = args.value("t1", std::numeric_limits<double>::infinity());
    std::vector<double> times;
    std::vector<double> values;
    for (std::size_t i = 0; i < series->times.size(); ++i) {
      if (series->times[i] >= t0 && series->times[i] <= t1) {
        times.push_back(series->times[i]);
        values.push_back(series->values[i]);
      }
    }
    json result = {{"dataset", series->dataset}, {"count", times.size()}};
    if (times.empty()) {
      reply(ok(result));
      return;
    }
    if (args.contains("at")) {
      // The sample in force at `at` (last at or before it), as the plot's tracker shows it.
      const auto it = std::upper_bound(times.begin(), times.end(), args.value("at", 0.0));
      const auto k = std::distance(times.begin(), it) - 1;
      result["sample"] = k < 0 ? json() : json{times[static_cast<std::size_t>(k)], values[static_cast<std::size_t>(k)]};
      reply(ok(result));
      return;
    }
    if (name == "statistics") {
      double sum = 0;
      double sum_sq = 0;
      const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
      for (const double v : values) {
        sum += v;
        sum_sq += v * v;
      }
      const double n = static_cast<double>(values.size());
      const double mean = sum / n;
      result.update(
          {{"min", *lo},
           {"max", *hi},
           {"mean", mean},
           {"std", std::sqrt(std::max(0.0, sum_sq / n - mean * mean))},
           {"first", {times.front(), values.front()}},
           {"last", {times.back(), values.back()}}});
    } else {
      // ponytail: plain stride decimation; min/max-preserving buckets if agents miss spikes.
      const std::size_t max_points = std::max<std::size_t>(2, args.value("max_points", std::size_t{200}));
      const std::size_t stride = (times.size() + max_points - 1) / max_points;
      json samples = json::array();
      for (std::size_t k = 0; k < times.size(); k += stride) {
        samples.push_back({times[k], values[k]});
      }
      result["stride"] = stride;
      result["samples"] = std::move(samples);
    }
    reply(ok(result));
  } else if (name == "export_series") {
    // One time-aligned CSV for the agent's own analysis (numpy, pandas...): PlotJuggler has
    // already decoded the file, whatever its format.
    const json wanted = args.value("series", json::array());
    if (!wanted.is_array() || wanted.empty()) {
      reply(text("series: a list of {topic, field, dataset?}", true));
      return;
    }
    std::vector<Series> columns;
    for (const json& selector : wanted) {
      auto series = readSeries(selector);
      if (!series.has_value()) {
        reply(text(selector.dump() + ": " + series.error(), true));
        return;
      }
      columns.push_back(std::move(*series));
    }
    const double t0 = args.value("t0", -std::numeric_limits<double>::infinity());
    const double t1 = args.value("t1", std::numeric_limits<double>::infinity());
    std::vector<double> grid;  // a uniform grid at rate_hz, else every timestamp of every series
    const double rate = args.value("rate_hz", 0.0);
    if (rate > 0) {
      double lo = std::numeric_limits<double>::infinity();
      double hi = -lo;
      for (const Series& c : columns) {
        if (!c.times.empty()) {
          lo = std::min(lo, c.times.front());
          hi = std::max(hi, c.times.back());
        }
      }
      lo = std::max(lo, t0);
      hi = std::min(hi, t1);
      for (std::size_t i = 0; lo + static_cast<double>(i) / rate <= hi && grid.size() < 10'000'000; ++i) {
        grid.push_back(lo + static_cast<double>(i) / rate);
      }
    } else {
      for (const Series& c : columns) {
        std::copy_if(c.times.begin(), c.times.end(), std::back_inserter(grid), [t0, t1](double t) {
          return t >= t0 && t <= t1;
        });
      }
      std::sort(grid.begin(), grid.end());
      grid.erase(std::unique(grid.begin(), grid.end()), grid.end());
    }
    QString path = qs(args, "path");
    if (path.isEmpty()) {
      path = QDir::temp().filePath(u"plotjuggler_export_%1.csv"_s.arg(QDateTime::currentMSecsSinceEpoch()));
    }
    if (!QFileInfo(path).isAbsolute()) {
      reply(text("path must be absolute", true));
      return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      reply(text("cannot write " + path.toStdString() + ": " + file.errorString().toStdString(), true));
      return;
    }
    // Sample-and-hold: each row holds every series' last value at or before its time (empty before the first).
    std::string out = "time";
    for (const json& selector : wanted) {
      out += "," + selector.value("topic", "") + "/" + selector.value("field", "");
    }
    out += '\n';
    std::vector<std::size_t> cursor(columns.size(), 0);
    std::array<char, 32> number{};
    for (const double t : grid) {
      std::snprintf(number.data(), number.size(), "%.9f", t);
      out += number.data();
      for (std::size_t c = 0; c < columns.size(); ++c) {
        const Series& column = columns[c];
        while (cursor[c] < column.times.size() && column.times[cursor[c]] <= t) {
          ++cursor[c];
        }
        out += ',';
        if (cursor[c] > 0) {
          std::snprintf(number.data(), number.size(), "%.10g", column.values[cursor[c] - 1]);
          out += number.data();
        }
      }
      out += '\n';
      if (out.size() > (1U << 22)) {
        file.write(out.data(), static_cast<qint64>(out.size()));
        out.clear();
      }
    }
    file.write(out.data(), static_cast<qint64>(out.size()));
    file.close();
    json names = json::array();
    for (const json& selector : wanted) {
      names.push_back(selector.value("topic", "") + "/" + selector.value("field", ""));
    }
    reply(
        ok({{"path", path.toStdString()},
            {"rows", grid.size()},
            {"columns", names},
            {"note", "time in display seconds (as seek); values sample-and-hold, empty before a series starts"}}));
  } else if (name == "start_stream") {
    StreamingSourceManager& streams = *window_.streaming_manager_;
    const QString source = qs(args, "source");
    if (args.value("ask_user", false)) {
      // The user's own Start: select the source in the Stream combo (the manager follows it) and press
      // Start once this request has returned, since the plugin's dialog is modal.
      auto* combo = window_.findChild<QComboBox*>(u"comboStreaming"_s);
      if (combo == nullptr || combo->findText(source) < 0) {
        reply(text("unknown stream source (get_status lists stream_sources)", true));
        return;
      }
      combo->setCurrentText(source);
      QTimer::singleShot(0, this, [&streams, reply] {
        DatasetId started = 0;
        QString failure;
        QObject guard;
        connect(&streams, &StreamingSourceManager::streamStarted, &guard, [&started](DatasetId id) { started = id; });
        connect(&streams, &StreamingSourceManager::setupError, &guard, [&failure](const QString& m) { failure = m; });
        connect(&streams, &StreamingSourceManager::streamError, &guard, [&failure](DatasetId, const QString& m) {
          failure = m;
        });
        streams.onStartRequested();
        if (started != 0) {
          reply(ok({{"dataset_id", started}, {"note", "topics appear as the source advertises them: list_topics"}}));
        } else {
          reply(text(failure.isEmpty() ? "the user cancelled the stream dialog" : failure.toStdString(), true));
        }
      });
      return;
    }
    // No dialog: the agent's config, else what the user last confirmed in this source's dialog.
    const auto given = args.find("config");
    const QString config = given != args.end() && given->is_object()
                               ? QString::fromStdString(given->dump())
                               : QSettings().value(u"PluginConfig/"_s + source).toString();
    layout_xml::StreamingSourceRef ref;
    ref.id = u"stream"_s;
    ref.plugin_id = source;
    ref.plugin_config_json = config;
    QString error;
    const DatasetId id = streams.startFromLayout(ref, config, error);
    if (id == 0) {
      reply(text(error.toStdString(), true));
      return;
    }
    reply(
        ok({{"dataset_id", id},
            {"config", config.isEmpty() ? json() : json::parse(config.toStdString(), nullptr, false)},
            {"note", "topics appear as the source advertises them: list_topics"}}));
  } else if (name == "stop_stream") {
    const DatasetId id = resolveDataset(args.value("dataset", json()));
    if (id == 0 || !window_.streaming_manager_->stopDatasetAndWait(id, u"stopped by the agent"_s)) {
      reply(text("no live stream matches (get_status marks live ones with stream)", true));
      return;
    }
    reply(ok({{"stopped", id}, {"note", "its data stays loaded; unload_file removes it"}}));
  } else if (name == "open_file") {
    const QFileInfo info(qs(args, "path"));
    if (!info.isAbsolute() || !info.isFile()) {
      reply(text("path must be an absolute path to an existing file", true));
      return;
    }
    FileLoader& loader = *window_.file_loader_;
    auto* receiver = new QObject(this);
    auto ticket = std::make_shared<LoadRequestId>(0);
    connect(
        &loader, &FileLoader::loadFinished, receiver,
        [this, receiver, ticket, reply](
            quint64 t, LoadOutcome outcome, const QString& reason, DatasetId id, const QVector<DatasetId>&) {
          if (t != *ticket) {
            return;
          }
          receiver->deleteLater();
          if (outcome == LoadOutcome::kCancelled) {
            reply(text("the load was cancelled (the user closed the dialog or stopped the import)", true));
            return;
          }
          if (outcome != LoadOutcome::kLoaded) {
            reply(text("load failed: " + reason.toStdString(), true));
            return;
          }
          // What was actually loaded (the user's dialog choices included), so the agent need not guess.
          json result = {{"loaded", true}, {"dataset_id", id}};
          const SessionManager& manager = window_.session_->sessionManager();
          const QString path = manager.datasetSourcePath(id);
          for (const auto& src : manager.loadedSources()) {
            if (!path.isEmpty() && FileLoader::sameSourceIdentity(src.path, path)) {
              result["config"] = json::parse(src.plugin_config_json.toStdString(), nullptr, false);
            }
          }
          reply(ok(result));
        });
    LoadHints hints;
    if (args.value("ask_user", false)) {
      hints.dialog_policy = DialogPolicy::kInteractive;
    } else {
      hints.dialog_policy = DialogPolicy::kNever;
      if (const auto config = args.find("config"); config != args.end() && config->is_object()) {
        hints.preset_config_json = QString::fromStdString(config->dump());
      }
    }
    *ticket = loader.loadFileTicketed(LoadInput::fromNativePath(info.absoluteFilePath()), nullptr, hints);
    if (*ticket == 0) {
      receiver->deleteLater();
      reply(text("load rejected (no plugin for this file type?)", true));
    }
  } else if (name == "add_script_series") {
    // A plugin-style transform owned by the agent: listed under Custom Topics, saved in
    // layouts with its script, and outside undo/redo like the agent's tabs.
    const std::string series = args.value("name", "");
    if (series.empty()) {
      reply(text("name is required", true));
      return;
    }
    // Inputs: `inputs` (N, joined on exact timestamps) or the single topic/field/dataset.
    const json inputs = args.contains("inputs") ? args["inputs"]
                                                : json::array(
                                                      {{{"topic", args.value("topic", "")},
                                                        {"field", args.value("field", "")},
                                                        {"dataset", args.value("dataset", json())}}});
    std::vector<std::string> input_names;  // "[dataset_source:]topic/field", resolved by the service
    for (const json& in : inputs) {
      json selector = in;
      if (selector.contains("dataset") && selector["dataset"].is_null()) {
        selector.erase("dataset");
      }
      const QString source = datasetName(selector);
      const QString topic = qs(selector, "topic");
      const QString field = qs(selector, "field");
      if (!catalog.resolveCurveKey(
              SavedSeriesPath{
                  .topic = topic, .field = field, .dataset_id = 0, .dataset_source = source, .dataset_path = {}})) {
        reply(text(
            "no series " + topic.toStdString() + " / " + field.toStdString() +
                " in one dataset (see list_catalog; pass dataset when several hold it)",
            true));
        return;
      }
      input_names.push_back(
          (source.isEmpty() ? std::string() : source.toStdString() + ":") + topic.toStdString() + "/" +
          field.toStdString());
    }
    // Several outputs live under the series name, so two scripts' `x` never collide.
    std::vector<std::string> outputs{series};
    if (args.contains("outputs")) {
      outputs.clear();
      for (const std::string& output : args["outputs"].get<std::vector<std::string>>()) {
        outputs.push_back(series + "/" + output);
      }
    }
    // The transform id must equal the script's class id: derive it from the name and stamp it
    // onto whatever class the agent's script defines (Luau: returns; Python: class T).
    std::string transform_id;
    for (const char c : series) {
      transform_id += (std::isalnum(static_cast<unsigned char>(c)) != 0) ? c : '_';
    }
    std::string script = args.value("script", "");
    if (script.starts_with("# pj-script: python")) {
      script += "\nT.id = \"" + transform_id + "\"\n";
    } else {
      if (script.starts_with("-- pj-script:")) {
        script = script.substr(script.find('\n') + 1);
      }
      script = "-- pj-script: luau\nlocal agent_class = (function()\n" + script + "\nend)()\nagent_class.id = \"" +
               transform_id + "\"\nreturn agent_class\n";
    }
    DataProcessorService& service = session.sessionManager().dataProcessorService();
    auto created = service.upsertTransform(
        owner.toStdString(), transform_id, input_names, outputs, script, args.value("params", json::object()).dump(),
        /*ephemeral=*/false, /*input_column_index=*/0, /*history_exempt=*/true);
    if (!created.has_value()) {
      reply(text(created.error(), true));
      return;
    }
    catalog.rebuildFromDatastore();
    window_.syncCustomTopicsPanel();
    json topics = json::object();  // each output topic with its plottable fields
    for (const std::string& output : created->outputs) {
      topics[output] = json::array();
    }
    for (const CatalogItem& item : catalog.items()) {
      const auto* scalar = std::get_if<ScalarFieldPayload>(&item.payload);
      if (scalar != nullptr && topics.contains(item.topic_name.toStdString())) {
        topics[item.topic_name.toStdString()].push_back(scalar->field_path.toStdString());
      }
    }
    reply(ok({{"topics", topics}}));
  } else if (name == "remove_script_series") {
    DataProcessorService& service = session.sessionManager().dataProcessorService();
    const Status removed = service.removeTransform(DataProcessorService::makeTransformKey(owner.toStdString(), [&args] {
      std::string id = args.value("name", "");
      std::replace_if(
          id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) == 0; }, '_');
      return id;
    }()));
    if (removed.has_value()) {
      catalog.rebuildFromDatastore();
      window_.syncCustomTopicsPanel();
    }
    reply(fromStatus(removed, "ok"));
  } else if (name == "add_filter") {
    // A built-in filter becomes an agent transform too (owned, removable, outside undo, under
    // Custom Topics): select its class from the bundled resource, filling schema defaults.
    QFile resource(u":/filters/builtin_filters.luau"_s);
    if (!resource.open(QIODevice::ReadOnly)) {
      reply(text("the built-in filters are not in this build", true));
      return;
    }
    const std::string filter = args.value("filter", "");
    if (filter.empty() ||
        !std::all_of(filter.begin(), filter.end(), [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; })) {
      reply(text("unknown built-in filter '" + filter + "'", true));
      return;
    }
    const std::string selector = "local classes = (function()\n" + resource.readAll().toStdString() +
                                 "\nend)()\n"
                                 "for _, c in ipairs(classes) do\n"
                                 "  if c.id == \"" +
                                 filter +
                                 "\" then\n"
                                 "    local create = c.create\n"
                                 "    c.create = function(given)\n"
                                 "      local p = {}\n"
                                 "      for _, d in ipairs(c.parameters or {}) do p[d.name] = d.default end\n"
                                 "      for k, v in pairs(given or {}) do p[k] = v end\n"
                                 "      return create(p)\n"
                                 "    end\n"
                                 "    return c\n"
                                 "  end\n"
                                 "end\n"
                                 "error(\"unknown built-in filter '" +
                                 filter + "'\")\n";
    json delegated = args;
    delegated["script"] = selector;
    if (!delegated.contains("name")) {
      delegated["name"] = args.value("topic", "") + "/" + args.value("field", "") + "[" + filter + "]";
    }
    callTool("add_script_series", delegated, owner, std::move(reply));
  } else if (name == "screenshot") {
    if (args.contains("tab")) {
      if (const Status focused = tabs.focusTab(owner, qs(args, "tab")); !focused) {
        reply(text(focused.error(), true));
        return;
      }
    }
    reply(
        {{"content", json::array(
                         {{{"type", "image"},
                           {"mimeType", "image/png"},
                           {"data", screenshotPng(args.value("max_width", 1600)).toBase64().toStdString()}}})}});
  } else if (name == "unload_file") {
    const DatasetId id = resolveDataset(args.value("dataset", json()));
    if (id == 0) {
      reply(text("no such dataset (see get_status)", true));
      return;
    }
    std::vector<TopicId> topics = session.sessionManager().dataEngine().listTopics(id);
    auto done = std::make_shared<bool>(false);
    window_.confirmAndRemoveDependentTransforms(topics, [this, id, done, reply]() {
      AppSession& s = *window_.session_;
      window_.removeDatasetData(id);
      if (window_.active_streaming_dataset_id_ == 0) {
        s.playbackEngine().pause();
        s.seedPlaybackFromSession();
      }
      window_.resetUndoHistory();
      *done = true;
      reply(ok({{"unloaded", id}}));
    });
    if (!*done) {
      // ponytail: the continuation's own reply is dropped once this one went out; fine for a spike.
      reply(text("derived series depend on this dataset: the user was asked to confirm in PlotJuggler"));
    }
  } else if (name == "reload_file") {
    const DatasetId id = resolveDataset(args.value("dataset", json()));
    const QString path = id == 0 ? QString() : window_.file_loader_->sourcePathForDataset(id);
    if (path.isEmpty()) {
      reply(text("no file-backed dataset matches (see get_status)", true));
      return;
    }
    LoadHints hints;
    hints.dialog_policy = DialogPolicy::kNever;
    for (const auto& src : session.sessionManager().loadedSources()) {
      if (FileLoader::sameSourceIdentity(src.path, path)) {
        hints.expected_plugin_id = src.plugin_id;
        hints.preset_config_json = src.plugin_config_json;
      }
    }
    FileLoader& loader = *window_.file_loader_;
    auto* receiver = new QObject(this);
    auto ticket = std::make_shared<LoadRequestId>(0);
    connect(
        &loader, &FileLoader::loadFinished, receiver,
        [receiver, ticket, reply](
            quint64 t, LoadOutcome outcome, const QString&, DatasetId new_id, const QVector<DatasetId>&) {
          if (t != *ticket) {
            return;
          }
          receiver->deleteLater();
          reply(
              outcome == LoadOutcome::kLoaded ? ok({{"reloaded", true}, {"dataset_id", new_id}})
                                              : text("reload failed or cancelled", true));
        });
    *ticket = loader.loadFileTicketed(LoadInput::fromNativePath(path), nullptr, hints);
    if (*ticket == 0) {
      receiver->deleteLater();
      reply(text("reload rejected", true));
    }
  } else {
    reply(text("unknown tool " + name, true));
  }
}

QByteArray McpSpikeServer::screenshotPng(int max_width) const {
  QImage image = window_.grab().toImage();
  {
    QPainter painter(&image);
    for (QRhiWidget* view : window_.findChildren<QRhiWidget*>()) {
      if (!view->isVisible() || view->window() != &window_) {
        continue;
      }
      const QImage frame = view->grabFramebuffer();
      if (!frame.isNull()) {
        painter.drawImage(QRect(view->mapTo(&window_, QPoint(0, 0)), view->size()), frame);
      }
    }
    // The agent should see what the user is being asked. Wayland hides real window positions,
    // so the dialog is drawn centred over a dimmed window, as most compositors place it.
    if (QWidget* modal = QApplication::activeModalWidget(); modal != nullptr && modal != &window_) {
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
  return png;
}

Expected<McpSpikeServer::Series> McpSpikeServer::readSeries(const json& selector) const {
  SessionManager& manager = window_.session_->sessionManager();
  DatastoreToolboxHost host(manager.dataEngine(), manager.objectStore());
  const sdk::ToolboxHostView view(host.raw());
  auto snapshot = view.catalogSnapshotV2();
  if (!snapshot.has_value()) {
    return unexpected(snapshot.error());
  }
  const std::string topic_name = selector.value("topic", "");
  const std::string field_name = selector.value("field", "");
  const quint32 only = selector.contains("dataset") ? resolveDataset(selector["dataset"]) : 0;
  std::vector<std::pair<quint32, sdk::FieldHandle>> matches;
  const auto fields = snapshot->fields();
  for (const auto& topic : snapshot->topics()) {
    if (str(topic.name) != topic_name || (only != 0 && topic.source.id != only)) {
      continue;
    }
    for (uint32_t i = 0; i < topic.field_count; ++i) {
      if (str(fields[topic.first_field + i].name) == field_name) {
        matches.emplace_back(topic.source.id, fields[topic.first_field + i].handle);
      }
    }
  }
  if (matches.size() != 1) {
    json ids = json::array();
    for (const auto& match : matches) {
      ids.push_back(match.first);
    }
    return unexpected(
        matches.empty() ? "no scalar field " + topic_name + " / " + field_name + " (see list_catalog)"
                        : "the field exists in several datasets " + ids.dump() + ": pass dataset");
  }
  const auto [dataset, handle] = matches.front();
  auto materialized = view.readSeries(handle);
  if (!materialized.has_value()) {
    return unexpected(materialized.error());
  }
  Series series{.dataset = dataset, .times = {}, .values = valuesAsDouble(*materialized)};
  const auto stamps = materialized->timestamps();
  if (series.values.size() != stamps.size()) {
    return unexpected(std::string("this field's type cannot be read as numbers"));
  }
  series.times.reserve(stamps.size());
  for (const int64_t stamp : stamps) {
    const auto t = manager.displayTimeForSource(dataset, stamp);
    series.times.push_back(t ? t->value : std::numeric_limits<double>::quiet_NaN());
  }
  return series;
}

QString McpSpikeServer::datasetName(const json& args) const {
  if (!args.contains("dataset")) {
    return {};
  }
  const quint32 id = resolveDataset(args["dataset"]);
  for (const auto& [dataset_id, name] : window_.session_->catalogModel().datasets()) {
    if (dataset_id == id) {
      return name;
    }
  }
  // Unknown: a name no dataset has, so the lookup fails instead of picking any dataset.
  return args["dataset"].is_string() ? QString::fromStdString(args["dataset"].get<std::string>()) : u"?"_s;
}

std::optional<assistant_agent::ToolContext> McpSpikeServer::agentContext(const QString& owner) {
  std::shared_ptr<ToolboxPanelSession>& session = agent_sessions_[owner];
  if (session == nullptr) {
    RuntimeToolboxPlugin stand_in;  // no library: nothing is loaded, only the services are built
    stand_in.id = owner.toStdString();
    stand_in.name = "MCP agent";
    session =
        window_.buildToolboxPanelSession(stand_in, owner, u"MCP agent"_s, [](const QString&, const QString& error) {
          std::fprintf(stderr, "[mcp] agent services: %s\n", qPrintable(error));
        });
    if (session == nullptr) {
      agent_sessions_.remove(owner);
      return std::nullopt;
    }
  }
  const sdk::ServiceRegistry services(spikeToolboxServices(*session));
  assistant_agent::ToolContext context;
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

void McpSpikeServer::runAssistantTool(const QString& owner, assistant_agent::ToolResult result, Reply reply) {
  if (!result.resume) {
    reply(text(result.content, !result.ok));
    return;
  }
  QTimer::singleShot(50, this, [this, owner, resume = std::move(result.resume), reply = std::move(reply)]() mutable {
    std::optional<assistant_agent::ToolContext> context = agentContext(owner);
    if (!context) {
      reply(text("PlotJuggler could not build the services this tool needs", true));
      return;
    }
    assistant_agent::ToolResult next;
    try {
      next = resume(*context);
    } catch (const std::exception& e) {
      reply(text(std::string("tool failed: ") + e.what(), true));
      return;
    }
    runAssistantTool(owner, std::move(next), std::move(reply));
  });
}

quint32 McpSpikeServer::resolveDataset(const json& selector) const {
  const CatalogModel& catalog = window_.session_->catalogModel();
  const SessionManager& manager = window_.session_->sessionManager();
  for (const auto& [id, dataset_name] : catalog.datasets()) {
    if (selector.is_number_integer() && selector.get<quint32>() == id) {
      return id;
    }
    if (selector.is_string()) {
      const QString wanted = QString::fromStdString(selector.get<std::string>());
      if (wanted == dataset_name || wanted == manager.datasetSourcePath(id) || wanted == QString::number(id)) {
        return id;
      }
    }
  }
  return 0;
}

}  // namespace PJ
