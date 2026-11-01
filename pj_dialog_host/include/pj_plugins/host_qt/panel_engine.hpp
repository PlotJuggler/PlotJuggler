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

// Forward declarations so callers can pass session/catalog without a hard link.
namespace PJ {
class AppSession;
class CatalogModel;
}  // namespace PJ

namespace PJ {

/// Configuration for PanelEngine.
struct PanelEngineConfig {
  int tick_interval_ms = 50;
  bool enable_diff = true;

  /// Optional resolver from a dragged catalog key (e.g.
  /// "dataset:1/topic:1/column:2") to a topic or series name (e.g. "topic/field")
  /// before the drop is delivered to the plugin's onItemsDropped. The PJ4 curve
  /// tree drags opaque catalog keys; plugins (Quaternion, FFT, …) expect names.
  /// If unset, or if it returns empty for a key, that key is delivered verbatim.
  std::function<std::string(const std::string& catalog_key)> catalog_key_resolver = {};

  /// Optional resolver from a dragged catalog key to its data source id, delivered beside the name
  /// (DroppedItem::data_source_id). Same-named series of two datasets differ only here.
  std::function<uint32_t(const std::string& catalog_key)> catalog_key_data_source = {};

  /// Optional session + catalog. When both are non-null, QFrame chart containers
  /// use a full PlotWidget (zoom/tracker/legend) instead of ChartPreviewWidget,
  /// matching the FilterEditorPanel preview quality. Right-click menu is disabled.
  AppSession* session = nullptr;
  CatalogModel* catalog = nullptr;

  // Restart the periodic tick deadline after each UI event the plugin accepts.
  // This is a debounce for panels whose expensive work happens in on_tick, and
  // also gives browser event dispatch an explicit post-edit timer arm. Off by
  // default so existing native panel cadence is unchanged; declared last so
  // positional initializers predating it stay valid. Trade-off for opting in: a
  // sustained event stream (e.g. a continuous slider drag) postpones on_tick for
  // its whole duration — event-driven widget updates still apply per event, but
  // plugin-internal periodic work waits for the first quiet tick interval.
  bool restart_tick_timer_on_event = false;

  // Open a QFileDialog for file/save/folder picker actions. Appended after
  // restart_tick_timer_on_event for the same reason that member is late in the
  // struct: positional initializers predating it stay valid.
  bool enable_file_picker = true;

  /// Builds the views a `scene_view` frame embeds. Set: the engine announces
  /// PJ_DIALOG_HOST_EMBEDS_SCENE_VIEWS and delivers each refused request to the
  /// plugin as a `scene_view_error` event after the update that carried it.
  /// The panel and its interactive sub-panel embed views; a modal sub-dialog
  /// never does. Unset: such frames stay empty. Last for the reason above.
  EmbeddedViewHost embedded_views = {};
};

/// Hosts a long-lived interactive panel built from a plugin's typed-dialog UI.
///
/// Sibling of DialogEngine. Same .ui loader, same widget binding, same
/// tick-and-diff mechanism. Differences:
///   * Returns a bare QWidget* via openPanel() instead of running QDialog::exec().
///   * No required QDialogButtonBox — the plugin draws its own button row.
///   * Close is plugin-initiated via WidgetData::requestClose("<reason>");
///     the engine forwards the reason via the callback set with
///     onCloseRequested(); that callback decides whether the panel is torn down.
///   * While the panel root is hidden (e.g. pinned into a non-current
///     central tab) ticks are throttled to 1/10 rate — the plugin's periodic
///     logic keeps advancing, the UI poll+diff mostly pauses — and a
///     catch-up tick fires the moment the root is shown again.
///   * An interactive sub-panel (requestSubPanel) does not pause the ticks,
///     which update it too, and is deleted once dismissed.
///
/// Typical usage from pj_app:
///   auto* engine = new PJ::PanelEngine(std::move(dialog_handle), {}, this);
///   engine->onCloseRequested([this](std::string reason) { restoreCentralArea(); return true; });
///   QWidget* widget = engine->openPanel();
///   if (widget == nullptr) { ... handle error ... }
///   mainWindow->presentPanel(widget);  // takes ownership; engine keeps a weak ref
class PanelEngine : public QObject {
  Q_OBJECT
 public:
  explicit PanelEngine(DialogHandle handle, PanelEngineConfig config = {}, QObject* parent = nullptr);
  ~PanelEngine() override;

  PanelEngine(const PanelEngine&) = delete;
  PanelEngine& operator=(const PanelEngine&) = delete;
  PanelEngine(PanelEngine&&) = delete;
  PanelEngine& operator=(PanelEngine&&) = delete;

  /// Build the QWidget from the plugin's .ui blob, apply initial widget data,
  /// wire widget signals, start the tick timer. Returns a non-owning pointer
  /// to the constructed widget — the caller parents it into its target layout
  /// (typically MainWindow's central area). PanelEngine retains the widget
  /// reference for tick/event delivery; deletion of the widget (e.g. by
  /// re-parenting and dropping) does not crash the engine but stops further
  /// updates.
  ///
  /// Returns nullptr on .ui load failure.
  [[nodiscard]] QWidget* openPanel();

  /// Stop the tick timer and call the plugin's on_rejected. Safe to call
  /// multiple times; idempotent.
  void close();

  /// Set the callback fired when the plugin emits requestClose("<reason>").
  /// The string carries the plugin-provided reason.
  ///
  /// The callback OWNS the decision: return true to let PanelEngine tear itself
  /// down (stop ticking, reject the plugin), false to keep the session running.
  /// Declining matters for a panel the host keeps alive across its own batch
  /// completion — a pinned tab that ignores "import_complete" must keep
  /// exchanging widget data, or it is left on screen as an inert shell whose
  /// every click is dropped. With no callback installed the engine closes, which
  /// is the takeover default.
  void onCloseRequested(std::function<bool(std::string /*reason*/)> cb);

  /// Statistics from the current panel session (zeroed on each openPanel).
  struct Stats {
    int tick_count = 0;
    int event_count = 0;
    int diff_apply_count = 0;
    /// Polls whose payload was byte-identical to the previous one and were
    /// dropped before parsing (a gauge of plugin chattiness).
    int skipped_identical_count = 0;
  };
  [[nodiscard]] Stats stats() const;

 protected:
  /// Installed on the panel root. On an app theme change (StyleChange /
  /// ApplicationPaletteChange) it re-applies the panel's last full widget-data,
  /// which re-themes host-themed icons (setButtonIconNamed → loadSvg) for the new
  /// theme. Those icons are applied once and then diffed away, so they would not
  /// re-tint on their own.
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace PJ
