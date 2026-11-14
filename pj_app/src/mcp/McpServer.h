#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#ifndef PJ_TARGET_WASM
#include <QTcpServer>
#endif
#include <QTimer>
#include <QUrl>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef PJ_TARGET_WASM
#include "BrowserFileStore.h"
#endif
#include "DialogWatch.h"
#include "mcp/McpProtocol.h"
#include "pj_base/plugin_data_api.h"
#include "pj_runtime/agent_tools/tool_context.hpp"

class QTcpSocket;
class QWidget;

namespace PJ {

class MainWindow;
struct LoadHints;
struct LoadInput;
struct ToolboxPanelSession;

/// The in-app MCP server: loopback HTTP/1.1 (JSON responses, `Connection: close`) answering both
/// the 2026-07-28 stateless protocol and the 2025 initialize one, behind bearer tokens. Lives on
/// the GUI thread, owned by MainWindow, which destroys it before its own services. A tool may hold
/// its reply (an import, a dialog the user answers) while the event loop keeps serving others.
/// The browser build cannot listen: there it only answers what a relay link brings in
/// (handleRelayed, McpBrowserLink), always as a public client.
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

  /// Listens on a free 127.0.0.1 port (not in the browser); every request is refused until a token
  /// is added. The tokens are never logged.
  explicit McpServer(MainWindow& window);
  /// Drops pending deferred results, then the agents' service assemblies they point into (their
  /// tabs and derived series stay, like a closed toolbox's), and removes the discovery file.
  /// Plugin tokens die with it; their sessions' services are not touched.
  ~McpServer() override;

#ifndef PJ_TARGET_WASM
  [[nodiscard]] bool listening() const {
    return server_.isListening();
  }
  [[nodiscard]] quint16 port() const {
    return server_.serverPort();
  }
#endif
  [[nodiscard]] bool publicAccess() const {
    return !public_token_.isEmpty();
  }

  /// Opens public access with a new random token and writes the owner-only discovery file
  /// `<instancesDir>/<pid>.json` the bridge reads, after deleting the files of instances whose
  /// process is gone (desktop only: the browser has no bridge to discover it). A no-op while open.
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

  /// One JSON-RPC message an agent sent through a relay link (McpBrowserLink), answered as the
  /// public client `client` (its MCP client name). `send` gets the response document, or an empty
  /// array for a notification, exactly once, possibly later; a held reply is dropped once
  /// `connection` is destroyed. Refused (an error response) while public access is closed.
  void handleRelayed(
      const QByteArray& body, const QString& client, QObject* connection, std::function<void(QByteArray)> send);

  /// One unfinished job, for the user's view of agent activity (every client's, unlike jobs_list).
  struct JobInfo {
    quint64 id = 0;
    /// Who runs it: the public client's owner, or the plugin's id.
    QString client;
    QString summary;
    QString state;
    double elapsed_s = 0.0;
  };
  /// Every client's jobs that have not ended, oldest first.
  [[nodiscard]] std::vector<JobInfo> unfinishedJobs() const;
  /// The user's Stop: as that job's own client's job_kill, keeping what an import already loaded.
  void stopJob(quint64 id);

 signals:
  /// A job started or ended (unfinishedJobs changed).
  void jobsChanged();
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
    /// The client's connection (its socket, or the object McpBrowserLink gives the relay session
    /// that brought the request); gone once it disconnected. Whether a call's work then stops is
    /// per tool (see serviceWaiters).
    QPointer<QObject> connection;
    /// The call's JSON-RPC id: what a `notifications/cancelled` from the same owner names (the bridge
    /// sends ids unique across its sessions).
    nlohmann::json request_id;
    /// The job this call runs as (an app tool registers its kill hook there); 0 for none.
    quint64 job = 0;
    /// The request declared the MCP Tasks extension (mcp::declaresTasks): its call may be answered
    /// with a task handle.
    bool tasks = false;
  };
  /// An app tool: answers through `reply`, now or later, exactly once. Work it leaves for later
  /// (a timer, a queued load, the user's answer to a dialog) asks stillAuthorized(caller) first.
  using AppTool = void (McpServer::*)(const nlohmann::json& args, const Caller& caller, Reply reply);

  using Clock = std::chrono::steady_clock;
  enum class JobState { kRunning, kWaitingForUser, kCompleted, kFailed, kKilled };
  /// Someone waiting on a job: the call that started it (until `deadline`, when it gets the job id
  /// and the job goes on in the background), a job_wait, or a job_kill.
  struct Waiter {
    enum class Kind { kCall, kWait, kKill } kind = Kind::kCall;
    Reply reply;
    /// The waiting request's connection: once gone, nobody reads the reply.
    QPointer<QObject> connection;
    Clock::time_point deadline;
  };
  /// A job-capable call (mcp::isJobCapableTool): an agent tool's continuation (an object
  /// evaluation) resumed on resume_tick_, or an app tool's work (a file load) that reports through
  /// finishJob and stops through `kill`. While its call still waits on it (a kCall waiter), it is
  /// in the foreground. Once its id is known (`retained`) its result is kept for job_wait until it
  /// expires (pruneJobs).
  struct Job {
    quint64 id = 0;
    /// Who started it, and so who may see and stop it (sameClient).
    Caller caller;
    /// The caller as the user's view names it (JobInfo::client).
    QString client;
    /// Its MCP task id, once its call was answered with a task handle: unguessable, unlike `id`.
    std::string task_id;
    /// When it started, on the wall clock (a task's createdAt).
    QDateTime created;
    std::string tool;
    std::string summary;
    JobState state = JobState::kRunning;
    Clock::time_point started;
    Clock::time_point ended_at;
    /// A stop was asked for: whatever result comes next ends the job as killed.
    bool killing = false;
    /// Its id is known to its client (its call got it at wait_s, was answered early or hung up
    /// on work that carries on, or jobs_list showed it), so its result is kept when it ends;
    /// else it is forgotten then.
    bool retained = false;
    /// The agent continuation (empty while it runs and once ended). Destroying it releases its
    /// evaluation, which stops the work, so it must die while its token's services live.
    std::function<agent_tools::ToolResult(agent_tools::ToolContext&)> resume;
    /// The resumeDeferred round that last resumed or queued it, which does not resume it again.
    quint64 round = 0;
    /// An app tool's stop (e.g. FileLoader::cancelLoad): it only asks, and the work's own end then
    /// finishes the job (finishJob). Never blocks nor spins an event loop.
    std::function<void(bool keep_partial)> kill;
    /// Closes the dialog this job's work has open for the user, as their Cancel would; set while
    /// it is up (dialogWatch), when the job reads waiting_for_user.
    std::function<void()> close_dialog;
    /// What the job has done so far, cheap; unset when nothing is known.
    std::function<nlohmann::json()> progress;
    /// The tool's result, once ended.
    nlohmann::json result;
    std::vector<Waiter> waiters;
  };

#ifndef PJ_TARGET_WASM
  void onReadyRead(QTcpSocket* socket);
  /// Answers one complete request: the transport checks (token, method, path), then dispatch.
  void handle(QTcpSocket* socket, const mcp::HttpRequest& request);
#endif
  /// Whether `caller` holds the public token: the full tool set, run as `caller.owner`.
  [[nodiscard]] bool isPublic(const Caller& caller) const {
    return !public_token_.isEmpty() && caller.token == public_token_;
  }
  /// Whether `caller`'s token is still accepted (not revoked, public access not closed).
  [[nodiscard]] bool authorized(const Caller& caller) const {
    return isPublic(caller) || plugin_tokens_.contains(caller.token);
  }
  /// Asks later whether `caller` may still act: its token is accepted, this server lives, and the
  /// job the call runs as (if any) has neither ended nor been asked to stop.
  [[nodiscard]] std::function<bool()> stillAuthorized(const Caller& caller);
  /// Kills and forgets `token`'s jobs (their continuations point into its services), and closes its
  /// open connections.
  void dropCallsOf(const QByteArray& token);
  /// Answers one JSON-RPC message from `caller`, whose transport accepted it. `header_client` is the
  /// client name its transport carried ("" for none). `respond(status, body)` runs once: 200 with
  /// the response, or 202 with no body for a notification. Everything that reads the body runs
  /// inside one exception boundary.
  void dispatch(
      Caller caller, const QByteArray& body, const QString& header_client,
      std::function<void(int status, QByteArray body)> respond);
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
  /// One tick: first the waiters (serviceWaiters), then one resume of every continuation queued
  /// before this round. A continuation runs taken out of its job, so a revocation it causes (a
  /// session torn down) still drops the other jobs before their services die. A mutating tool
  /// waits while the user has a modal dialog. A call made while one is running does nothing.
  void resumeDeferred();
  /// Answers the waiters past their deadline (a call gets the job id, or a task handle, and its job
  /// goes on in the background) and drops those whose client left. A job whose call left dies with
  /// it when nothing would read its result or act on its question (an agent tool's, unload_file's
  /// prompt); other app work (an import, a stream, an export) carries on for its client to find.
  void serviceWaiters();

  /// Whether two callers are the same client, which shares its jobs: the same token (a plugin's is
  /// its own) and, on the public token, the same owner.
  [[nodiscard]] static bool sameClient(const Caller& a, const Caller& b) {
    return a.token == b.token && a.owner == b.owner;
  }
  [[nodiscard]] static bool ended(const Job& job) {
    return job.state != JobState::kRunning && job.state != JobState::kWaitingForUser;
  }
  /// How many jobs of `caller`'s client have not ended (stopping ones included).
  [[nodiscard]] int runningJobs(const Caller& caller) const;
  /// Registers a job for `caller`'s call of `tool`, its `reply` waiting the call's wait_s on it; 0
  /// (and `reply` refused) when the client already runs kMaxRunningJobs.
  quint64 openJob(const std::string& tool, const nlohmann::json& args, const Caller& caller, Reply reply);
  /// Adds a waiter of `kind` on `job` for `seconds`, answered on `caller`'s connection.
  static void addWaiter(Job& job, Waiter::Kind kind, const Caller& caller, Reply reply, double seconds);
  /// Job `id` while it has not ended; null for an unknown or ended one.
  [[nodiscard]] Job* liveJob(quint64 id);
  /// What an agent tool returned for job `id`: a continuation to resume on the next tick, or the
  /// result that ends it. Dropped when the job was killed or its token revoked meanwhile.
  void continueJob(quint64 id, agent_tools::ToolResult result);
  /// Ends job `id` with tool result `result` (completed, or failed, or killed when it failed after
  /// a stop was asked for) and answers its waiters; a job whose id nobody knows is forgotten. No-op
  /// for an ended or unknown job (a load's terminal after a revocation).
  void finishJob(quint64 id, nlohmann::json result);
  /// Stops job `id`: closes the dialog it has open and calls its kill hook, whose terminal result
  /// then ends it; with neither, ends it at once (destroying its continuation, which releases its
  /// evaluation).
  void killJob(quint64 id, bool keep_partial);
  /// For work of job `id` that may ask the user: keeps the closer of its dialog while it is up
  /// (and closes it at once when the job was stopped meanwhile). Safe after this server is gone.
  [[nodiscard]] dialog_presenter::DialogWatch dialogWatch(quint64 id);
  /// Answers the call still waiting on job `id` with `result` while the job carries on (a stream
  /// that started); nothing when the call already got its job id or left.
  void answerCall(quint64 id, const nlohmann::json& result);
  /// Forgets job `id`, answering nobody: taken out of the table before it (and its continuation)
  /// is destroyed.
  void dropJob(quint64 id);
  /// Job `id` if it is one of `caller`'s client.
  [[nodiscard]] Job* ownedJob(const Caller& caller, const nlohmann::json& id);
  /// How `job` reads to an agent: running, waiting_for_user (its own dialog is up, or the user's
  /// holds it back), completed, failed, killed.
  [[nodiscard]] static const char* stateName(const Job& job);
  /// Seconds `job` has run, or ran, to a tenth.
  [[nodiscard]] static double elapsedSeconds(const Job& job);
  /// {job_id, tool, summary, state, elapsed_s} of `job`, with its progress when it reports any and
  /// `stopping` while a stop it was asked for has not taken effect.
  [[nodiscard]] static nlohmann::json jobStatus(const Job& job);
  /// What job_kill answers: `job`'s status with its result's text once ended.
  [[nodiscard]] static nlohmann::json killReply(const Job& job);
  /// Forgets ended jobs past their expiry, and all but the newest ended ones of each client.
  void pruneJobs();
  /// Runs resume_tick_ while a continuation or a waiter needs it.
  void updateTick();
  /// `notifications/cancelled`: kills the one foreground call of `caller`'s client with id
  /// `request_id`; nothing when none or several match.
  void cancelRequest(const Caller& caller, const nlohmann::json& request_id);

  /// `job` as an MCP Task: taskId, status (working, completed, cancelled; a tool error is a
  /// completed task, as the extension has it), statusMessage, createdAt, lastUpdatedAt, ttlMs,
  /// pollIntervalMs. Gives the job its task id on first use.
  [[nodiscard]] static nlohmann::json taskOf(Job& job);
  /// The whole JSON-RPC response to `request`, one of the MCP Tasks methods (mcp::isTasksMethod),
  /// from `caller`: an error when it did not declare the extension, or when the task is not one of
  /// its client's.
  [[nodiscard]] nlohmann::json taskRequest(const mcp::RpcMessage& request, const Caller& caller);

  // The job tools, served to every token.
  void jobsList(const nlohmann::json& args, const Caller& caller, Reply reply);
  void jobWait(const nlohmann::json& args, const Caller& caller, Reply reply);
  void jobKill(const nlohmann::json& args, const Caller& caller, Reply reply);
  /// PNG of the main window with every visible QRhiWidget's next presented frame composited in, and
  /// any open modal dialog drawn over a dimmed window. Asynchronous; `done` never runs after this
  /// server is gone.
  void grabScreenshot(int max_width, std::function<void(const QByteArray& png)> done);
  /// Queues `input` for loading and holds `reply` until it settles (open_file's second half, and in
  /// the browser reload_file's: `reload` as awaitLoad's). `loaded`, when given, hears the dataset
  /// of a load that succeeded, before the answer is worded.
  void startLoad(
      LoadInput input, LoadHints hints, quint64 job, Reply reply, bool reload = false,
      std::function<void(quint32 dataset)> loaded = {});
  /// Holds `reply` until load `ticket` settles; `on_loaded` words the success. Gives job `job` its
  /// kill (cancelling the load) and its progress. A `reload` replaces data the user already has, so
  /// its kill always discards: keep_partial would leave half of the replacement in its place.
  void awaitLoad(
      quint64 ticket, quint64 job, Reply reply, std::function<nlohmann::json(quint32 dataset)> on_loaded,
      bool reload = false);
  /// Ties job `job` to the stream into dataset `id` it started: the job stops the stream when
  /// killed, and ends when the stream does (stopped by anyone, ended on its own, or failed).
  void followStream(quint64 job, quint32 id);

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
#ifndef PJ_TARGET_WASM
  QTcpServer server_;
#endif
  /// What a plugin token's tools run against: its session's services, as its plugin.
  struct PluginToken {
    PJ_service_registry_t services;
    QString owner;
  };
  QHash<QByteArray, PluginToken> plugin_tokens_;
  /// "" while public access is closed.
  QByteArray public_token_;
#ifndef PJ_TARGET_WASM
  QString discovery_path_;
  /// Bytes of each socket whose request is still incomplete; an entry also means "may time out".
  QHash<QTcpSocket*, QByteArray> pending_;
  int open_sockets_ = 0;
#endif
  /// Clears the indicator when no client has called for a while.
  QTimer idle_;
  QString active_client_;
  /// One toolbox service assembly per public agent owner. Declared before jobs_, whose
  /// continuations point into it, so it is destroyed after them.
  QHash<QString, std::shared_ptr<ToolboxPanelSession>> agent_sessions_;
  /// Every job by id; ids are never reused within a run.
  std::map<quint64, Job> jobs_;
  quint64 last_job_id_ = 0;
  /// Numbers resumeDeferred's rounds (Job::round).
  quint64 resume_round_ = 0;
  /// Whether resumeDeferred is running. A nested call (a reply or continuation that spins an event
  /// loop) would advance resume_round_ under the outer loop, which would then treat the follow-ups
  /// the nested call queued as due, again and again; so it returns at once.
  bool draining_ = false;
  QTimer resume_tick_;
#ifdef PJ_TARGET_WASM
  /// Downloads `url`, a file the agent's relay serves (the page cannot read the agent's disk), into
  /// a staged file and loads it with `hints`. Until the load is queued the job's kill stops the
  /// download. `reload` as awaitLoad's.
  void fetchAndLoad(
      const QUrl& url, const QString& name, LoadHints hints, const Caller& caller, Reply reply, bool reload);

  /// A file the relay served, by name, and the source identity of the dataset loaded from it.
  struct RelayFile {
    QUrl url;
    QString name;
    QString identity;
  };
  /// Where open_file stages the files the relay serves (each removed with its dataset's lease).
  BrowserFileStore agent_files_{QStringLiteral("/pj_agent_files")};
  /// What each dataset this server loaded was downloaded from. The relay serves the file again as
  /// it is now, which is reload_file's only way to read a file anew in a browser. An entry holds
  /// only while the dataset still has the identity it was loaded with: the user's Replace keeps the
  /// dataset's id and gives it another source.
  std::map<quint32, RelayFile> relay_files_;
#endif
};

#ifndef PJ_TARGET_WASM
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
#endif

}  // namespace PJ
