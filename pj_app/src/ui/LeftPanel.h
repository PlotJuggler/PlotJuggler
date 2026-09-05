#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QWidget>
#include <vector>

#include "pj_runtime/PluginRuntimeCatalog.h"
#include "pj_widgets/ChromeMetrics.h"

class QEvent;

namespace Ui {
class LeftPanel;
}

namespace PJ {

// "Sources" section: file load + streaming source. Emits high-level
// user-intent signals — MainWindow wires them to services.
class LeftPanel : public QWidget {
  Q_OBJECT
 public:
  explicit LeftPanel(QWidget* parent = nullptr);
  ~LeftPanel() override;

 signals:
  void loadDataRequested();
  void reloadDataRequested();
  // Emitted when the user picks a path from the recent popup in the Input
  // header. The popup has two sections — Layouts and Files — so picking an
  // entry emits one of two signals depending on its section. MainWindow wires
  // recentFileSelected to FileLoader::loadFile and recentLayoutSelected to
  // onLoadRecentLayout. On WASM the latter carries a bounded browser-recipe id
  // rather than a filesystem path.
  void recentFileSelected(QString path);
  void recentLayoutSelected(QString path);
  void clearRecentLayoutsRequested();
  // The cog button is a one-shot Start action — there is no "stop" affordance
  // in the UI (Davide: streaming should always be open). Emitted on click.
  void streamingStartRequested();
  // Pause/resume of the active stream session(s). When paused, samples keep
  // ingesting but the viewport no longer follows the live edge — mirrors PJ3
  // semantics. Wired into StreamingSourceManager::onPauseToggled.
  void streamingPauseToggled(bool paused);
  // Record/Stop for the session recording (all active streams). The button is
  // checkable, but the panel never decides the state: MainWindow drives
  // RecordingService and reflects the outcome through setRecordingActive().
  void streamingRecordToggled(bool record);
  void streamingSourceChanged(QString source);
  // Buffer length (seconds) for the streaming source. Persisted to
  // QSettings; emitted when the user adjusts the inline scrubber.
  void streamingBufferChanged(int seconds);
  // Emitted when the user clicks a cloud-tagged toolbox entry in the Cloud
  // page. `plugin_id` is the toolbox's manifest `id`; MainWindow wires this to
  // the launcher slot that binds a ToolboxRuntimeHost and presents the panel.
  void cloudToolboxRequested(QString plugin_id);

 public slots:
  void onStylesheetChanged(QString theme);
  // Rebinds Chrome metrics from MainWindow. Re-runs applyIcons() so
  // the Sources band, page rows, and streaming row absorb new icon
  // metrics, layout padding and the spacing between items.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);
  // Repopulates the streaming combo. Preserves the current selection if the
  // previously-selected name is still present.
  void setStreamingSources(const QStringList& names);
  void setReloadEnabled(bool enabled);
  void setRecentEnabled(bool enabled);

  // Shows or hides the Record button and its status label. The panel starts
  // hidden and the host opts in (RecordingService::isSupported()), so a build
  // with no recording sink never offers a control that cannot do anything.
  void setRecordingSupported(bool supported);

  // Reflects RecordingService state: checked, red icon and a visible status
  // label while recording.
  void setRecordingActive(bool active);
  // Shows `text` in the status label, elided to the label's current width with
  // the full string as the tooltip. Re-elided automatically when the label is
  // resized or its font changes.
  void setRecordingStatusText(const QString& text);

  // Builds <left_panel_state sources_tab="..." streaming_source="..."
  // streaming_buffer="..."/>. Caller appends to the layout document.
  // visibility is NOT included here — MainWindow handles it via chrome_state.
  [[nodiscard]] QDomElement saveSourcesState(QDomDocument& doc) const;

  // Applies <left_panel_state> attributes individually; missing or
  // mismatched values are silently ignored. Never writes to QSettings —
  // layout-driven UI changes don't mutate the global per-user defaults.
  void restoreSourcesState(const QDomElement& element);

 public:
  // Repopulates the Cloud combo from the catalog (mirrors setStreamingSources:
  // selection preserved by plugin id, an empty catalog disables the row). Not a
  // slot: RuntimeToolboxPlugin is non-copyable (owns a ToolboxLibrary), so MOC
  // can't marshal it. Filters to toolboxes whose manifest `tags` contains
  // "cloud". Safe to call repeatedly (e.g. on catalogChanged).
  void populateCloudToolboxes(const std::vector<RuntimeToolboxPlugin>& toolboxes);

 protected:
  // Re-elides the recording status on the label's resize and font changes.
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void applyIcons(QString theme);
  // Swaps the pause/resume icon and tooltip to match the button's checked
  // state. Called from applyIcons() and on every toggled() emission so the
  // glyph tracks both theme changes and user clicks.
  void applyPauseButtonState(QString theme);
  // Record icon and tooltip for the current checked state. Called from
  // applyIcons() and on every toggled() emission.
  void applyRecordButtonState(const QString& theme);
  // Elides recording_status_text_ to the label's current width.
  void elideRecordingStatus();
  // Reserves room for a typical status ("00:00 | 000.0 kB") in the label's
  // current font so an ordinary recording is readable even in a squeezed panel;
  // only the dropped-count suffix may still elide there.
  void applyRecordingStatusMinimumWidth();

  Ui::LeftPanel* ui_;
  // Full, un-elided recording status; the label shows an elided view of it.
  QString recording_status_text_;
  // Chrome metrics from MainWindow::chromeMetricsChanged.
  ChromeMetrics chrome_metrics_;
};

}  // namespace PJ
