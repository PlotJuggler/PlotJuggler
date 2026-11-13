#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The MCP vocabulary the in-app server and the stdio bridge share: JSON-RPC envelopes, both
// protocol eras, the tool list, client-to-owner mapping, the HTTP/1.1 subset the server speaks
// and the discovery folder. QtCore only (the bridge runs before any QApplication).

#include <QByteArray>
#include <QHash>
#include <QLoggingCategory>
#include <QString>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "pj_runtime/agent_tools/tool_registry.hpp"

namespace PJ::mcp {

Q_DECLARE_LOGGING_CATEGORY(lcMcp)

inline constexpr qsizetype kMaxHeaderBytes = 16 * 1024;
/// Also the bridge's limit on one stdin line.
inline constexpr qsizetype kMaxBodyBytes = 1024 * 1024;
/// JSON nested deeper than this is refused: no tool takes more than a few levels, and a deep
/// document would recurse through every later copy, compare and dump.
inline constexpr int kMaxJsonDepth = 64;
inline constexpr int kMaxOpenSockets = 16;
/// An HTTP request still incomplete after this long is closed. A complete one may be held for
/// longer (an import, a dialog the user is answering).
inline constexpr int kIncompleteRequestTimeoutMs = 10'000;
/// The header the bridge names its MCP client in. The server derives the owner from it.
inline constexpr char kClientHeader[] = "x-pj-client";

/// What every tool's reply was missing a service assembly says.
inline constexpr char kNoServicesError[] = "PlotJuggler could not build the services this tool needs";

/// An object JSON schema with `properties` and the `required` names.
[[nodiscard]] nlohmann::json schema(nlohmann::json properties, nlohmann::json required = nlohmann::json::array());

/// One tools/list entry with the MCP annotations every tool here carries (readOnlyHint from
/// `read_only`; nothing reaches outside the user's machine).
[[nodiscard]] nlohmann::json toolEntry(
    const std::string& name, const std::string& description, nlohmann::json input_schema, bool read_only);

/// An MCP CallToolResult holding one text block.
[[nodiscard]] nlohmann::json textResult(const std::string& text, bool is_error = false);

/// `value` as compact JSON text. Never throws: bytes that are not UTF-8 (a name from a file) become
/// the replacement character.
[[nodiscard]] std::string dumpJson(const nlohmann::json& value);

/// A successful CallToolResult whose text is `payload` as JSON (dumpJson).
[[nodiscard]] nlohmann::json jsonResult(const nlohmann::json& payload);

/// Which tools a client may call: kFull (the public token, the bridge) or kAgentOnly (a plugin
/// token: the agent tools, without the app's own).
enum class ToolSet { kFull, kAgentOnly };

/// The tools of `set`, with JSON schemas and MCP annotations. Static, so the bridge can list tools
/// while no PlotJuggler runs.
[[nodiscard]] nlohmann::json toolList(ToolSet set);

/// The agent tool layer (pj_runtime's `agent_tools`), one instance for listing and calls.
[[nodiscard]] const agent_tools::ToolRegistry& agentTools();

/// True for tools that never change the session: the only ones allowed while the user has a modal
/// dialog open. `screenshot` is not one (its `tab` focuses a tab); the server lets it through
/// without `tab`.
[[nodiscard]] bool isReadOnlyTool(const std::string& name);

/// Whether a call of `name` with `args` may run while the user has a modal dialog open: the
/// read-only tools, `screenshot` when it focuses no tab, and `job_kill`, which changes state but
/// only stops the agent's own work.
[[nodiscard]] bool runsUnderUsersDialog(const std::string& name, const nlohmann::json& args);

/// How long a job-capable call holds its reply before it becomes a background job (its `wait_s`
/// argument, clamped), and how long job_wait waits (`timeout_s`). Kept under the bridge's per-call
/// timeout and the shorter ones of common clients.
inline constexpr double kDefaultWaitS = 20.0;
inline constexpr double kMaxWaitS = 120.0;
inline constexpr double kMaxJobWaitS = 60.0;

/// The MCP Tasks extension of protocol 2026-07-28, which this server offers: for a client that
/// declares it on a request, a call that outlasts its wait_s answers with a task handle
/// (`resultType: "task"`) instead of the job-id text, and `tasks/get`, `tasks/update` and
/// `tasks/cancel` reach that job. The job tools keep working beside it.
inline constexpr char kTasksExtension[] = "io.modelcontextprotocol/tasks";
/// The JSON-RPC error a client gets for a tasks/* method when it did not declare kTasksExtension.
inline constexpr int kMissingClientCapability = -32021;

/// Whether `method` is one of the extension's own (`tasks/get`, `tasks/update`, `tasks/cancel`).
[[nodiscard]] bool isTasksMethod(const std::string& method);

/// Whether a request's `params` declare kTasksExtension in their per-request client capabilities
/// (`_meta["io.modelcontextprotocol/clientCapabilities"].extensions`).
[[nodiscard]] bool declaresTasks(const nlohmann::json& params);

/// Whether `name` takes `wait_s`: it can outlive one request (an object evaluation, a file load) and
/// then becomes a background job that job_wait, job_kill and jobs_list reach.
[[nodiscard]] bool isJobCapableTool(const std::string& name);

/// The seconds a call asks to wait in its argument `key`, in [0, `max`]; kDefaultWaitS when absent
/// or not a finite number.
[[nodiscard]] double waitSeconds(const nlohmann::json& args, const char* key = "wait_s", double max = kMaxWaitS);

/// The result of a lifecycle request (`initialize`, `server/discover`, `ping`), or nullopt for any
/// other method. `initialize` echoes a protocol version this server knows (2025 era), else offers
/// its default; `server/discover` lists the versions of both eras. The instructions name the tools
/// of `set` only.
[[nodiscard]] std::optional<nlohmann::json> lifecycleResult(
    const std::string& method, const nlohmann::json& params, ToolSet set);

/// The client's name from `initialize` params (2025) or a request's `_meta` (2026); "" when absent.
[[nodiscard]] std::string clientName(const nlohmann::json& params);

/// What every agent owner starts with. The plugin catalog refuses plugin ids with this prefix
/// (PJ::kAgentIdPrefix, checked equal in McpServer.cpp), so an agent never shares a plugin's identity.
inline constexpr char kAgentOwnerPrefix[] = "agent-";

/// The owner (plugin-id namespace for tabs and derived series) of an MCP client:
/// `agent-<name>` with the name lower-cased to [a-z0-9-] and cut at 32 characters, `agent-mcp`
/// for no name. Two sessions of one client share the namespace.
[[nodiscard]] std::string ownerFor(const std::string& client);

/// A client name fit for a header and a label: printable ASCII only, at most 64 characters.
[[nodiscard]] std::string printableClientName(const std::string& client);

/// One client message, parsed and checked as a JSON-RPC 2.0 request or notification.
struct RpcMessage {
  /// The request's id once it is a string or an integer; null otherwise (and in replies to a
  /// message whose id is unusable).
  nlohmann::json id{};
  std::string method{};
  nlohmann::json params = nlohmann::json::object();
  /// Set when the message is refused: the complete error response to send (parse error, nesting
  /// deeper than kMaxJsonDepth, or an envelope that is not JSON-RPC 2.0).
  std::optional<nlohmann::json> error{};
  /// A valid message without an id: it gets no reply.
  bool notification = false;
};

/// Parses one message: a string or integer `id` when present, a string `method`, an object
/// `params` when present. Never throws.
[[nodiscard]] RpcMessage parseRpcMessage(const std::string& text);

/// A JSON-RPC error response, with the error's `data` when given; `id` is null when the request's
/// own id is unusable.
[[nodiscard]] nlohmann::json errorResponse(
    const nlohmann::json& id, int code, const std::string& message, const nlohmann::json& data = nullptr);

/// One parsed HTTP/1.1 request.
struct HttpRequest {
  QByteArray method;
  QByteArray path;
  QHash<QByteArray, QByteArray> headers;  ///< lower-case names
  QByteArray body;
};

/// Parses the bytes received so far: 0 while the request is incomplete, 200 with `out` filled once
/// it is complete, else the HTTP status to refuse it with (400 malformed or a bad Content-Length,
/// 413 body over kMaxBodyBytes, 431 header over kMaxHeaderBytes). Bytes after the body are ignored
/// (the server answers one request per connection).
[[nodiscard]] int parseHttpRequest(const QByteArray& buffer, HttpRequest& out);

/// 0 when `request` may be served, else the status refusing it: 403 for any `Origin` header (a
/// browser page; agents send none), 401 without `Authorization: Bearer <token>`. The token is
/// compared in constant time.
[[nodiscard]] int checkAccess(const HttpRequest& request, const QByteArray& token);

/// A complete `Connection: close` response.
[[nodiscard]] QByteArray httpResponse(
    int status, const QByteArray& body = {}, const QByteArray& content_type = "application/json");

/// Per-user folder holding one `<pid>.json` discovery file per running instance with MCP on, and
/// the recorded executable path. Created owner-only. Under AppLocalDataLocation, which needs only
/// HOME: agents start the bridge with a stripped environment. Needs the application and
/// organization names set.
[[nodiscard]] QString instancesDir();

/// The file to start this program again: the AppImage file when run from one (its mount vanishes
/// with this process), else the binary.
[[nodiscard]] QString selfExecutable();

/// Records selfExecutable() in instancesDir()/executable, so an agent plugin's launcher finds
/// PlotJuggler where PATH does not. Rewrites only on change. Call on attended starts only, so tests
/// never repoint it at a build tree.
void recordExecutable();

/// `plotjuggler4 --mcp-bridge`: MCP over stdio, forwarding to a running instance found in
/// instancesDir(). Runs before any QApplication; `version` is the app version it reports. Returns
/// the process exit code.
int runBridge(int argc, char* argv[], const QString& version);

}  // namespace PJ::mcp
