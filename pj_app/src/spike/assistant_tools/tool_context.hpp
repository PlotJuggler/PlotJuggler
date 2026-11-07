// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>

namespace assistant_agent {

// The outcome of a tool call, handed back to the model verbatim. `content` is
// the text (usually compact JSON) the model reads; `ok=false` marks a failure
// the model should see and react to (e.g. a rejected transform), NOT a plugin
// crash — errors are data here, never exceptions across the ABI.
struct ToolContext;

struct ToolResult {
  bool ok = true;
  std::string content;
  // GUI tick continuation; empty once the model-facing result is terminal.
  std::function<ToolResult(ToolContext&)> resume;

  static ToolResult success(std::string content) {
    return {true, std::move(content), {}};
  }
  static ToolResult deferred(std::function<ToolResult(ToolContext&)> continuation) {
    return {true, {}, std::move(continuation)};
  }
  static ToolResult failure(std::string content) {
    return {false, std::move(content), {}};
  }
};

// Host handles the executors reach through. Every executor runs on the GUI
// (host-callback) thread — the GuiExecutor guarantees that — so these views may
// be used directly. Obtained fresh per turn from the dialog's providers.
struct ToolContext {
  PJ::sdk::ToolboxHostView host;       // catalog snapshot + readSeries
  PJ::sdk::DataProcessorsHostView dp;  // createTransform / createMarkers / validateScript
  // Reads back what we just created — markers are objects, and entryCount() is
  // the only way to know how many were actually produced. Optional: an unbound
  // view means the answer carries no count, never that the tool fails.
  PJ::sdk::ToolboxObjectReadHostView objects;
  // App-control path. Optional on the same terms: an unbound view degrades to a
  // clean "host did not expose <service>" the model reads.
  PJ::sdk::PlaybackHostView playback;  // play/pause/seek/rate/state + ns->display-seconds
  // Tabs this assistant composed, and the only plots it may touch. The viewport
  // view is bounded to them by the host, so the two travel together.
  PJ::sdk::PlotTabHostView plot_tabs;
  PJ::sdk::ViewportHostView viewport;         // zoom, scoped to the tabs above
  std::function<void()> notify_data_changed;  // runtimeHost().notifyDataChanged() after a create
  std::string language = "luau";              // transform/marker script backend
};

}  // namespace assistant_agent
