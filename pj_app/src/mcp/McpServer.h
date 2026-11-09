#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTimer>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "mcp/McpProtocol.h"
#include "pj_base/plugin_data_api.h"
#include "pj_runtime/agent_tools/tool_context.hpp"

class QTcpSocket;
class QWidget;

namespace PJ {

class MainWindow;
struct ToolboxPanelSession;

/// The in-app MCP server: loopback HTTP/1.1 (JSON responses, `Connection: close`) answering both
/// the 2026-07-28 stateless protocol and the 2025 initialize one, behind bearer tokens. Lives on
/// the GUI thread, owned by MainWindow, which destroys it before its own services. A tool may hold
/// its reply (an import, a dialog the user answers) while the event loop keeps serving others.
///
/// Two kinds of token. The public one (openPublicAccess) is in the discovery file for any local
/// agent: the full tool set, each request acting as `mcp::ownerFor(<client name>)` (the bridge's
/// X-PJ-Client header or the request's `_meta`), in a service assembly built per owner. A plugin
/// token (addPluginToken) is handed to one toolbox plugin only: the agent tools alone, run against
/// that plugin's own session's services (which carry its identity); no client-claimed name counts.
class McpServer : public QObject {
  Q_OBJECT
 public:
  /// Sends a tools/call result (an MCP CallToolResult). Safe after the client left; call it once.
  using Reply = std::function<void(nlohmann::json result)>;

  /// Listens on a free 127.0.0.1 port; every request is refused until a token is added. The
  /// tokens are never logged.
  explicit McpServer(MainWindow& window);
  /// Drops pending deferred results, then the agents' service assemblies they point into (their
  /// tabs and derived series stay, like a closed toolbox's), and removes the discovery file.
  /// Plugin tokens die with it; their sessions' services are not touched.
  ~McpServer() override;

  [[nodiscard]] bool listening() const {
    return server_.isListening();
  }
  [[nodiscard]] quint16 port() const {
    return server_.serverPort();
  }
  [[nodiscard]] bool publicAccess() const {
    return !public_token_.isEmpty();
  }

  /// Opens public access with a new random token and writes the owner-only discovery file
  /// `<instancesDir>/<pid>.json` the bridge reads, after deleting the files of instances whose
  /// process is gone. A no-op while open.
  void openPublicAccess();
  /// Revokes the public token (as revoke does a plugin token; app work its calls left for later,
  /// such as a dialog to open or the user's answer to one, then applies nothing), drops the agents'
  /// service assemblies and removes the discovery file. Plugin tokens keep working.
  void closePublicAccess();

  /// A new random token serving only the agent tools, run against `services` (a toolbox session's
  /// registry, which names the plugin they act as) as `owner`, that plugin's id (the owner of its
  /// tabs, for the host-side zoom). `services` must outlive the token: revoke it before they go.
  [[nodiscard]] QByteArray addPluginToken(PJ_service_registry_t services, const QString& owner);
  /// Revokes plugin token `token` synchronously: no request carrying it is accepted after this
  /// returns, its queued results are dropped (before the services they point into can die) and its
  /// open connections are closed. Side effects its calls already caused stay. No-op for an unknown
  /// token.
  void revoke(const QByteArray& token);

 signals:
  /// The public client that made the latest request (its printable name), or "" once none has
  /// made one for a while or public access closed. Drives the status-bar indicator.
  void activeClientChanged(const QString& client);

 private:
  friend class McpTestPeer;

  /// Who made a tools/call: the token it was accepted with.
  struct Caller {
    QByteArray token;
    /// The public client's owner (`mcp::ownerFor` its name); "" for a plugin token, which never
    /// reaches what reads it (the app tools and agentContext).
    QString owner;
    /// The client's connection; gone once it disconnected, and its deferred result with it.
    QPointer<QTcpSocket> socket;
  };
  /// An app tool: answers through `reply`, now or later, exactly once. Work it leaves for later
  /// (a timer, a queued load, the user's answer to a dialog) asks stillAuthorized(caller) first.
  using AppTool = void (McpServer::*)(const nlohmann::json& args, const Caller& caller, Reply reply);
  /// An agent tool's result still in progress (an object evaluation), resumed on resume_tick_.
  struct Deferred {
    std::string tool;
    Caller caller;
    std::function<agent_tools::ToolResult(agent_tools::ToolContext&)> resume;
    Reply reply;
    /// The resumeDeferred round that queued it, which does not resume it again.
    quint64 round = 0;
  };

  void onReadyRead(QTcpSocket* socket);
  /// Whether `caller` holds the public token: the full tool set, run as `caller.owner`.
  [[nodiscard]] bool isPublic(const Caller& caller) const {
    return !public_token_.isEmpty() && caller.token == public_token_;
  }
  /// Whether `caller`'s token is still accepted (not revoked, public access not closed).
  [[nodiscard]] bool authorized(const Caller& caller) const {
    return isPublic(caller) || plugin_tokens_.contains(caller.token);
  }
  /// Asks authorized(caller) later; false once this server is gone too.
  [[nodiscard]] std::function<bool()> stillAuthorized(const Caller& caller);
  /// Drops `token`'s queued results (they point into its services) and closes its open connections.
  void dropCallsOf(const QByteArray& token);
  /// Answers one complete request. Everything that reads the body runs inside one exception boundary.
  void handle(QTcpSocket* socket, const mcp::HttpRequest& request);
  /// Notes the client for the indicator.
  void noteClient(const QString& client);
  /// Runs tool `name` for `caller`; an app tool `caller` may not call is unknown. While the user has
  /// a modal dialog open, only read-only tools run. A throwing handler (a wrongly typed argument)
  /// becomes a tool error.
  void callTool(const std::string& name, const nlohmann::json& args, const Caller& caller, Reply reply);
  /// What the user sees as `widget`'s title (without the modified-marker placeholder).
  [[nodiscard]] static std::string shownTitle(const QWidget& widget);
  /// The refusal of a mutating tool while the user has `modal` open.
  [[nodiscard]] static nlohmann::json userDialogOpen(const QWidget& modal);
  /// The app's own tools by name (McpServerTools.cpp).
  [[nodiscard]] static const std::unordered_map<std::string, AppTool>& appTools();
  /// Dataset by numeric id, display name or source path; 0 when none matches.
  [[nodiscard]] quint32 resolveDataset(const nlohmann::json& selector) const;
  /// The agent tool context for `owner`: the services a toolbox plugin with that id gets, built on
  /// first use (registering `owner` as an ad-hoc owner, so its tabs and rows say "AI agent").
  /// nullopt when the host refused to build them.
  [[nodiscard]] std::optional<agent_tools::ToolContext> agentContext(const QString& owner);
  /// The agent tool context `caller`'s token gives: the public owner's agentContext, or its plugin
  /// session's services. nullopt once the token is revoked or the services could not be built.
  [[nodiscard]] std::optional<agent_tools::ToolContext> contextFor(const Caller& caller);
  /// What `caller` would find loaded (datasets with their display time ranges, topics, numeric fields), for the
  /// initialize and server/discover instructions; empty when nothing is loaded. Capped, since a client repeats it in
  /// every request. Runs on the GUI thread, over one catalog snapshot.
  [[nodiscard]] std::string loadedSummary(const Caller& caller);
  /// Replies with `result`, or queues it on resume_tick_ while it is deferred. Drops it once
  /// `caller`'s token is revoked (a continuation's follow-up included).
  void runAgentTool(const std::string& tool, const Caller& caller, agent_tools::ToolResult result, Reply reply);
  /// One resume of every deferred result queued before this round, each taken out of deferred_
  /// only while it runs, so a revocation it causes (a session torn down) still drops the others
  /// before their services die. One whose client left or whose token was revoked is dropped; a
  /// mutating tool waits while the user has a modal dialog. A call made while one is running does
  /// nothing.
  void resumeDeferred();
  /// PNG of the main window with every visible QRhiWidget's next presented frame composited in, and
  /// any open modal dialog drawn over a dimmed window. Asynchronous; `done` never runs after this
  /// server is gone.
  void grabScreenshot(int max_width, std::function<void(const QByteArray& png)> done);
  /// Holds `reply` until load `ticket` settles; `on_loaded` words the success.
  void awaitLoad(quint64 ticket, Reply reply, std::function<nlohmann::json(quint32 dataset)> on_loaded);

  // The app tools, one per tools/list entry (McpServerTools.cpp).
  void getStatus(const nlohmann::json& args, const Caller& caller, Reply reply);
  void about(const nlohmann::json& args, const Caller& caller, Reply reply);
  void openFile(const nlohmann::json& args, const Caller& caller, Reply reply);
  void reloadFile(const nlohmann::json& args, const Caller& caller, Reply reply);
  void unloadFile(const nlohmann::json& args, const Caller& caller, Reply reply);
  void startStream(const nlohmann::json& args, const Caller& caller, Reply reply);
  void stopStream(const nlohmann::json& args, const Caller& caller, Reply reply);
  void exportSeries(const nlohmann::json& args, const Caller& caller, Reply reply);
  void addScriptSeries(const nlohmann::json& args, const Caller& caller, Reply reply);
  void addFilter(const nlohmann::json& args, const Caller& caller, Reply reply);
  void removeScriptSeries(const nlohmann::json& args, const Caller& caller, Reply reply);
  void screenshot(const nlohmann::json& args, const Caller& caller, Reply reply);

  MainWindow& window_;
  QTcpServer server_;
  /// What a plugin token's tools run against: its session's services, as its plugin.
  struct PluginToken {
    PJ_service_registry_t services;
    QString owner;
  };
  QHash<QByteArray, PluginToken> plugin_tokens_;
  /// "" while public access is closed.
  QByteArray public_token_;
  QString discovery_path_;
  /// Bytes of each socket whose request is still incomplete; an entry also means "may time out".
  QHash<QTcpSocket*, QByteArray> pending_;
  int open_sockets_ = 0;
  /// Clears the indicator when no client has called for a while.
  QTimer idle_;
  QString active_client_;
  /// One toolbox service assembly per public agent owner. Declared before deferred_, whose
  /// continuations point into it, so it is destroyed after them.
  QHash<QString, std::shared_ptr<ToolboxPanelSession>> agent_sessions_;
  std::vector<Deferred> deferred_;
  /// Numbers resumeDeferred's rounds (Deferred::round).
  quint64 resume_round_ = 0;
  /// Whether resumeDeferred is running. A nested call (a reply or continuation that spins an event
  /// loop) would advance resume_round_ under the outer loop, which would then treat the follow-ups
  /// the nested call queued as due, again and again; so it returns at once.
  bool draining_ = false;
  QTimer resume_tick_;
};

/// The "pj.mcp_endpoint.v1" host of one toolbox session: the plugin's private token on the
/// window's MCP server (a plugin token, see McpServer). Destroying it releases the token, so the
/// session destroys it after the plugin instance (which may release from its destructor) and
/// before the services the token reaches. Copes with the server dying first (its tokens die with
/// it): release then does nothing, and the next acquire starts a new server when it can, else
/// fails. Main thread.
class McpEndpointHost {
 public:
  /// `server` returns the window's MCP server, starting it when needed, or nullptr when it cannot
  /// listen. `services` (the session's registry, which names the plugin) must outlive this;
  /// `owner` is that plugin's id.
  McpEndpointHost(std::function<McpServer*()> server, PJ_service_registry_t services, QString owner);
  ~McpEndpointHost();
  McpEndpointHost(const McpEndpointHost&) = delete;
  McpEndpointHost& operator=(const McpEndpointHost&) = delete;

  /// The service to register; points at this object.
  [[nodiscard]] PJ_mcp_endpoint_host_t raw() noexcept;

 private:
  bool acquire(PJ_mcp_endpoint_t& out, PJ_error_t* out_error);
  void release();

  std::function<McpServer*()> server_on_demand_;
  PJ_service_registry_t services_;
  QString owner_;
  /// The server that accepts token_; null while none is held (also once that server is gone).
  QPointer<McpServer> server_;
  /// What acquire handed out; kept until release, as the contract requires.
  std::string url_;
  QByteArray token_;
};

}  // namespace PJ
