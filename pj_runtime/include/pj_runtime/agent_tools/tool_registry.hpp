// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "pj_runtime/agent_tools/derived_recipes.hpp"
#include "pj_runtime/agent_tools/tool_context.hpp"

namespace PJ::agent_tools {

// One tool an agent can call. `input_schema` is a JSON Schema object, so one
// registry drives the MCP `tools/list` response and the unit tests: no second
// source of truth to drift.
struct ToolSpec {
  std::string name;
  std::string description;
  nlohmann::json input_schema;
  std::function<ToolResult(const nlohmann::json& args, ToolContext& ctx)> executor;
};

// The fixed catalog of agent tools: read the catalog and series, evaluate and
// create derived series, objects and markers, drive playback and the agent's own
// plot and scene tabs. The only removals are of what the calling agent created
// itself; loaded data cannot be deleted.
class ToolRegistry {
 public:
  ToolRegistry();

  [[nodiscard]] const std::vector<ToolSpec>& tools() const {
    return tools_;
  }
  [[nodiscard]] const ToolSpec* find(std::string_view name) const;

  // Execute by name. Unknown tool -> a failure ToolResult (never throws), so a
  // model that hallucinates a tool name gets a correctable error, not a crash.
  [[nodiscard]] ToolResult execute(std::string_view name, const nlohmann::json& args, ToolContext& ctx) const;

  // The catalog as MCP `tools/list` entries (name, description, inputSchema).
  [[nodiscard]] nlohmann::json toMcpToolsList() const;

 private:
  void add(ToolSpec spec);
  std::vector<ToolSpec> tools_;
};

}  // namespace PJ::agent_tools
