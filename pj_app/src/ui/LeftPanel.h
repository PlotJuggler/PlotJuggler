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

// The "Sources" section (file, stream, cloud). Emits user-intent signals; MainWindow wires them to services.
class LeftPanel : public QWidget {
  Q_OBJECT
 public:
  explicit LeftPanel(QWidget* parent = nullptr);
  ~LeftPanel() override;

 signals:
  void loadDataRequested();
  void reloadDataRequested();
  // From the recent popup's Files and Layouts sections. In the browser recentLayoutSelected carries a
  // BrowserPersistence recipe id, not a path.
  void recentFileSelected(QString path);
  void recentLayoutSelected(QString path);
  void clearRecentLayoutsRequested();
  // A one-shot Start: the panel has no Stop.
  void streamingStartRequested();
  // Pause/resume of every stream session (StreamingSourceManager::onPauseToggled).
  void streamingPauseToggled(bool paused);
  // Record/Stop of every active stream. The panel never decides the button's state: MainWindow drives
  // RecordingService and reflects the outcome through setRecordingActive().
  void streamingRecordToggled(bool record);
  void streamingSourceChanged(QString source);
  // Seconds; emitted (and persisted) when the user finishes adjusting the scrubber, not on every drag tick.
  void streamingBufferChanged(int seconds);
  // `plugin_id` is the cloud-tagged toolbox's manifest `id`.
  void cloudToolboxRequested(QString plugin_id);

 public slots:
  void onStylesheetChanged(QString theme);
  void onChromeMetricsChanged(const ChromeMetrics& metrics);
  // Keeps the current selection when still present; on the first population, restores the persisted one.
  void setStreamingSources(const QStringList& names);
  void setReloadEnabled(bool enabled);
  void setRecentEnabled(bool enabled);

  // Shows or hides the Record button and its status label, from RecordingService::isSupported().
  void setRecordingSupported(bool supported);

  // Reflects RecordingService's state without emitting streamingRecordToggled.
  void setRecordingActive(bool active);
  // Elided to the label's width (re-elided on resize and font change), full text in the tooltip.
  void setRecordingStatusText(const QString& text);

  // <left_panel_state sources_tab streaming_source streaming_buffer/>, a persisted layout element. The panel's
  // visibility is in chrome_state, not here.
  [[nodiscard]] QDomElement saveSourcesState(QDomDocument& doc) const;

  // Applies each attribute on its own; missing or unknown values are ignored. Emits streamingSourceChanged and
  // streamingBufferChanged for what it applies, so the streaming manager follows the restored controls. Never writes
  // QSettings: a layout must not change the user's defaults.
  void restoreSourcesState(const QDomElement& element);

 public:
  // Lists the toolboxes whose manifest `tags` contain "cloud"; keeps the selection by plugin id; no entry disables
  // the row. Not a slot: RuntimeToolboxPlugin is non-copyable, so MOC cannot marshal it.
  void populateCloudToolboxes(const std::vector<RuntimeToolboxPlugin>& toolboxes);

 protected:
  // Re-elides the recording status on the label's resize and font changes.
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void applyIcons(QString theme);
  void applyPauseButtonState(QString theme);
  void applyRecordButtonState(const QString& theme);
  void elideRecordingStatus();
  // Reserves room for a typical status ("00:00 | 000.0 kB") in the label's
  // current font so an ordinary recording is readable even in a squeezed panel;
  // only the dropped-count suffix may still elide there.
  void applyRecordingStatusMinimumWidth();

  Ui::LeftPanel* ui_;
  // Un-elided; the label shows an elided view of it.
  QString recording_status_text_;
  ChromeMetrics chrome_metrics_;
};

}  // namespace PJ
