#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// SPIKE: throwaway MCP-over-HTTP server for experiments; not the production design.

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <vector>

#include "pj_base/expected.hpp"
#include "tool_context.hpp"  // spike/assistant_tools

class QTcpSocket;

namespace PJ {

class MainWindow;
struct ToolboxPanelSession;

/// Loopback MCP server (Streamable HTTP, JSON responses only) answering both the
/// 2026-07-28 stateless protocol and the 2025-06-18 initialize/session one. Runs on
/// the GUI thread; a tool may reply later through its Reply callback.
class McpSpikeServer : public QObject {
 public:
  /// Sends a tools/call result (MCP `CallToolResult` object). Safe after the client left.
  using Reply = std::function<void(nlohmann::json result)>;

  /// Listens on 127.0.0.1:`port` (0 = any free port) and, when listening, writes the
  /// owner-only discovery file the stdio bridge reads; the destructor removes it.
  McpSpikeServer(MainWindow& window, quint16 port, QString token);
  ~McpSpikeServer() override;
  [[nodiscard]] bool listening() const {
    return server_.isListening();
  }
  [[nodiscard]] quint16 port() const {
    return server_.serverPort();
  }

 private:
  struct Request {
    QByteArray method;
    QByteArray path;
    QHash<QByteArray, QByteArray> headers;  // lower-case keys
    QByteArray body;
  };
  void onReadyRead(QTcpSocket* socket);
  void handle(QTcpSocket* socket, const Request& request);
  /// `owner` is the tab namespace of the calling client (PluginPlotTabsController plugin id).
  void callTool(const std::string& name, const nlohmann::json& args, const QString& owner, Reply reply);
  /// PNG of the main window with every visible QRhiWidget's framebuffer composited in.
  [[nodiscard]] QByteArray screenshotPng(int max_width) const;
  /// Dataset by numeric id, display name or source path; 0 when none matches.
  [[nodiscard]] quint32 resolveDataset(const nlohmann::json& selector) const;
  /// One scalar field, every sample, times in display seconds (as `seek` takes them).
  struct Series {
    quint32 dataset = 0;
    std::vector<double> times;
    std::vector<double> values;
  };
  /// `selector` = {topic, field, dataset?}, read through the plugin catalog snapshot and
  /// `readSeries` path; an error names the ambiguity or the missing field.
  [[nodiscard]] Expected<Series> readSeries(const nlohmann::json& selector) const;
  /// args["dataset"] as the dataset display name `resolveCurveKey` filters by; empty when absent.
  [[nodiscard]] QString datasetName(const nlohmann::json& args) const;
  /// The Assistant Agent's tool context for `owner`: the services a toolbox plugin with that id
  /// gets (data processors, object reads, playback, its own plot/scene tabs), built on first use.
  /// nullopt when the host refused to build them.
  [[nodiscard]] std::optional<assistant_agent::ToolContext> agentContext(const QString& owner);
  /// Runs one Assistant Agent tool; a deferred result (an object evaluation) is polled every
  /// 50 ms with a fresh context, as the plugin's GUI tick does, until it is terminal.
  void runAssistantTool(const QString& owner, assistant_agent::ToolResult result, Reply reply);

  MainWindow& window_;
  QTcpServer server_;
  QString token_;
  QString discovery_path_;
  QHash<QTcpSocket*, QByteArray> buffers_;
  /// One toolbox service assembly per agent owner. Released on aboutToQuit, while the
  /// MainWindow services they point into are still alive (this object dies after them).
  QHash<QString, std::shared_ptr<ToolboxPanelSession>> agent_sessions_;
};

}  // namespace PJ
