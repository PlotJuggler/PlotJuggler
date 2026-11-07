#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// SPIKE: MCP vocabulary shared by the in-app server and the stdio bridge.

#include <QString>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "tool_registry.hpp"  // spike/assistant_tools

namespace PJ::mcp {

/// Every tool an instance serves, with JSON schemas and MCP annotations. Static, so the
/// bridge can list tools while no PlotJuggler runs.
[[nodiscard]] nlohmann::json toolList();

/// The Assistant Agent's tool layer (spike/assistant_tools), one instance for listing and calls.
[[nodiscard]] const assistant_agent::ToolRegistry& assistantTools();

/// True for tools that never change the session; the only ones allowed while the user has
/// a modal dialog open.
[[nodiscard]] bool isReadOnlyTool(const std::string& name);

/// The result of a lifecycle request (`initialize`, `server/discover`, `ping`), or nullopt
/// for any other method. `initialize` echoes a protocol version this server knows.
[[nodiscard]] std::optional<nlohmann::json> lifecycleResult(const std::string& method, const nlohmann::json& params);

/// The client's name from `initialize` params (2025) or a request's `_meta` (2026); "" when absent.
[[nodiscard]] std::string clientName(const nlohmann::json& params);

/// A tab-owner id for an MCP client name ("claude-code" -> "agent-claude-code"): each client
/// gets its own tab namespace; "mcp-agent" when the client gave no name.
[[nodiscard]] std::string ownerFor(const std::string& client);

/// Per-user folder holding one `<pid>.json` discovery file per running instance with MCP on.
/// Created owner-only. Needs the application/organization names set (Windows/macOS paths use them).
[[nodiscard]] QString instancesDir();

/// The file to start this program again: the AppImage file when run from one (its mount vanishes
/// with this process), else the binary.
[[nodiscard]] QString selfExecutable();

/// Records this executable's path in instancesDir()/executable (the AppImage file, not its mount,
/// when run from one), so an agent plugin's launcher finds PlotJuggler where PATH does not.
/// Rewrites only on change; call once per GUI start, after the application names are set.
void recordExecutable();

/// `plotjuggler4 --mcp-bridge`: MCP over stdio, forwarding to a running instance found in
/// instancesDir(). Runs before any QApplication; returns the process exit code.
int runBridge(int argc, char* argv[]);

}  // namespace PJ::mcp
