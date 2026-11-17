// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A layout's plot viewport is framed with the display offset its <fileInfo> timeline state applies, not the offset in
// effect while the plot XML was restored.
//
//   * Sync leg: restoreWorkspaceState frames each plot's saved absolute viewport with the current offset, then
//     restoreChromeAndPanels applies the layout's per-source offsets. The plot's per-dataset displayOffsetChanged
//     handler only replots, so applySavedViewportOrZoom is re-run over every plot when applyTimelineStateFromLayout
//     actually moved an offset.
//
//   * Async leg: a progressive reload has not yet registered the reloaded dataset's source path when
//     restoreChromeAndPanels runs, so the offsets are skipped. The refs are stashed and re-applied at drain via
//     applyPendingTimelineState(), before the viewport re-frame.
//
// MainWindow's destructor leaks process state, so both legs share one window inside a single TEST.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QRectF>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <memory>
#include <string>
#include <string_view>

#include "LayoutXml.h"
#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace PJ {

class MainWindowViewportReframeTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }
  // Restores the plots/curves from `doc` under kSilentDrop (the undo/redo policy); true when the restore applied.
  static bool restoreWorkspace(MainWindow& window, QDomDocument& doc) {
    return window.restoreWorkspaceState(doc, MainWindow::MissingCurvePolicy::kSilentDrop) ==
           MainWindow::RestoreResult::kApplied;
  }
  // The sync re-frame as restoreChromeAndPanels does it: apply the timeline offsets, then (if any moved) re-frame
  // every plot to its stashed viewport.
  static void applyTimelineAndReframe(MainWindow& window, const QList<layout_xml::DataSourceRef>& sources) {
    if (window.applyTimelineStateFromLayout(sources)) {
      window.forEachPlot([](PlotWidget* plot) { plot->applySavedViewportOrZoom(/*clear_after=*/true); });
    }
  }
  static void stashPendingTimeline(MainWindow& window, const QList<layout_xml::DataSourceRef>& sources) {
    window.pending_timeline_sources_ = sources;
  }
  static bool applyPending(MainWindow& window) {
    return window.applyPendingTimelineState();
  }
  static PlotWidget* firstPlot(MainWindow& window) {
    PlotWidget* found = nullptr;
    window.forEachPlot([&found](PlotWidget* plot) {
      if (found == nullptr) {
        found = plot;
      }
    });
    return found;
  }
};

}  // namespace PJ

namespace {

// Samples at a realistic epoch so the pre-apply and post-apply viewports are clearly different numbers.
constexpr PJ::Timestamp kT0Ns = 1'600'000'000'000'000'000;  // 1.6e9 s in ns
constexpr PJ::Timestamp kOneSecondNs = 1'000'000'000;
constexpr double kT0Sec = 1'600'000'000.0;

QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name); descriptor && descriptor->topic_id == topic_id) {
      return curve.name;
    }
  }
  return {};
}

// A layout document: a main tabbed_widget holding one plot on `curve_key` with an absolute <range> spanning
// [left_ns, right_ns], plus a <previouslyLoaded_Datafiles>/<fileInfo> for `source_path` carrying `offset_ns`.
QDomDocument buildLayoutDoc(
    const QString& curve_key, PJ::Timestamp left_ns, PJ::Timestamp right_ns, const QString& source_path,
    qint64 offset_ns) {
  QDomDocument doc;
  QDomElement root = doc.createElement(QStringLiteral("root"));
  root.setAttribute(QStringLiteral("pj4_version"), QStringLiteral("4"));
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
  QDomElement range = doc.createElement(QStringLiteral("range"));
  range.setAttribute(QStringLiteral("x_basis"), QStringLiteral("absolute"));
  range.setAttribute(QStringLiteral("left_ns"), QString::number(left_ns));
  range.setAttribute(QStringLiteral("right_ns"), QString::number(right_ns));
  range.setAttribute(QStringLiteral("left"), QString::number(static_cast<double>(left_ns) * 1.0e-9, 'f', 6));
  range.setAttribute(QStringLiteral("right"), QString::number(static_cast<double>(right_ns) * 1.0e-9, 'f', 6));
  range.setAttribute(QStringLiteral("top"), QStringLiteral("5.000000"));
  range.setAttribute(QStringLiteral("bottom"), QStringLiteral("-5.000000"));
  plot.appendChild(range);
  QDomElement curve = doc.createElement(QStringLiteral("curve"));
  curve.setAttribute(QStringLiteral("name"), curve_key);
  plot.appendChild(curve);

  dock_area.appendChild(plot);
  container.appendChild(dock_area);
  tab.appendChild(container);
  tabbed.appendChild(tab);
  root.appendChild(tabbed);

  QDomElement wrapper = doc.createElement(QStringLiteral("previouslyLoaded_Datafiles"));
  QDomElement file_info = doc.createElement(QStringLiteral("fileInfo"));
  file_info.setAttribute(QStringLiteral("filename"), source_path);
  QDomElement dataset = doc.createElement(QStringLiteral("dataset"));
  dataset.setAttribute(QStringLiteral("source_index"), QStringLiteral("0"));
  dataset.setAttribute(QStringLiteral("display_offset_ns"), QString::number(offset_ns));
  file_info.appendChild(dataset);
  wrapper.appendChild(file_info);
  root.appendChild(wrapper);
  return doc;
}

}  // namespace

// The async seam runs first (it only reads offsets), then the offset is reset and the sync re-frame is checked.
TEST(MainWindowViewportReframeTest, TimelineOffsetReframesPlotOnSyncAndDrainLegs) {
  QTemporaryDir extensions_dir;
  QTemporaryDir project_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  ASSERT_TRUE(project_dir.isValid());
  const QString source_path = project_dir.filePath(QStringLiteral("data/run.mcap"));
  // isSamePath compares canonical paths, empty for a non-existent file: the source path must exist on disk for the
  // timeline apply's candidate match.
  ASSERT_TRUE(QDir().mkpath(QFileInfo(source_path).absolutePath()));
  {
    QFile source_file(source_path);
    ASSERT_TRUE(source_file.open(QIODevice::WriteOnly));
  }

  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = PJ::MainWindowViewportReframeTestPeer::session(window);
  PJ::SessionManager& session = app.sessionManager();
  // "Use time offset" off: display == absolute, so a pre-apply viewport frames the raw epoch window and the -5 s
  // offset must shift it on apply.
  session.setUseTimeOffset(false);
  const PJ::DatasetId dataset_id = pj_test::createDataset(app, "run.mcap", /*own_time_domain=*/true);
  ASSERT_NE(dataset_id, 0U);
  const PJ::TopicId topic = pj_test::addScalarTopic(app, dataset_id, "/imu/accel", kT0Ns, kT0Ns + kOneSecondNs);
  ASSERT_NE(topic, 0U);
  app.catalogModel().rebuildFromDatastore();
  const QString curve_key = keyForTopic(app.catalogModel(), topic);
  ASSERT_FALSE(curve_key.isEmpty());
  session.setDatasetSourcePath(dataset_id, source_path);

  QDomDocument doc = buildLayoutDoc(
      curve_key, kT0Ns, kT0Ns + kOneSecondNs, source_path,
      /*offset_ns=*/-5 * kOneSecondNs);
  const QList<PJ::layout_xml::DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir(project_dir.path()));
  ASSERT_EQ(refs.size(), 1);

  // Async drain seam: nothing stashed yet -> a no-op.
  EXPECT_FALSE(PJ::MainWindowViewportReframeTestPeer::applyPending(window));
  EXPECT_EQ(session.sourceDisplayOffset(dataset_id).value.count(), 0);
  // Stash (as restoreChromeAndPanels does mid-progressive-load), then drain: the offset binds.
  PJ::MainWindowViewportReframeTestPeer::stashPendingTimeline(window, refs);
  EXPECT_TRUE(PJ::MainWindowViewportReframeTestPeer::applyPending(window));
  EXPECT_EQ(session.sourceDisplayOffset(dataset_id).value.count(), -5 * kOneSecondNs)
      << "the drain seam must apply the stashed source offset once the path is registered";
  // The stash is consumed: a second drain does nothing.
  EXPECT_FALSE(PJ::MainWindowViewportReframeTestPeer::applyPending(window));

  // Reset for the sync leg.
  session.setDisplayOffset(dataset_id, PJ::DisplayOffset{PJ::Duration{0}});
  ASSERT_EQ(session.sourceDisplayOffset(dataset_id).value.count(), 0);

  // Sync leg: restore the plot (offset 0 -> viewport framed at the raw absolute window).
  ASSERT_TRUE(PJ::MainWindowViewportReframeTestPeer::restoreWorkspace(window, doc));
  PJ::PlotWidget* plot = PJ::MainWindowViewportReframeTestPeer::firstPlot(window);
  ASSERT_NE(plot, nullptr) << "the layout must have created one plot";
  // Pre-apply: display == absolute (offset 0), so the axis frames [kT0, kT0+1] s.
  const QRectF before = plot->currentBoundingRect();
  EXPECT_NEAR(before.left(), kT0Sec, 1e-2);

  // Apply the -5 s offset and run the sync re-frame.
  PJ::MainWindowViewportReframeTestPeer::applyTimelineAndReframe(window, refs);

  // Post-apply: display = absolute - offset = absolute + 5 s. The axis frames the same absolute instant, now at
  // [kT0+5, kT0+6] s in display coordinates.
  const QRectF after = plot->currentBoundingRect();
  EXPECT_NEAR(after.left(), kT0Sec + 5.0, 5e-2) << "the viewport must re-frame to the offset-shifted absolute window";
  EXPECT_NEAR(after.right() - after.left(), 1.0, 5e-2) << "the window width (1 s) is preserved";
}
