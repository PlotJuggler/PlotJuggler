#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Shared by the browser-preset GUI tests (remote_preset_replay_test,
// streaming_preset_test): the MainWindow state a preset load leaves behind, and
// the modal boxes (a picker prompt or a warning) it may show.

#include <QApplication>
#include <QByteArray>
#include <QDomDocument>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <algorithm>
#include <optional>
#include <vector>

#include "FileLoader.h"
#include "MainWindow.h"
#include "SourceTimelineController.h"
#include "StreamingSourceManager.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/AppSession.h"

namespace PJ {

// Reaches the private MainWindow state the preset tests check.
class PresetTestPeer {
 public:
  using Workspace = MainWindow::CapturedWorkspace;

  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }
  [[nodiscard]] static FileLoader& fileLoader(MainWindow& window) {
    return *window.file_loader_;
  }
  [[nodiscard]] static StreamingSourceManager& streams(MainWindow& window) {
    return *window.streaming_manager_;
  }
  [[nodiscard]] static bool loadPending(const MainWindow& window) {
    return window.layout_selection_pending_;
  }
  [[nodiscard]] static Workspace capture(const MainWindow& window) {
    return window.captureWorkspace();
  }
  static void loadLayoutFile(MainWindow& window, const QString& path) {
    window.loadLayoutFromPath(path, MainWindow::LayoutLoadInteractivity::kAutomated);
  }
  // What File → Save Layout with Sources writes.
  [[nodiscard]] static std::optional<QDomDocument> sourceLayout(const MainWindow& window, QString& error) {
    return window.browserSourceLayoutDocument(error);
  }
  // The undo/redo snapshot of the live workspace.
  [[nodiscard]] static QByteArray liveState(const MainWindow& window) {
    return window.xmlSaveState().toByteArray(2);
  }
  static void recordDiscreteState(MainWindow& window) {
    window.onUndoableChange(/*force_new_state=*/true);
  }
  static void undo(MainWindow& window) {
    window.onUndo();
  }
  static void redo(MainWindow& window) {
    window.onRedo();
  }
  // The stream the playback transport follows (0 = none).
  [[nodiscard]] static DatasetId followedStream(const MainWindow& window) {
    return window.active_streaming_dataset_id_;
  }
  [[nodiscard]] static std::vector<DatasetId> timelineOrder(const MainWindow& window) {
    return window.source_timeline_controller_->currentTrackOrder();
  }
  // The plot whose saved id is `state_id`, or null.
  [[nodiscard]] static PlotWidget* plot(MainWindow& window, const QString& state_id) {
    PlotWidget* found = nullptr;
    window.forEachPlot([&found, &state_id](PlotWidget* plot) {
      if (plot->stateId() == state_id) {
        found = plot;
      }
    });
    return found;
  }
  [[nodiscard]] static int curveCount(MainWindow& window) {
    return static_cast<int>(curveKeys(window).size());
  }
  // The catalog keys of every plotted curve; distinct keys are distinct series.
  [[nodiscard]] static QStringList curveKeys(MainWindow& window) {
    QStringList keys;
    window.forEachPlot([&keys](PlotWidget* plot) {
      for (const auto& curve : plot->curveList()) {
        keys.push_back(curve.source_name);
      }
    });
    return keys;
  }
};

}  // namespace PJ

namespace pj_app_test {

// The visible top-level modal boxes (a picker prompt or a warning).
[[nodiscard]] inline int visibleDialogs() {
  const QWidgetList widgets = QApplication::topLevelWidgets();
  return static_cast<int>(std::count_if(widgets.cbegin(), widgets.cend(), [](const QWidget* widget) {
    return widget->isModal() && widget->isVisible();
  }));
}

inline void closeDialogs() {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (widget->isModal() && widget->isVisible()) {
      widget->close();
    }
  }
}

}  // namespace pj_app_test
