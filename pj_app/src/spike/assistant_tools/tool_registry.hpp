// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "derived_recipes/recipes.hpp"
#include "tool_context.hpp"

namespace assistant_agent {

// Runs one tool call and returns its result, blocking the caller's thread until
// the GUI thread executes it (see GuiExecutor). This is the seam a backend uses
// to run a tool the model asked for, without ever touching host services from
// its own worker thread.
using ToolInvoker = std::function<ToolResult(const std::string& name, const nlohmann::json& args)>;

// One tool the assistant can call. `input_schema` is a JSON Schema object (the
// shape MCP and every OpenAI-style function-spec consumer both want), so a
// single registry drives the MCP `tools/list` response, any function-spec
// serialization, AND the unit tests — no second source of truth to drift.
struct ToolSpec {
  std::string name;
  std::string description;
  nlohmann::json input_schema;
  std::function<ToolResult(const nlohmann::json& args, ToolContext& ctx)> executor;
};

// The fixed catalog of assistant tools. Read-only + additive by construction:
// list/describe/read topics, create derived series + markers, report status.
// There is deliberately no delete/destroy tool — the plugin ABI exposes no such
// host op, so one could not be built even if asked for.
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

  // Serialize the catalog for each backend's tool-advertisement format.
  [[nodiscard]] nlohmann::json toFunctionSpecs() const;  // OpenAI-style function specs
  [[nodiscard]] nlohmann::json toMcpToolsList() const;   // MCP tools/list entries

 private:
  void add(ToolSpec spec);
  std::vector<ToolSpec> tools_;
};

// A plain-text listing of what is currently loaded — topics, their fields and
// types — for handing to the model up front so it does not have to ask.
//
// This is metadata the host already holds, so building it is free; it
// deliberately carries no statistics (min/max would mean scanning every series,
// which just moves the cost rather than removing it).
//
// Bounded by `budget_chars`. Per topic this picks the cheapest rendering that
// still fits — every field with its type, a partial field list, or just a
// field count — before falling back to the old two-step degradation (full
// tree, then topic names only) for a budget too tight even for bare counts.
// Whenever anything is left out the text says so explicitly — a model that
// believes an incomplete listing is the whole truth will confidently tell the
// user a signal does not exist.
[[nodiscard]] std::string catalogDigest(
    const PJ::sdk::ToolboxHostView& host, std::size_t budget_chars = 10000,
    const PJ::sdk::PlaybackHostView& playback = {});

// Path resolution lives in the shared derived_recipes library (also used by the Transform
// Editor); re-exported here so the assistant's code and tests keep their names. Resolution
// is where the multi-dataset rules live (the "dataset:topic/field" qualifier, ambiguity
// refusal with qualified candidates).
using derived_recipes::ResolvedSeries;
using derived_recipes::resolveSeriesPath;
using derived_recipes::SeriesLookup;

}  // namespace assistant_agent
