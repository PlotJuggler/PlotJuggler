#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Shared scaffolding for the MainWindow layout-import GUI tests
// (alive / policy / cancel / lifecycle / binder): the friend test peer, the
// source-bound layout-file builder, the diagnostic recorder, and the common
// main() macro. The descriptor-scripted fake provider itself is the shared
// tests/support/fake_import_provider.h. MainWindow teardown leaks process
// state, so runner cases use separate ctest processes; lifecycle scenarios
// retain standalone executables and use the main() macro below.

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QObject>
#include <QProgressBar>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QToolButton>
#include <functional>
#include <utility>

#include "LayoutImportBatch.h"
#include "LayoutXml.h"
#include "MainWindow.h"
#include "PendingDisplayBinder.h"
#include "fake_import_provider.h"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_widgets/IngestProgressWidget.h"

namespace PJ {

// Reaches the private restore surfaces of MainWindow for the layout-import
// GUI scenarios: the explicit-interactivity load entry, the progressive
// in-flight flag, the batch handle, and the pending binder state.
class MainWindowLayoutImportTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }
  static void loadLayout(MainWindow& window, const QString& path, bool interactive) {
    window.loadLayoutFromPath(
        path, interactive ? MainWindow::LayoutLoadInteractivity::kInteractive
                          : MainWindow::LayoutLoadInteractivity::kAutomated);
  }
  [[nodiscard]] static bool progressiveInFlight(const MainWindow& window) {
    return window.progressive_layout_in_flight_;
  }
  [[nodiscard]] static LayoutImportBatch* batch(MainWindow& window) {
    return window.layout_import_batch_.get();
  }
  [[nodiscard]] static bool batchActive(const MainWindow& window) {
    return window.layoutImportBatchActive();
  }
  // True when the pending binder holds NO retained intents.
  [[nodiscard]] static bool binderEmpty(MainWindow& window) {
    return window.pending_binder_ == nullptr || window.pending_binder_->empty();
  }
  [[nodiscard]] static int plotCount(MainWindow& window) {
    int count = 0;
    window.forEachPlot([&count](PlotWidget* /*plot*/) { ++count; });
    return count;
  }
  [[nodiscard]] static PlotWidget* firstPlot(MainWindow& window) {
    PlotWidget* found = nullptr;
    window.forEachPlot([&found](PlotWidget* plot) {
      if (found == nullptr) {
        found = plot;
      }
    });
    return found;
  }
  [[nodiscard]] static QtDiagnosticBridge* diagnosticBridge(MainWindow& window) {
    return window.diagnostic_bridge_;
  }
  [[nodiscard]] static int restoreWaiterCount(const MainWindow& window) {
    return static_cast<int>(window.restore_waiters_.size());
  }
  [[nodiscard]] static int totalCurveCount(MainWindow& window) {
    int count = 0;
    window.forEachPlot([&count](PlotWidget* plot) { count += static_cast<int>(plot->curveList().size()); });
    return count;
  }

  // --- T7 binder surfaces: strip displayed ownership + batch observation ---

  using StripOwnerKind = MainWindow::IngestStripOwnerKind;
  [[nodiscard]] static StripOwnerKind stripOwnerKind(const MainWindow& window) {
    return window.ingest_strip_owner_kind_;
  }
  [[nodiscard]] static DatasetId stripOwnerDataset(const MainWindow& window) {
    return window.ingest_strip_owner_dataset_;
  }
  // "Engaged" = shown, or the anti-flash show delay is pending. These binaries
  // never show() the window, so the widget's isVisible() is always false; the
  // explicit hidden flag plus the timer is the observable pair.
  [[nodiscard]] static bool stripEngaged(const MainWindow& window) {
    return window.ingest_show_timer_->isActive() || !window.ingest_progress_->isHidden();
  }
  // The strip's caption rides the bar's format string (IngestProgressWidget
  // has no title getter, deliberately); ingest owners never set counter text,
  // so the format IS the displayed title here.
  [[nodiscard]] static QString stripTitle(const MainWindow& window) {
    const auto* bar = stripBar(window);
    return bar == nullptr ? QString() : bar->format();
  }
  [[nodiscard]] static int stripProgressMaximum(const MainWindow& window) {
    const auto* bar = stripBar(window);
    return bar == nullptr ? -1 : bar->maximum();
  }
  [[nodiscard]] static int stripProgressValue(const MainWindow& window) {
    const auto* bar = stripBar(window);
    return bar == nullptr ? -1 : bar->value();
  }
  [[nodiscard]] static int batchObserverCount(const MainWindow& window) {
    return static_cast<int>(window.layout_batch_ingest_conns_.size());
  }
  // The real GUI stop control (never the routed slot directly).
  [[nodiscard]] static bool clickStripStop(MainWindow& window) {
    auto* stop = window.ingest_progress_->findChild<QToolButton*>(QStringLiteral("ingestPrimaryButton"));
    if (stop == nullptr) {
      return false;
    }
    stop->click();
    return true;
  }
  // Test-only hygiene: a driven endIngest with no batch active reaches no
  // production handler (real ends route through the interactive host
  // callbacks or the batch observers), so a scenario's last driven end can
  // leave a stale displayed owner behind — and a show timer that fired during
  // pumping leaves the widget active (or a timer still pending) into the next
  // scenario. Reset the whole strip surface — owner fields, both timers, the
  // widget's active state — in TearDown so the scenarios stay
  // order-independent. Unconditional-safe there: no active ingests remain.
  static void resetStripState(MainWindow& window) {
    window.ingest_strip_owner_kind_ = MainWindow::IngestStripOwnerKind::kNone;
    window.ingest_strip_owner_dataset_ = 0;
    window.ingest_show_timer_->stop();
    window.ingest_hide_timer_->stop();
    window.ingest_progress_->setActive(false);
  }

  // --- Stage-5 live-E2E surfaces: the literal GUI menu actions (E6 automates
  // the real QActions, never the slots directly), the dataset-removal shell
  // route (the warm leg must unload first — findAlreadyLoaded resolves by
  // provenance and would short-circuit the reload), and the direct save entry
  // for re-save assertions that already proved the modal once. ---

  [[nodiscard]] static QAction* saveLayoutAction(MainWindow& window) {
    return window.action_save_layout_;
  }
  [[nodiscard]] static QAction* loadLayoutAction(MainWindow& window) {
    return window.action_load_layout_;
  }
  static void removeDatasetData(MainWindow& window, DatasetId dataset_id) {
    window.removeDatasetData(dataset_id);
  }
  static void saveLayoutToPath(MainWindow& window, const QString& path, bool include_data_source) {
    window.saveLayoutToPath(path, include_data_source);
  }

  // --- D9 scaffolding: the takeover-fold and pinned-panel seams (the same
  // private surfaces ToolboxPanelFoldTest drives, re-exposed here so the
  // binder suite can prove a headless batch job never reaches them). ---

  [[nodiscard]] static bool presentPanel(MainWindow& window, QWidget* panel) {
    return window.presentPanel(panel);
  }
  // Presents `container` as the chart-area takeover through a bare presenter
  // (the fold's only target).
  [[nodiscard]] static bool dockBare(
      MainWindow& window, QWidget* container, const QString& plugin_id, const QString& title,
      ToolboxRuntimeHost* host) {
    return window
        .registerToolbox(
            plugin_id,
            ToolboxPresenter::Spec{
                .container = container,
                .engine = nullptr,
                .session = {},
                .host = host,
                .title = title,
                .save_config = []() { return QStringLiteral("{}"); },
                .apply_chrome = {}})
        .moveToDock();
  }
  [[nodiscard]] static bool isTakeover(const MainWindow& window, const QString& plugin_id) {
    const ToolboxPresenter* presenter = window.findToolbox(plugin_id);
    return presenter != nullptr && presenter->presentation() == ToolboxPresenter::Presentation::kTakeover;
  }
  [[nodiscard]] static bool takeoverPanelPresent(const MainWindow& window) {
    return window.current_panel_ != nullptr;
  }
  static QWidget* releaseCentralPanel(MainWindow& window) {
    return window.releaseCentralPanel();
  }
  static void moveToTab(
      MainWindow& window, QWidget* container, const QString& plugin_id, const QString& title,
      ToolboxRuntimeHost* host) {
    window
        .registerToolbox(
            plugin_id,
            ToolboxPresenter::Spec{
                .container = container,
                .engine = nullptr,
                .session = {},
                .host = host,
                .title = title,
                .save_config = []() { return QStringLiteral("{}"); },
                .apply_chrome = {}})
        .moveToTab(/*transient=*/false);
  }
  [[nodiscard]] static bool isPinned(const MainWindow& window, const QString& plugin_id) {
    const ToolboxPresenter* presenter = window.findToolbox(plugin_id);
    return presenter != nullptr && presenter->presentation() == ToolboxPresenter::Presentation::kTab;
  }
  static void teardownAllToolboxes(MainWindow& window) {
    window.teardownAllToolboxes();
  }
  static void setHostSeams(
      MainWindow& window, std::function<bool(QString)> confirm, std::function<bool(ToolboxRuntimeHost*)> busy,
      std::function<void(ToolboxRuntimeHost*)> stop) {
    window.confirm_running_job_ = std::move(confirm);
    window.host_work_in_flight_ = std::move(busy);
    window.stop_host_work_ = std::move(stop);
  }

 private:
  [[nodiscard]] static const QProgressBar* stripBar(const MainWindow& window) {
    return window.ingest_progress_->findChild<QProgressBar*>(QStringLiteral("ingestProgressBar"));
  }
};

}  // namespace PJ

namespace pj_layout_import_gui {

// The shared fake provider, re-exported so the GUI tests keep addressing it
// through this namespace.
using pj_fake_import::g_instance;
using pj_fake_import::kFakeVtable;
using pj_fake_import::kProviderId;

// GUI-scenario descriptor: name/estimate are irrelevant here.
[[nodiscard]] inline QString makeDescriptor(const QString& trust, const QString& path, const QString& import_mode) {
  return pj_fake_import::makeDescriptor(QStringLiteral("gui"), trust, path, 0, import_mode);
}

// One plot with a topic-addressed (unresolved) curve plus one
// materialize-bearing <fileInfo> whose descriptor scripts the fake.
// `provider` overrides the <materialize provider> attribute (an unknown id
// drives the §6.4 provider-absent fallback).
[[nodiscard]] inline QDomDocument buildImportLayoutDoc(
    const QString& curve_topic, const QString& source_path, const QString& descriptor,
    const QString& provider = QString::fromUtf8(kProviderId)) {
  QDomDocument doc;
  QDomElement root = doc.createElement(QStringLiteral("root"));
  root.setAttribute(QStringLiteral("pj4_version"), QStringLiteral("4"));
  root.setAttribute(QStringLiteral("binding"), QStringLiteral("source"));
  doc.appendChild(root);

  QDomElement tabbed = doc.createElement(QStringLiteral("tabbed_widget"));
  tabbed.setAttribute(QStringLiteral("parent"), QStringLiteral("main_window"));
  QDomElement tab = doc.createElement(QStringLiteral("Tab"));
  tab.setAttribute(QStringLiteral("id"), QStringLiteral("t1"));
  tab.setAttribute(QStringLiteral("containers"), QStringLiteral("1"));
  QDomElement container = doc.createElement(QStringLiteral("Container"));
  QDomElement dock_area = doc.createElement(QStringLiteral("DockArea"));
  dock_area.setAttribute(QStringLiteral("id"), QStringLiteral("a1"));
  dock_area.setAttribute(QStringLiteral("name"), QStringLiteral("View"));
  QDomElement plot = doc.createElement(QStringLiteral("plot"));
  plot.setAttribute(QStringLiteral("id"), QStringLiteral("plot1"));
  plot.setAttribute(QStringLiteral("mode"), QStringLiteral("TimeSeries"));
  QDomElement curve = doc.createElement(QStringLiteral("curve"));
  // Stable topic identity only — no concrete key: the curve resolves (or is
  // retained as a pending intent) against whatever the import produces.
  curve.setAttribute(QStringLiteral("topic"), curve_topic);
  plot.appendChild(curve);
  dock_area.appendChild(plot);
  container.appendChild(dock_area);
  tab.appendChild(container);
  tabbed.appendChild(tab);
  root.appendChild(tabbed);

  QDomElement wrapper = doc.createElement(QStringLiteral("previouslyLoaded_Datafiles"));
  QDomElement file_info = doc.createElement(QStringLiteral("fileInfo"));
  file_info.setAttribute(QStringLiteral("filename"), source_path);
  QDomElement materialize = doc.createElement(QStringLiteral("materialize"));
  materialize.setAttribute(QStringLiteral("provider"), provider);
  materialize.setAttribute(QStringLiteral("identity"), QStringLiteral("id-1"));
  PJ::layout_xml::appendJsonAsCdata(doc, materialize, descriptor);
  file_info.appendChild(materialize);
  wrapper.appendChild(file_info);
  root.appendChild(wrapper);
  return doc;
}

// A generic-binding layout with one curve on `curve_topic`: no data sources,
// no <materialize> — the plain SYNC-apply shape (binds against loaded data).
[[nodiscard]] inline QDomDocument buildGenericCurveLayoutDoc(const QString& curve_topic) {
  QDomDocument doc;
  QDomElement root = doc.createElement(QStringLiteral("root"));
  root.setAttribute(QStringLiteral("pj4_version"), QStringLiteral("4"));
  root.setAttribute(QStringLiteral("binding"), QStringLiteral("generic"));
  doc.appendChild(root);
  QDomElement tabbed = doc.createElement(QStringLiteral("tabbed_widget"));
  tabbed.setAttribute(QStringLiteral("parent"), QStringLiteral("main_window"));
  QDomElement tab = doc.createElement(QStringLiteral("Tab"));
  tab.setAttribute(QStringLiteral("id"), QStringLiteral("t1"));
  tab.setAttribute(QStringLiteral("containers"), QStringLiteral("1"));
  QDomElement container = doc.createElement(QStringLiteral("Container"));
  QDomElement dock_area = doc.createElement(QStringLiteral("DockArea"));
  dock_area.setAttribute(QStringLiteral("id"), QStringLiteral("a1"));
  dock_area.setAttribute(QStringLiteral("name"), QStringLiteral("View"));
  QDomElement plot = doc.createElement(QStringLiteral("plot"));
  plot.setAttribute(QStringLiteral("id"), QStringLiteral("plot1"));
  plot.setAttribute(QStringLiteral("mode"), QStringLiteral("TimeSeries"));
  QDomElement curve = doc.createElement(QStringLiteral("curve"));
  curve.setAttribute(QStringLiteral("topic"), curve_topic);
  plot.appendChild(curve);
  dock_area.appendChild(plot);
  container.appendChild(dock_area);
  tab.appendChild(container);
  tabbed.appendChild(tab);
  root.appendChild(tabbed);
  return doc;
}

[[nodiscard]] inline bool writeLayoutFile(const QString& path, const QDomDocument& doc) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    return false;
  }
  file.write(doc.toByteArray(2));
  return true;
}

// The buildImportLayoutDoc + writeLayoutFile nest every scenario starts with.
[[nodiscard]] inline bool writeImportLayout(
    const QString& layout_path, const QString& curve_topic, const QString& source_path, const QString& descriptor,
    const QString& provider = QString::fromUtf8(kProviderId)) {
  return writeLayoutFile(layout_path, buildImportLayoutDoc(curve_topic, source_path, descriptor, provider));
}

// Records every diagnostic id the window reports into `ids` (delivery is
// queued — pump before asserting).
inline void recordDiagnosticIds(PJ::MainWindow& window, QStringList& ids) {
  QObject::connect(
      PJ::MainWindowLayoutImportTestPeer::diagnosticBridge(window), &PJ::QtDiagnosticBridge::diagnosticReported,
      &window, [&ids](int /*level*/, const QString& /*source*/, const QString& id, const QString& /*message*/) {
        ids.push_back(id);
      });
}

}  // namespace pj_layout_import_gui

// The common GUI-test main(): offscreen platform unless overridden,
// test-mode QStandardPaths, one QApplication.
#define PJ_MAIN_WINDOW_TEST_MAIN                         \
  int main(int argc, char** argv) {                      \
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) { \
      qputenv("QT_QPA_PLATFORM", "offscreen");           \
    }                                                    \
    QStandardPaths::setTestModeEnabled(true);            \
    QApplication app(argc, argv);                        \
    ::testing::InitGoogleTest(&argc, argv);              \
    return RUN_ALL_TESTS();                              \
  }
