// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <span>
#include <string>
#include <utility>

namespace PJ::agent_tools {

struct ToolContext;

// Consecutive samples of a series, stamped `timestamps`: `values` holds each one's value (NaN at a null), or the single
// value all of them hold. The spans are valid only during the call.
using ValueBlockSink = std::function<void(std::span<const std::int64_t> timestamps, std::span<const double> values)>;

// The outcome of a tool call, handed back to the model verbatim. `content` is
// the text (usually compact JSON) the model reads; `ok=false` marks a failure
// the model should see and react to (e.g. a rejected transform): errors are
// data here, never exceptions.
struct ToolResult {
  bool ok = true;
  std::string content;
  // GUI tick continuation; empty once the model-facing result is terminal.
  // Neither it nor an executor may spin a nested event loop: the caller relies on
  // that to keep the services a running call reaches alive until it returns (only a
  // later event can destroy them), and destroying a continuation may release into them.
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

// Host handles the executors reach through. The caller runs every executor on the
// GUI thread, where these views may be used directly, and builds the context fresh
// for each call (views can be invalidated between event-loop ticks). A result with
// `resume` set is not terminal: the caller invokes `resume` again on later ticks,
// with a fresh context, until it returns a terminal result.
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
  // Tabs this agent composed, and the only plots it may touch. The viewport
  // view is bounded to them by the host, so the two travel together.
  PJ::sdk::PlotTabHostView plot_tabs;
  PJ::sdk::ViewportHostView viewport;  // zoom, scoped to the tabs above
  // Zooms ONE of the caller's tabs, `tab`, to `range` (display-axis seconds), or fits it to its data when `range`
  // is empty. Optional: `viewport` can only frame every tab the caller owns at once (its SDK contract), so a host
  // that can address one tab sets this, and plot_tab's zoom uses it instead.
  std::function<PJ::Status(const std::string& tab, std::optional<std::pair<double, double>> range)> zoom_tab;
  // Reads `field` limited to raw ns [first, last] plus the sample before `first` (its value holds at the window's
  // start), decoding only the chunks the window touches. Optional: unset, a windowed tool reads the whole series
  // through `host` and cuts it, with the same result.
  // ponytail: a host hook because the toolbox ABI's read_series_arrow has no window; a windowed tail slot on
  // PJ_toolbox_host_vtable_t (ToolboxHostView) would serve every plugin and retire this hook.
  std::function<PJ::Expected<PJ::sdk::MaterializedSeriesView>(
      PJ::sdk::FieldHandle field, std::int64_t first, std::int64_t last)>
      read_window;
  // Hands `on_block` the whole numeric series of `field`, in time order, as blocks (ValueBlockSink), so a windowed read
  // gets its whole-series flat facts without a full read (FlatRunAccumulator::addBlock). Optional, like read_window;
  // a windowed read_series uses the two only together.
  std::function<PJ::Status(PJ::sdk::FieldHandle field, const ValueBlockSink& on_block)> scan_values;
  std::function<void()> notify_data_changed;  // runtimeHost().notifyDataChanged() after a create
};

}  // namespace PJ::agent_tools
