// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <memory>
#include <string>
#include <vector>

#include "MainWindow.h"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_runtime/CatalogModel.h"
#include "support/loader_test_support.h"
#include "support/preset_test_support.h"

using namespace Qt::StringLiterals;

namespace {

using PJ::PresetTestPeer;
using pj_app_test::pumpUntil;

// Every ctest case owns one shell and an isolated, real mock DataSource DSO.
class MainWindowStartupTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_.isValid());
    ASSERT_TRUE(data_.isValid());
    const QString plugin = QString::fromUtf8(PJ_MOCK_FILE_SOURCE_PLUGIN_PATH);
    ASSERT_TRUE(QFile::copy(plugin, extensions_.filePath(QFileInfo(plugin).fileName())));
    window_ = std::make_unique<PJ::MainWindow>(extensions_.path());
    QObject::connect(window_.get(), &PJ::MainWindow::layoutRestoreSettled, window_.get(), [this](bool ok) {
      settles_.push_back(ok);
    });
  }

  QString layout(const QByteArray& body) {
    const QString path = data_.filePath(QStringLiteral("startup.pj4.xml"));
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write(body), body.size());
    return path;
  }

  // A layout comparing two runs: plot_run1 ("Run 1") plots mock/file_data from `first`, plot_run2 from `second`.
  QString twoRunLayout(const QString& first, const QString& second) {
    const auto curve = [](const QString& file, const QString& label) {
      return u"<curve topic=\"mock/file_data\" field=\"value\" dataset_source=\"%1\" dataset_path=\"%2\"/>"_s.arg(
          label, file);
    };
    return layout(
        (u"<root pj4_version=\"4\" binding=\"source\"><tabbed_widget parent=\"main_window\">"
         u"<Tab id=\"t1\" containers=\"1\"><Container><DockSplitter orientation=\"-\" count=\"2\" sizes=\"0.5;0.5\">"
         u"<DockArea id=\"a1\" name=\"Run 1\"><plot id=\"plot_run1\" mode=\"TimeSeries\">"_s +
         curve(first, QFileInfo(first).fileName()) +
         u"</plot></DockArea><DockArea id=\"a2\" name=\"Run 2\"><plot id=\"plot_run2\" mode=\"TimeSeries\">"_s +
         curve(second, QFileInfo(second).fileName()) +
         u"</plot></DockArea></DockSplitter></Container></Tab></tabbed_widget></root>"_s)
            .toUtf8());
  }

  // The source files of the datasets the curves of plot `state_id` are bound to, ';'-joined.
  std::string curveFiles(const QString& state_id) {
    QStringList files;
    PJ::PlotWidget* plot = PresetTestPeer::plot(*window_, state_id);
    EXPECT_NE(plot, nullptr) << state_id.toStdString();
    if (plot == nullptr) {
      return {};
    }
    auto& session = PresetTestPeer::session(*window_);
    for (const auto& curve : plot->curveList()) {
      const auto item = session.catalogModel().itemDescriptor(curve.source_name);
      files.push_back(
          item.has_value() ? PresetTestPeer::fileLoader(*window_).sourcePathForDataset(item->dataset_id) : QString());
    }
    return files.join(u';').toStdString();
  }

  // Destroy the window before its connected result storage and temporary files.
  QTemporaryDir extensions_;
  QTemporaryDir data_;
  std::vector<bool> settles_;
  std::unique_ptr<PJ::MainWindow> window_;
};

TEST_F(MainWindowStartupTest, WaitsForEveryExplicitFileBeforeApplyingGenericLayout) {
  const QString first = pj_app_test::makeMockFile(data_, QStringLiteral("first.mock"));
  const QString second = pj_app_test::makeMockFile(data_, QStringLiteral("second.mock"));
  const QString path = layout(
      "<root pj4_version=\"4\" binding=\"generic\"><tabbed_widget parent=\"main_window\">"
      "<Tab id=\"t1\" containers=\"1\"><Container><DockArea id=\"a1\" name=\"View\">"
      "<plot id=\"plot1\" mode=\"TimeSeries\"/></DockArea></Container></Tab></tabbed_widget></root>");
  window_->loadAtStartup({first, second}, path, /*interactive=*/false);
  EXPECT_TRUE(settles_.empty());
  ASSERT_TRUE(pumpUntil([&] { return !settles_.empty(); }));
  EXPECT_EQ(settles_, std::vector<bool>{true});
  EXPECT_EQ(PresetTestPeer::session(*window_).catalogModel().datasets().size(), 2u);
  pj_app_test::flushQueuedEvents();
  EXPECT_EQ(settles_.size(), 1u);
}

TEST_F(MainWindowStartupTest, ExplicitFileReplacesSavedSourcesAndRebindsQualifiedCurve) {
  const QString input = pj_app_test::makeMockFile(data_, QStringLiteral("replacement.mock"));
  const QString path = layout(
      "<root pj4_version=\"4\" binding=\"source\">"
      "<tabbed_widget parent=\"main_window\"><Tab id=\"t1\" containers=\"1\"><Container>"
      "<DockArea id=\"a1\" name=\"View\"><plot id=\"plot1\" mode=\"TimeSeries\">"
      "<curve topic=\"mock/file_data\" field=\"value\" dataset_id=\"1234\" dataset_path=\"old.mock\"/>"
      "</plot></DockArea></Container></Tab></tabbed_widget>"
      "<previouslyLoaded_Datafiles><fileInfo filename=\"old.mock\" prefix=\"\"/>"
      "</previouslyLoaded_Datafiles></root>");
  window_->loadAtStartup({input}, path, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&] { return !settles_.empty(); }));
  EXPECT_EQ(settles_, std::vector<bool>{true});
  EXPECT_EQ(PresetTestPeer::curveCount(*window_), 1);
  const auto& datasets = PresetTestPeer::session(*window_).catalogModel().datasets();
  ASSERT_EQ(datasets.size(), 1u);
  EXPECT_EQ(PresetTestPeer::fileLoader(*window_).sourcePathForDataset(datasets.front().first), input);
}

// A layout saved to compare two runs of one recording reopens on new copies of the runs: each saved dataset whose
// file name matches an explicit file keeps its binding, now to that file, although both files carry the topic.
TEST_F(MainWindowStartupTest, ExplicitFilesKeepTheDatasetsTheLayoutNamesByFileName) {
  const QString run1 = pj_app_test::makeMockFile(data_, QStringLiteral("run1.mock"));
  const QString run2 = pj_app_test::makeMockFile(data_, QStringLiteral("run2.mock"));
  const QString path = layout(
      "<root pj4_version=\"4\" binding=\"source\"><tabbed_widget parent=\"main_window\">"
      "<Tab id=\"t1\" containers=\"1\"><Container><DockSplitter orientation=\"-\" count=\"2\" sizes=\"0.5;0.5\">"
      "<DockArea id=\"a1\" name=\"Run 2\"><plot id=\"plot_run2\" mode=\"TimeSeries\">"
      "<curve topic=\"mock/file_data\" field=\"value\" dataset_id=\"2\" dataset_source=\"run2.mock\""
      " dataset_path=\"recorded/run2.mock\"/></plot></DockArea>"
      "<DockArea id=\"a2\" name=\"Run 1\"><plot id=\"plot_run1\" mode=\"TimeSeries\">"
      "<curve topic=\"mock/file_data\" field=\"value\" dataset_id=\"1\" dataset_source=\"run1.mock\""
      " dataset_path=\"recorded/run1.mock\"/></plot></DockArea>"
      "</DockSplitter></Container></Tab></tabbed_widget>"
      "<previouslyLoaded_Datafiles><fileInfo filename=\"recorded/run1.mock\" prefix=\"\"/>"
      "<fileInfo filename=\"recorded/run2.mock\" prefix=\"\"/></previouslyLoaded_Datafiles></root>");
  window_->loadAtStartup({run1, run2}, path, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&] { return !settles_.empty(); }));
  EXPECT_EQ(settles_, std::vector<bool>{true});
  EXPECT_EQ(PresetTestPeer::session(*window_).catalogModel().datasets().size(), 2u) << "saved files not replayed";
  EXPECT_EQ(curveFiles(QStringLiteral("plot_run1")), run1.toStdString());
  EXPECT_EQ(curveFiles(QStringLiteral("plot_run2")), run2.toStdString());
}

// Two explicit files sharing a file name: the saved full path picks between them. A third saved path finds both files
// claimed and stays generic (unresolved, as both files carry the topic).
TEST_F(MainWindowStartupTest, ExplicitFilesWithOneFileNameMatchByFullPathFirst) {
  ASSERT_TRUE(QDir(data_.path()).mkpath(QStringLiteral("a")));
  ASSERT_TRUE(QDir(data_.path()).mkpath(QStringLiteral("b")));
  const QString in_a = pj_app_test::makeMockFile(data_, QStringLiteral("a/run.mock"));
  const QString in_b = pj_app_test::makeMockFile(data_, QStringLiteral("b/run.mock"));
  const QByteArray curve =
      "<curve topic=\"mock/file_data\" field=\"value\" dataset_source=\"run.mock\" dataset_path=\"";
  const QString path = layout(
      "<root pj4_version=\"4\" binding=\"source\"><tabbed_widget parent=\"main_window\">"
      "<Tab id=\"t1\" containers=\"1\"><Container><DockSplitter orientation=\"-\" count=\"3\" sizes=\"0.3;0.3;0.4\">"
      "<DockArea id=\"a1\" name=\"B\"><plot id=\"plot_b\" mode=\"TimeSeries\">" +
      curve + in_b.toUtf8() +
      "\"/></plot></DockArea>"
      "<DockArea id=\"a2\" name=\"A\"><plot id=\"plot_a\" mode=\"TimeSeries\">" +
      curve +
      "a/run.mock\"/></plot></DockArea>"
      "<DockArea id=\"a3\" name=\"C\"><plot id=\"plot_c\" mode=\"TimeSeries\">" +
      curve +
      "c/run.mock\"/></plot></DockArea>"
      "</DockSplitter></Container></Tab></tabbed_widget></root>");
  window_->loadAtStartup({in_a, in_b}, path, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&] { return !settles_.empty(); }));
  EXPECT_EQ(curveFiles(QStringLiteral("plot_b")), in_b.toStdString());
  EXPECT_EQ(curveFiles(QStringLiteral("plot_a")), in_a.toStdString());
  EXPECT_EQ(curveFiles(QStringLiteral("plot_c")), "");
}

// The layout compares run1/run.mock and run2/run.mock, but the user passes run1 and another file. Run1 claims its
// file by path, so run2 has no namesake left and takes the leftover file by position instead of showing run1's data
// twice.
TEST_F(MainWindowStartupTest, ASavedRunWhoseNamesakeIsClaimedTakesTheLeftoverFile) {
  ASSERT_TRUE(QDir(data_.path()).mkpath(QStringLiteral("run1")));
  ASSERT_TRUE(QDir(data_.path()).mkpath(QStringLiteral("run2")));
  const QString run1 = pj_app_test::makeMockFile(data_, QStringLiteral("run1/run.mock"));
  const QString run2 = pj_app_test::makeMockFile(data_, QStringLiteral("run2/run.mock"));
  const QString other = pj_app_test::makeMockFile(data_, QStringLiteral("other.mock"));
  const QString path = twoRunLayout(run1, run2);
  window_->loadAtStartup({run1, other}, path, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&] { return !settles_.empty(); }));
  EXPECT_EQ(curveFiles(QStringLiteral("plot_run1")), run1.toStdString());
  EXPECT_EQ(curveFiles(QStringLiteral("plot_run2")), other.toStdString());
}

// The saved d/left.mock and d/right.mock are still on disk. e/left.mock takes left by name although it comes second,
// and right pairs with the file left over: the two never swap.
TEST_F(MainWindowStartupTest, ASavedFileStillOnDiskStillMatchesByName) {
  ASSERT_TRUE(QDir(data_.path()).mkpath(QStringLiteral("d")));
  ASSERT_TRUE(QDir(data_.path()).mkpath(QStringLiteral("e")));
  const QString saved_left = pj_app_test::makeMockFile(data_, QStringLiteral("d/left.mock"));
  const QString saved_right = pj_app_test::makeMockFile(data_, QStringLiteral("d/right.mock"));
  const QString right_new = pj_app_test::makeMockFile(data_, QStringLiteral("e/right_new.mock"));
  const QString left = pj_app_test::makeMockFile(data_, QStringLiteral("e/left.mock"));
  window_->loadAtStartup({right_new, left}, twoRunLayout(saved_left, saved_right), /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&] { return !settles_.empty(); }));
  EXPECT_EQ(curveFiles(QStringLiteral("plot_run1")), left.toStdString());
  EXPECT_EQ(curveFiles(QStringLiteral("plot_run2")), right_new.toStdString());
}

// Two new runs under new names: the layout's two datasets, in layout order, take the two files in command-line order.
TEST_F(MainWindowStartupTest, NewRunsUnderNewNamesBindByPosition) {
  const QString x = pj_app_test::makeMockFile(data_, QStringLiteral("x.mock"));
  const QString y = pj_app_test::makeMockFile(data_, QStringLiteral("y.mock"));
  window_->loadAtStartup({x, y}, twoRunLayout(u"saved/a.mock"_s, u"saved/b.mock"_s), /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&] { return !settles_.empty(); }));
  EXPECT_EQ(curveFiles(QStringLiteral("plot_run1")), x.toStdString());
  EXPECT_EQ(curveFiles(QStringLiteral("plot_run2")), y.toStdString());
}

// Three files for two saved datasets: no pairing, the qualifiers are stripped (and, as every file carries the
// topic, the curves stay unresolved).
TEST_F(MainWindowStartupTest, PositionalPairingNeedsAsManyFilesAsDatasets) {
  const QString x = pj_app_test::makeMockFile(data_, QStringLiteral("x.mock"));
  const QString y = pj_app_test::makeMockFile(data_, QStringLiteral("y.mock"));
  const QString z = pj_app_test::makeMockFile(data_, QStringLiteral("z.mock"));
  window_->loadAtStartup({x, y, z}, twoRunLayout(u"saved/a.mock"_s, u"saved/b.mock"_s), /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&] { return !settles_.empty(); }));
  EXPECT_EQ(curveFiles(QStringLiteral("plot_run1")), "");
  EXPECT_EQ(curveFiles(QStringLiteral("plot_run2")), "");
}

// One dataset placed by name, the other by position; each mapping is reported once.
TEST_F(MainWindowStartupTest, OneDatasetByNameTheOtherByPosition) {
  std::vector<std::string> mapped;
  const QMetaObject::Connection connection = QObject::connect(
      window_->diagnosticBridge(), &PJ::QtDiagnosticBridge::diagnosticReported, window_.get(),
      [&mapped](int, const QString&, const QString& id, const QString& message) {
        if (id == u"dataset-mapped"_s) {
          mapped.push_back(message.toStdString());
        }
      });
  const QString b = pj_app_test::makeMockFile(data_, QStringLiteral("b.mock"));
  const QString z = pj_app_test::makeMockFile(data_, QStringLiteral("z.mock"));
  window_->loadAtStartup({b, z}, twoRunLayout(u"saved/a.mock"_s, u"saved/b.mock"_s), /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&] { return !settles_.empty(); }));
  EXPECT_EQ(curveFiles(QStringLiteral("plot_run1")), z.toStdString());
  EXPECT_EQ(curveFiles(QStringLiteral("plot_run2")), b.toStdString());
  pj_app_test::flushQueuedEvents();
  const std::string arrow = QString(QChar(0x2192)).toStdString();
  EXPECT_EQ(
      mapped, (std::vector<std::string>{
                  "Layout dataset \"b.mock\" " + arrow + " " + b.toStdString() + " (by name)",
                  "Layout dataset \"a.mock\" " + arrow + " " + z.toStdString() + " (positional)"}));
  QObject::disconnect(connection);
}

TEST_F(MainWindowStartupTest, FailedExplicitFileAbortsLayoutAndSettlesFalseOnce) {
  const QString good = pj_app_test::makeMockFile(data_, QStringLiteral("good.mock"));
  const QString bad = pj_app_test::makeMockFile(data_, QStringLiteral("bad.unsupported"));
  const QString path = layout("<root pj4_version=\"4\" binding=\"generic\"/>");
  window_->loadAtStartup({bad, good}, path, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&] { return !settles_.empty(); }));
  EXPECT_EQ(settles_, std::vector<bool>{false});
  EXPECT_EQ(PresetTestPeer::session(*window_).catalogModel().datasets().size(), 1u);
  pj_app_test::flushQueuedEvents();
  EXPECT_EQ(settles_.size(), 1u);
}

TEST_F(MainWindowStartupTest, DataOnlyLoadsWithoutRestoringALayout) {
  const QString input = pj_app_test::makeMockFile(data_, QStringLiteral("data.mock"));
  window_->loadAtStartup({input}, {}, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&] { return !PresetTestPeer::session(*window_).catalogModel().isEmpty(); }));
  pj_app_test::flushQueuedEvents();
  EXPECT_TRUE(settles_.empty());
}

}  // namespace
