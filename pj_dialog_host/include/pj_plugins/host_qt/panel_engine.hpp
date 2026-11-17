// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <memory>
#include <pj_plugins/host/dialog_handle.hpp>
#include <pj_plugins/host_qt/embedded_view_host.hpp>
#include <string>

// Forward declarations: callers pass a session and catalog without linking them.
namespace PJ {
class AppSession;
class CatalogModel;
}  // namespace PJ

namespace PJ {

/// Configuration for PanelEngine.
struct PanelEngineConfig {
  int tick_interval_ms = 50;
  bool enable_diff = true;

  /// Resolves a dragged catalog key ("dataset:1/topic:1/column:2") to a topic or series name ("topic/field")
  /// before the drop reaches the plugin's onItemsDropped: the curve tree drags opaque catalog keys, plugins expect
  /// names. Unset, or empty for a key: the key is delivered verbatim.
  std::function<std::string(const std::string& catalog_key)> catalog_key_resolver = {};

  /// Resolves a dragged catalog key to its data source id, delivered beside the name
  /// (DroppedItem::data_source_id); same-named series of two datasets differ only there.
  std::function<uint32_t(const std::string& catalog_key)> catalog_key_data_source = {};

  /// With both set, QFrame chart containers use a full PlotWidget (zoom, tracker, legend) instead of
  /// ChartPreviewWidget, with its right-click menu disabled.
  AppSession* session = nullptr;
  CatalogModel* catalog = nullptr;

  // Restarts the tick deadline after each UI event the plugin accepts: a debounce for panels whose expensive work
  // is in on_tick, and an explicit post-edit timer arm for browser event dispatch. A sustained event stream (a
  // slider drag) postpones on_tick for its whole duration: widget updates still apply per event, but the plugin's
  // periodic work waits for the first quiet tick interval. Members are appended after this one so callers'
  // positional initializers stay valid.
  bool restart_tick_timer_on_event = false;

  // Opens a file dialog for file, save and folder picker actions.
  bool enable_file_picker = true;

  /// Builds the views a `scene_view` frame embeds. Set: the engine announces PJ_DIALOG_HOST_EMBEDS_SCENE_VIEWS and
  /// delivers each refused request to the plugin as a `scene_view_error` event after the update that carried it.
  /// The panel and its interactive sub-panel embed views; a modal sub-dialog never does. Unset: such frames stay
  /// empty.
  EmbeddedViewHost embedded_views = {};
};

/// Hosts a long-lived interactive panel built from a plugin's typed-dialog UI. Same .ui loader, widget binding and
/// tick-and-diff as DialogEngine. Differences:
///   * openPanel() returns a bare QWidget* instead of running a dialog.
///   * No required QDialogButtonBox: the plugin draws its own button row.
///   * Close is plugin-initiated through WidgetData::requestClose("<reason>"); the callback set with
///     onCloseRequested() decides whether the panel is torn down.
///   * While the panel root is hidden (pinned into a non-current central tab) ticks run at 1/10 rate, so the
///     plugin's periodic logic keeps advancing while the UI poll and diff mostly pause; a catch-up tick fires
///     when the root is shown again.
///   * An interactive sub-panel (requestSubPanel) does not pause the ticks, which update it too, and is deleted
///     once dismissed.
///
/// Usage:
///   auto* engine = new PJ::PanelEngine(std::move(dialog_handle), {}, this);
///   engine->onCloseRequested([this](std::string reason) { restoreCentralArea(); return true; });
///   QWidget* widget = engine->openPanel();  // nullptr when the .ui does not load
///   mainWindow->presentPanel(widget);       // takes ownership; the engine keeps a weak reference
class PanelEngine : public QObject {
  Q_OBJECT
 public:
  explicit PanelEngine(DialogHandle handle, PanelEngineConfig config = {}, QObject* parent = nullptr);
  ~PanelEngine() override;

  PanelEngine(const PanelEngine&) = delete;
  PanelEngine& operator=(const PanelEngine&) = delete;
  PanelEngine(PanelEngine&&) = delete;
  PanelEngine& operator=(PanelEngine&&) = delete;

  /// Builds the widget from the plugin's .ui, applies the initial widget data, wires the signals and starts the
  /// tick timer. The caller parents the returned widget (non-owning) into its layout. The engine keeps a weak
  /// reference: deleting the widget stops further updates and is safe. nullptr when the .ui fails to load.
  [[nodiscard]] QWidget* openPanel();

  /// Stops the tick timer and calls the plugin's on_rejected. Idempotent.
  void close();

  /// Sets the callback fired when the plugin emits requestClose("<reason>") (the plugin's reason). It decides:
  /// true tears the engine down (stops ticking, rejects the plugin), false keeps the session running. Declining
  /// matters for a pinned tab that ignores "import_complete": it must keep exchanging widget data, or it stays on
  /// screen as an inert shell whose clicks are dropped. With no callback the engine closes.
  void onCloseRequested(std::function<bool(std::string /*reason*/)> cb);

  /// Counters of the current panel session, zeroed by openPanel().
  struct Stats {
    int tick_count = 0;
    int event_count = 0;
    int diff_apply_count = 0;
    /// Polls of a byte-identical payload, dropped before parsing (a gauge of plugin chattiness).
    int skipped_identical_count = 0;
  };
  [[nodiscard]] Stats stats() const;

 protected:
  /// Installed on the panel root. On a theme change (StyleChange, ApplicationPaletteChange) it re-applies the last
  /// full widget data: host-themed icons (setButtonIconNamed) are applied once and then diffed away, so they would
  /// not re-tint on their own.
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace PJ
