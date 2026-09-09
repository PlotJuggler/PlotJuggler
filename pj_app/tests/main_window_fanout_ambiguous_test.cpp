// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// FIX 4: applyTimelineStateFromLayout's fan-out matcher must not GUESS when the
// saved fan-out shape no longer matches the loaded one. If MORE THAN ONE saved
// <dataset> child of a <fileInfo> shares a source_name, those children are
// indistinguishable by name and bind by source_index ONLY — and only when the
// same-named fan-out shape is preserved. When it is not (two saved "camera" tracks
// but only one "camera" candidate loaded), NO offset is applied to the survivor: a
// name match must never let it inherit an offset we cannot prove is its own.
//
// One MainWindow per binary (its dtor leaks process state), so this scenario lives
// in its own test target rather than a second TEST in main_window_source_layout_test.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <string>
#include <string_view>

#include "LayoutXml.h"
#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace PJ {

class MainWindowFanoutAmbiguousTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }
  static bool applyTimeline(MainWindow& window, const QList<layout_xml::DataSourceRef>& sources) {
    return window.applyTimelineStateFromLayout(sources);
  }
};

}  // namespace PJ

namespace {

// Creates a dataset on its own TimeDomain (required for a per-source offset) and
// writes a two-sample topic, mirroring FileLoader's one-domain-per-source.
PJ::DatasetId addDataset(PJ::AppSession& app, std::string_view source_name, std::string_view topic) {
  const PJ::DatasetId dataset = pj_test::createDataset(app, source_name, /*own_time_domain=*/true);
  if (dataset == 0 || pj_test::addScalarTopic(app, dataset, topic, /*first_ts=*/1'000, /*second_ts=*/2'000) == 0) {
    return 0;
  }
  return dataset;
}

// Two saved children named "camera" (+1s, +2s) load into a session where only ONE
// same-named candidate exists. The fan-out shape changed, so neither offset binds.
TEST(MainWindowFanoutAmbiguousTest, DuplicateSavedNameWithFewerCandidatesAppliesNoOffset) {
  QTemporaryDir extensions_dir;
  QTemporaryDir project_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  ASSERT_TRUE(project_dir.isValid());
  const QString source_path = project_dir.filePath(QStringLiteral("data/run.mcap"));
  // isSamePath compares canonical paths (empty for a non-existent file), so the
  // source file must exist on disk for the loaded "camera" dataset to be a candidate.
  // Without this the test would pass for the WRONG reason (no candidate at all).
  ASSERT_TRUE(QDir().mkpath(QFileInfo(source_path).absolutePath()));
  {
    QFile source_file(source_path);
    ASSERT_TRUE(source_file.open(QIODevice::WriteOnly));
  }

  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = PJ::MainWindowFanoutAmbiguousTestPeer::session(window);
  PJ::SessionManager& session = app.sessionManager();

  // Only ONE "camera" dataset loaded from run.mcap (the fan-out collapsed to one).
  const PJ::DatasetId camera = addDataset(app, "camera", "/image");
  ASSERT_NE(camera, 0U);
  app.catalogModel().rebuildFromDatastore();
  session.setDatasetSourcePath(camera, source_path);
  ASSERT_EQ(session.sourceDisplayOffset(camera).value.count(), 0);

  // Build a v4 ref whose <fileInfo> saved TWO "camera" children (a genuine fan-out
  // at save time) with distinct offsets and source_index 0/1.
  PJ::layout_xml::DataSourceRef ref;
  ref.resolved_path = source_path;
  PJ::layout_xml::DataSourceDatasetRef saved0;
  saved0.source_name = QStringLiteral("camera");
  saved0.source_index = 0;
  saved0.display_offset_ns = 1'000'000'000;  // +1 s
  saved0.has_display_offset = true;
  PJ::layout_xml::DataSourceDatasetRef saved1;
  saved1.source_name = QStringLiteral("camera");
  saved1.source_index = 1;
  saved1.display_offset_ns = 2'000'000'000;  // +2 s
  saved1.has_display_offset = true;
  ref.datasets = {saved0, saved1};

  const bool changed = PJ::MainWindowFanoutAmbiguousTestPeer::applyTimeline(window, {ref});

  // Neither saved offset may bind: a name match must not shift the lone survivor, and
  // source_index 1 is out of range. The dataset keeps its natural zero offset.
  EXPECT_FALSE(changed) << "an ambiguous same-named fan-out collapse must apply no offset";
  EXPECT_EQ(session.sourceDisplayOffset(camera).value.count(), 0)
      << "the surviving 'camera' must not inherit either saved offset";
}

}  // namespace
