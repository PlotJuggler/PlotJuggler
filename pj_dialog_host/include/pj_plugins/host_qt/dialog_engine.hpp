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
#include <vector>

QT_BEGIN_NAMESPACE
class QDialog;
QT_END_NAMESPACE

namespace PJ {

/// Result of showing a dialog.
enum class DialogResult { kAccepted, kRejected };

/// Resolves a parser's dialog vtable by encoding name; nullptr when that encoding has no dialog. DialogEngine
/// injects the parser's options UI into data source dialogs with it.
using QueryParserDialogFn = std::function<const PJ_dialog_vtable_t*(const std::string& encoding)>;

/// A content-selected file staged at a path a plugin can read. The host holds `lease` until the dialog ends so
/// backing_path stays readable while the plugin uses it.
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
  bool enable_diff = true;         // apply only the widgets that changed on a tick
  bool enable_file_picker = true;  // show a file dialog for file_picker actions

  /// Resolves parser dialog vtables. With it set and a widget named "pj_parser_slot" in the loaded UI, the engine
  /// injects the parser's dialog widget into that slot whenever the encoding combo (comboBoxProtocol) changes.
  QueryParserDialogFn parser_dialog_provider;

  /// Asynchronous content picker for hosts that cannot return a real filesystem path (browsers). Native builds
  /// leave it empty and use the synchronous file dialog.
  DialogFileSelector file_selector;

  /// Config handed to the parser dialog's loadConfig() when it is injected; skipped when empty.
  std::string initial_parser_config;

  /// Shows the dialog non-modally so the parent window stays interactive (needed for drag-and-drop from the host
  /// UI into the dialog).
  bool non_modal = false;

  /// Put before the plugin dialog's own title (say, who asked for it). Sub-dialogs keep theirs.
  QString title_prefix{};

  /// Sizes the plugin's SectionHeaderBand widgets to the app's band height: a plugin .ui creates them at the
  /// widget's default height, which is shorter than every other band in the app. Unset (headless runs, tests: no
  /// live app metrics): bands keep the default height.
  std::optional<ChromeMetrics> section_band_metrics;
};

/// Runs a plugin dialog: loads its .ui, wraps it in the app's Dialog, applies the plugin's widget data, forwards
/// widget events to the plugin, and ticks the plugin to diff-apply its widget data until the dialog finishes (then
/// on_accepted / on_rejected and the completion). The dialog is application-modal through show(), not open(),
/// which would downgrade it to window-modal, and no nested event loop runs.
class DialogEngine {
 public:
  using Completion = std::function<void(DialogResult)>;

  explicit DialogEngine(PJ::DialogHandle handle, DialogEngineConfig config = {});

  /// Opens the plugin dialog and returns. The engine and the plugin context borrowed by its DialogHandle must
  /// outlive the completion, which runs on the GUI thread. A second call while a dialog is open completes at once
  /// with kRejected.
  void openDialog(QWidget* parent, Completion completion);

  /// Blocking wrapper over openDialog() that waits in a nested event loop; desktop only (kRejected, with a
  /// warning, on WebAssembly). Do not add new callers.
  [[nodiscard]] DialogResult showDialog(QWidget* parent = nullptr);

  /// Tears down the dialog opened by openDialog(), if still open, before returning: the plugin gets exactly one
  /// on_rejected, every widget and tick callback is severed, and the completion runs with kRejected. Call it while
  /// the plugin context borrowed by the DialogHandle is still alive; afterwards nothing can reach the plugin. No-op
  /// when no dialog is open.
  void cancelActiveDialog();

  [[nodiscard]] bool isDialogOpen() const {
    return dialog_open_;
  }

  /// Pumps `max_ticks` plugin ticks without a UI and returns the final widget_data JSON.
  [[nodiscard]] std::string runHeadless(int max_ticks);

  /// The plugin's current saved config.
  [[nodiscard]] std::string savedConfig() const;

  /// The parser dialog's saved config; empty when no parser dialog was shown. Valid after an accepted dialog.
  [[nodiscard]] std::string parserConfig() const;

  /// Hands over the leases of every file picked in an accepted dialog, including re-picks and picks in a replaced
  /// parser dialog (browser builds mount each pick read-only in MEMFS, backed by the picked File). A plugin that
  /// reads those paths after the dialog (a DBC opened during import) needs them until it is done: the caller keeps
  /// the returned owners that long. Empty after a rejected dialog and after the first call.
  [[nodiscard]] std::vector<std::shared_ptr<void>> takeSelectedFiles();

  /// Counters of the last dialog.
  struct Stats {
    int tick_count = 0;
    int event_count = 0;
    int diff_apply_count = 0;
    /// Re-reads of a byte-identical payload, dropped before parsing (a gauge of plugin chattiness).
    int skipped_identical_count = 0;
    bool has_parser_slot = false;
    bool parser_dialog_injected = false;
  };
  [[nodiscard]] Stats lastStats() const {
    return stats_;
  }

 private:
  struct AsyncRunner;

  PJ::DialogHandle handle_;
  DialogEngineConfig config_;
  Stats stats_;
  std::string parser_config_;                          // set on accept
  std::vector<std::shared_ptr<void>> selected_files_;  // staged picks of the accepted dialog
  bool dialog_open_ = false;
  // Cleared when the dialog completes; cancelActiveDialog() rejects through it.
  QPointer<QDialog> active_dialog_;
};

}  // namespace PJ
