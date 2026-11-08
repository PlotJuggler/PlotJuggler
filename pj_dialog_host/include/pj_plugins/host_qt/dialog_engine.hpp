#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <pj_widgets/ChromeMetrics.h>

#include <QPointer>
#include <QWidget>
#include <functional>
#include <memory>
#include <optional>
#include <pj_plugins/host/dialog_handle.hpp>
#include <string>

QT_BEGIN_NAMESPACE
class QDialog;
QT_END_NAMESPACE

namespace PJ {

/// Result of showing a dialog.
enum class DialogResult { kAccepted, kRejected };

/// Callback to resolve a parser's dialog vtable by encoding name.
/// Returns nullptr if no dialog is available for that encoding.
/// Used by DialogEngine to inject parser-specific options UI into data source dialogs.
using QueryParserDialogFn = std::function<const PJ_dialog_vtable_t*(const std::string& encoding)>;

/// A content-selected file staged at a path a legacy plugin can read. The host
/// retains `lease` until the dialog ends so backing_path cannot disappear while
/// the plugin is still using it.
struct DialogSelectedFile {
  std::string backing_path;
  std::shared_ptr<void> lease;
};
using DialogFileSelectionCompletion = std::function<void(std::optional<DialogSelectedFile>)>;
using DialogFileSelector = std::function<void(
    QWidget* parent, const std::string& name_filter, const std::string& title,
    DialogFileSelectionCompletion completion)>;

/// Configuration for DialogEngine.
struct DialogEngineConfig {
  int tick_interval_ms = 50;
  bool enable_diff = true;         // Only apply changed widgets on tick
  bool enable_file_picker = true;  // Show QFileDialog for file_picker actions

  /// Optional callback to resolve parser dialog vtables.
  /// When set and the loaded UI contains a widget named "pj_parser_slot",
  /// the engine will inject the parser's dialog widget into that slot
  /// whenever the encoding combo (comboBoxProtocol) changes.
  QueryParserDialogFn parser_dialog_provider;

  /// Optional asynchronous content picker for hosts that cannot return a real
  /// user filesystem path (notably browsers). Native builds leave this empty
  /// and retain the existing synchronous desktop QFileDialog behavior.
  DialogFileSelector file_selector;

  /// Initial parser config to restore when injecting the parser dialog.
  /// If non-empty, the parser dialog's loadConfig() is called with this.
  std::string initial_parser_config;

  /// If true, the dialog is shown non-modally (Qt::NonModal) so the parent
  /// window remains interactive. Required for drag-and-drop from the host UI
  /// into the dialog. Defaults to false (modal).
  bool non_modal = false;

  /// Put before the plugin dialog's own title (say, who asked for it). Sub-dialogs keep theirs.
  QString title_prefix{};

  /// Chrome metrics for sizing plugin SectionHeaderBand widgets to the app's
  /// canonical band height. A plugin .ui inflates its bands at the widget's
  /// default height, with no wiring to the host — so without this they render
  /// shorter than every other section band in the app. When set, the engine
  /// applies these metrics to every SectionHeaderBand in the loaded UI so the
  /// bands match the panel-hosted toolboxes. Unset -> bands keep their default
  /// height (headless runs and tests, which have no live app metrics).
  std::optional<ChromeMetrics> section_band_metrics;
};

/// Orchestrates the full dialog lifecycle for a plugin:
///   1. Load .ui via QUiLoader
///   2. Wrap in QDialog, wire QDialogButtonBox
///   3. Apply initial get_widget_data()
///   4. Wire signals -> on_widget_event
///   5. Start tick timer -> on_tick -> diff apply
///   6. ApplicationModal QDialog::show() (not open(), which would downgrade
///      the modality) and return to the application event loop
///   7. On finished, call on_accepted / on_rejected and the completion
class DialogEngine {
 public:
  using Completion = std::function<void(DialogResult)>;

  explicit DialogEngine(PJ::DialogHandle handle, DialogEngineConfig config = {});

  /// Build and open the plugin dialog without entering a nested event loop.
  /// The engine and the plugin context borrowed by its DialogHandle must
  /// outlive the callback. Completion runs on the GUI thread.
  void openDialog(QWidget* parent, Completion completion);

  /// Desktop compatibility facade. The dialog lifecycle itself is implemented
  /// by openDialog(); this wrapper waits in a nested loop only on native builds.
  /// Calling it on WebAssembly is rejected at runtime.
  [[nodiscard]] DialogResult showDialog(QWidget* parent = nullptr);

  /// Synchronously tear down the dialog opened by openDialog(), if one is still
  /// open: the plugin receives exactly one on_rejected, every widget/tick
  /// callback is severed, and the completion runs with kRejected — all before
  /// this returns. Callers use it to close a pending dialog while the plugin
  /// context borrowed by the engine's DialogHandle is still alive; afterwards no
  /// tick, widget event, or late completion can reach the plugin. No-op when no
  /// dialog is open (already completed, or openDialog was never called).
  void cancelActiveDialog();

  [[nodiscard]] bool isDialogOpen() const {
    return dialog_open_;
  }

  /// Run plugin headlessly (no UI): pump N ticks, return final widget_data JSON.
  [[nodiscard]] std::string runHeadless(int max_ticks);

  /// Return the plugin's current saved config.
  [[nodiscard]] std::string savedConfig() const;

  /// Return the parser dialog's saved config (empty if no parser dialog was shown).
  /// Only valid after showDialog() returns kAccepted.
  [[nodiscard]] std::string parserConfig() const;

  /// Stats from the last showDialog() call.
  struct Stats {
    int tick_count = 0;
    int event_count = 0;
    int diff_apply_count = 0;
    /// Re-reads whose payload was byte-identical to the previous one and were
    /// dropped before parsing (a gauge of plugin chattiness).
    int skipped_identical_count = 0;
    bool has_parser_slot = false;
    bool parser_dialog_injected = false;  // True if a parser dialog was actually injected
  };
  [[nodiscard]] Stats lastStats() const {
    return stats_;
  }

 private:
  struct AsyncRunner;

  PJ::DialogHandle handle_;
  DialogEngineConfig config_;
  Stats stats_;
  std::string parser_config_;  // Saved parser config (populated on accept)
  bool dialog_open_ = false;
  // The QDialog currently driven by openDialog(); cancelActiveDialog() rejects
  // through it. Cleared when the dialog completes.
  QPointer<QDialog> active_dialog_;
};

}  // namespace PJ
