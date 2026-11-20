// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// One layout over several datasets. These tests pin intended behaviour (pj_app ARCHITECTURE "One layout over several
// datasets"): a layout aimed at one dataset, applied while several loaded datasets carry its topics, asks which ones
// it should show; several choices duplicate its tabs, one copy per dataset; an automated load takes the first loaded
// dataset and says so. Its references are never left ambiguous. A failure here is a contract break.

#include <gtest/gtest.h>
#include <qwt_plot_curve.h>

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "support/loader_test_support.h"
#include "support/preset_test_support.h"

using namespace Qt::StringLiterals;

namespace {

using PJ::PresetTestPeer;

constexpr char kNegate[] = R"LUAU(-- pj-script: luau
return { id="negate", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return -v end } end }
)LUAU";

// A generic layout made for one run: one tab plotting the raw sensor (saved colour), a filter output (no saved
// colour) and, `with_transform`, a transform output (no saved colour).
QByteArray genericLayout(bool with_transform) {
  const QString transform_curve = with_transform ? u"<curve topic=\"sensor/negated\" field=\"value\"/>"_s : QString();
  const QString transform = with_transform
                                ? uR"(<transform owner_plugin="pluginA" id="negate" backend="luau" api_version="1">
<input name="sensor/value" topic="sensor" field="value" column="0"/><output name="sensor/negated"/>
<params><![CDATA[{}]]></params><script><![CDATA[)"_s +
                                      QString::fromLatin1(kNegate) + u"]]></script></transform>"_s
                                : QString();
  return (uR"(<root pj4_version="4" binding="generic"><tabbed_widget id="" name="main" parent="main_window">
<Tab id="t1" tab_name="Plots" containers="1"><Container><DockSplitter count="1" orientation="-" sizes="1">
<DockArea id="a1" name="p"><plot id="p1" mode="TimeSeries">
<curve topic="sensor" field="value" color="#d62728"/>
<curve topic="sensor_abs" field="value"/>)"_s +
          transform_curve +
          uR"(
</plot></DockArea></DockSplitter></Container></Tab><currentTabIndex index="0"/></tabbed_widget>
<data_processors>
<processor input_topic="sensor" input_field="value" output_name="sensor_abs" processor_id="absolute"><![CDATA[{}]]></processor>
)"_s + transform +
          u"</data_processors></root>"_s)
      .toUtf8();
}

// A generic layout made for one run: one tab, one plot, a curve per entry of `curves` ("<topic>" or
// "<topic>@<source>@<path>" for a qualified one), field "value".
QByteArray curveLayout(const QStringList& curves) {
  QString body;
  for (const QString& curve : curves) {
    const QStringList parts = curve.split(u'@');
    body += u"<curve topic=\"%1\" field=\"value\""_s.arg(parts[0]);
    if (parts.size() == 3) {
      body += u" dataset_source=\"%1\" dataset_path=\"%2\""_s.arg(parts[1], parts[2]);
    }
    body += u"/>"_s;
  }
  return (uR"(<root pj4_version="4" binding="generic"><tabbed_widget id="" name="main" parent="main_window">
<Tab id="t1" tab_name="Plots" containers="1"><Container><DockSplitter count="1" orientation="-" sizes="1">
<DockArea id="a1" name="p"><plot id="p1" mode="TimeSeries">)"_s +
          body + u"</plot></DockArea></DockSplitter></Container></Tab></tabbed_widget></root>"_s)
      .toUtf8();
}

class LayoutDatasetChoiceTest : public ::testing::Test {
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
    QObject::connect(
        window_->diagnosticBridge(), &PJ::QtDiagnosticBridge::diagnosticReported, window_.get(),
        [this](int, const QString&, const QString& id, const QString& message) {
          diagnostics_.push_back(id + u": "_s + message);
        });
    auto* timer = new QTimer(window_.get());
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, window_.get(), [this]() { answerModal(); });
    timer->start();
    // Two runs recorded a second apart.
    run_a_ = addRun("a", 100);
    run_b_ = addRun("b", 1'000'000'100);
  }

  // A dataset "run.csv" at <data>/<folder>/run.csv carrying the "sensor" series from `start_ns`; both runs share the
  // file name, so the session labels them "run.csv" and "run.csv (2)".
  PJ::DatasetId addRun(const char* folder, PJ::Timestamp start_ns) {
    PJ::AppSession& app = PresetTestPeer::session(*window_);
    const PJ::DatasetId id = pj_test::createDataset(app, "run.csv", /*own_time_domain=*/true);
    pj_test::addScalarTopic(app, id, "sensor", start_ns, start_ns + 100);
    app.sessionManager().setDatasetSourcePath(id, data_.filePath(QString::fromLatin1(folder) + u"/run.csv"_s));
    return id;
  }

  QString layoutFile(const QByteArray& body) {
    const QString path = data_.filePath(u"layout.pj4.xml"_s);
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write(body), body.size());
    return path;
  }

  // The next dataset question is answered with exactly `rows` checked (OK), or Cancel when `rows` is empty.
  void answerNextQuestion(std::vector<int> rows) {
    answers_.push_back(std::move(rows));
  }

  // A dataset question takes the next queued answer; every other modal box, and a question nobody queued an answer
  // for, is rejected and counted in other_modals_, so a test never hangs on it. Answers are queued: a modal the
  // answer opens runs a nested loop, and the polling timer never fires again inside its own slot.
  void answerModal() {
    auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (modal == nullptr || modal->property("pjTestSeen").isValid()) {
      return;
    }
    modal->setProperty("pjTestSeen", true);
    auto* list = modal->findChild<QListWidget*>(u"listDatasets"_s);
    if (list == nullptr || answers_.empty()) {
      ++other_modals_;
      for (const QLabel* label : modal->findChildren<QLabel*>()) {
        other_texts_ += label->text();
      }
      QMetaObject::invokeMethod(modal, &QDialog::reject, Qt::QueuedConnection);
      return;
    }
    const std::vector<int> rows = answers_.front();
    answers_.pop_front();
    ++asked_;
    if (auto* reason = modal->findChild<QLabel*>(u"labelSingleOnly"_s); reason != nullptr && !reason->isHidden()) {
      single_only_ = reason->text();
    }
    offered_.clear();
    for (int row = 0; row < list->count(); ++row) {
      offered_.push_back(list->item(row)->text());
    }
    if (rows.empty()) {
      QMetaObject::invokeMethod(modal, &QDialog::reject, Qt::QueuedConnection);
      return;
    }
    for (int row = 0; row < list->count(); ++row) {
      list->item(row)->setCheckState(
          std::find(rows.begin(), rows.end(), row) != rows.end() ? Qt::Checked : Qt::Unchecked);
    }
    QPushButton* ok = modal->findChild<QDialogButtonBox*>(u"buttonBox"_s)->button(QDialogButtonBox::Ok);
    QTimer::singleShot(0, ok, [this, ok]() {
      if (before_answer_) {
        before_answer_();
      }
      ok->click();
    });
  }

  void load(const QByteArray& body, bool interactive) {
    const std::size_t settled = settles_.size();
    PresetTestPeer::loadLayoutFile(*window_, layoutFile(body), interactive);
    ASSERT_TRUE(pj_app_test::pumpUntil([this, settled] { return settles_.size() > settled; }));
  }

  QStringList tabNames() {
    QStringList names;
    auto* tabs = window_->findChild<PJ::TabbedPlotWidget*>();
    for (int index = 0; tabs != nullptr && index < tabs->dockerCount(); ++index) {
      names.push_back(tabs->dockerAt(index)->name());
    }
    return names;
  }

  // The first plot of tab `tab`.
  PJ::PlotWidget* plotOfTab(int tab) {
    auto* tabs = window_->findChild<PJ::TabbedPlotWidget*>();
    if (tabs == nullptr || tab >= tabs->dockerCount() || tabs->dockerAt(tab)->plotCount() == 0) {
      return nullptr;
    }
    PJ::DockWidget* dock = tabs->dockerAt(tab)->plotAt(0);
    return dock != nullptr ? dock->plotWidget() : nullptr;
  }

  // "<dataset id>:<colour>" of each curve the first plot of tab `tab` shows, in plot order.
  QStringList curves(int tab = 0) {
    QStringList found;
    PJ::PlotWidget* plot = plotOfTab(tab);
    if (plot == nullptr) {
      return {u"no plot on tab "_s + QString::number(tab)};
    }
    const PJ::CatalogModel& catalog = PresetTestPeer::session(*window_).catalogModel();
    for (const auto& curve : plot->curveList()) {
      const auto item = catalog.itemDescriptor(curve.source_name);
      found.push_back(
          (item.has_value() ? QString::number(item->dataset_id) : u"?"_s) + u':' + curve.curve->pen().color().name());
    }
    return found;
  }

  // Every curve of tab `tab` is bound to `dataset`, and there are `count` of them.
  void expectAllBoundTo(int tab, PJ::DatasetId dataset, int count) {
    const QStringList bound = curves(tab);
    ASSERT_EQ(bound.size(), count) << bound.join(u' ').toStdString();
    for (const QString& curve : bound) {
      EXPECT_TRUE(curve.startsWith(QString::number(dataset) + u':')) << curve.toStdString();
    }
  }

  // The display time `dataset` starts at.
  double startOf(PJ::DatasetId dataset) {
    const auto range = PresetTestPeer::session(*window_).sessionManager().datasetDisplayRange(dataset);
    return range.has_value() ? range->min.value : std::numeric_limits<double>::quiet_NaN();
  }

  QString diagnostic(const QString& id) {
    QCoreApplication::processEvents();
    for (const QString& line : diagnostics_) {
      if (line.startsWith(id + u": "_s)) {
        return line;
      }
    }
    return {};
  }

  QTemporaryDir extensions_;
  QTemporaryDir data_;
  std::vector<bool> settles_;
  std::vector<QString> diagnostics_;
  std::deque<std::vector<int>> answers_;
  QStringList offered_;  ///< the rows of the last question
  QString single_only_;
  std::function<void()> before_answer_;  ///< runs while the question is open, just before it is answered
  int asked_ = 0;
  int other_modals_ = 0;
  QString other_texts_;  ///< the label texts of the other modal boxes
  PJ::DatasetId run_a_ = 0;
  PJ::DatasetId run_b_ = 0;
  std::unique_ptr<PJ::MainWindow> window_;
};

// Intended: File > Load Layout asks; the one dataset checked is the one every reference names, and its time stays
// where it was (nothing to compare it with).
TEST_F(LayoutDatasetChoiceTest, OneCheckedDatasetIsTheOneTheLayoutShows) {
  const double start_a = startOf(run_a_);
  const double start_b = startOf(run_b_);
  answerNextQuestion({1});
  load(genericLayout(/*with_transform=*/true), /*interactive=*/true);
  EXPECT_EQ(offered_, (QStringList{u"run.csv"_s, u"run.csv (2)"_s}));
  EXPECT_EQ(other_modals_, 0);
  EXPECT_EQ(settles_, std::vector<bool>{true});
  EXPECT_EQ(tabNames(), (QStringList{u"Plots"_s}));
  expectAllBoundTo(0, run_b_, 3);
  EXPECT_DOUBLE_EQ(startOf(run_a_), start_a);
  EXPECT_DOUBLE_EQ(startOf(run_b_), start_b);
}

// Intended: two checked datasets duplicate the tab, one copy per dataset, each copy's curves bound to its own dataset
// (the filter computed per copy), and every curve in the same colour in both copies (the copies exist to compare
// runs), whether the layout saved its colour or not.
TEST_F(LayoutDatasetChoiceTest, TwoCheckedDatasetsDuplicateTheTabsInTheSameColours) {
  answerNextQuestion({0, 1});
  load(genericLayout(/*with_transform=*/false), /*interactive=*/true);
  EXPECT_TRUE(single_only_.isEmpty()) << single_only_.toStdString();
  EXPECT_EQ(other_modals_, 0);
  EXPECT_EQ(settles_, std::vector<bool>{true});
  EXPECT_EQ(tabNames(), (QStringList{u"Plots [run.csv]"_s, u"Plots [run.csv (2)]"_s}));
  expectAllBoundTo(0, run_a_, 2);
  expectAllBoundTo(1, run_b_, 2);
  const QStringList copy_a = curves(0);
  const QStringList copy_b = curves(1);
  ASSERT_EQ(copy_a.size(), copy_b.size());
  for (int index = 0; index < copy_a.size(); ++index) {
    EXPECT_EQ(copy_a[index].section(u':', 1), copy_b[index].section(u':', 1)) << "curve " << index;
  }
  EXPECT_EQ(copy_a[0].section(u':', 1), u"#d62728"_s) << "a saved colour is copied as is";
}

// Intended: the copies are there to compare the runs, so the checked datasets start at the same display time, as
// the Source Timeline's "Align starts" would place them, and each copy's plot shows its run there.
TEST_F(LayoutDatasetChoiceTest, TwoCheckedDatasetsStartAtTheSameTime) {
  const PJ::DatasetId run_c = addRun("c", 2'000'000'100);
  const double start_c = startOf(run_c);
  ASSERT_NE(startOf(run_a_), startOf(run_b_));
  answerNextQuestion({0, 1});
  load(genericLayout(/*with_transform=*/false), /*interactive=*/true);
  EXPECT_EQ(offered_.size(), 3);
  EXPECT_EQ(settles_, std::vector<bool>{true});
  EXPECT_DOUBLE_EQ(startOf(run_a_), startOf(run_b_));
  EXPECT_DOUBLE_EQ(startOf(run_c), start_c) << "an unchecked dataset is not moved";
  for (const auto& [tab, dataset] : {std::pair{0, run_a_}, std::pair{1, run_b_}}) {
    PJ::PlotWidget* plot = plotOfTab(tab);
    ASSERT_NE(plot, nullptr) << tab;
    const auto shows_its_run = [this, plot, dataset]() {
      const QRectF view = plot->currentBoundingRect();
      return view.left() <= startOf(dataset) && startOf(dataset) <= view.right();
    };
    EXPECT_TRUE(pj_app_test::pumpUntil(shows_its_run))
        << "tab " << tab << " shows x " << plot->currentBoundingRect().left() << ".."
        << plot->currentBoundingRect().right() << ", its run starts at " << startOf(dataset);
  }
}

// Intended: a layout holding a transform shows one dataset at a time, and the dialog says why: the transform's id is
// its script's class id, so a second copy cannot exist. Checking a second dataset unchecks the first.
TEST_F(LayoutDatasetChoiceTest, ALayoutWithATransformShowsOneDatasetAtATime) {
  answerNextQuestion({0, 1});
  load(genericLayout(/*with_transform=*/true), /*interactive=*/true);
  EXPECT_TRUE(single_only_.contains(u"pluginA/negate"_s)) << single_only_.toStdString();
  EXPECT_EQ(other_modals_, 0);
  EXPECT_EQ(settles_, std::vector<bool>{true});
  EXPECT_EQ(tabNames(), (QStringList{u"Plots"_s}));
  expectAllBoundTo(0, run_b_, 3);
}

// Intended: Cancel leaves the workspace as it was, like the missing-data prompt's Cancel.
TEST_F(LayoutDatasetChoiceTest, CancelLeavesTheWorkspaceUnchanged) {
  const QByteArray before = PresetTestPeer::liveState(*window_);
  answerNextQuestion({});
  load(genericLayout(/*with_transform=*/false), /*interactive=*/true);
  EXPECT_EQ(offered_.size(), 2);
  EXPECT_EQ(settles_, std::vector<bool>{false});
  EXPECT_EQ(PresetTestPeer::liveState(*window_), before);
}

// Intended: datasets removed while the question was open are said so, not taken for a Cancel.
TEST_F(LayoutDatasetChoiceTest, ChosenDatasetsRemovedMeanwhileAreReported) {
  answerNextQuestion({0, 1});
  before_answer_ = [this]() {
    PJ::SessionManager& sessions = PresetTestPeer::session(*window_).sessionManager();
    sessions.removeDataset(run_a_);
    sessions.removeDataset(run_b_);
  };
  load(genericLayout(/*with_transform=*/false), /*interactive=*/true);
  EXPECT_EQ(settles_, std::vector<bool>{false});
  EXPECT_EQ(other_modals_, 1);
  EXPECT_TRUE(other_texts_.contains(u"removed"_s)) << other_texts_.toStdString();
}

// Intended: a choice partly removed while the question was open shows the rest and says some are gone.
TEST_F(LayoutDatasetChoiceTest, SomeChosenDatasetsRemovedMeanwhileAreReported) {
  answerNextQuestion({0, 1});
  before_answer_ = [this]() { PresetTestPeer::session(*window_).sessionManager().removeDataset(run_a_); };
  load(genericLayout(/*with_transform=*/false), /*interactive=*/true);
  EXPECT_EQ(settles_, std::vector<bool>{true});
  EXPECT_EQ(other_modals_, 1);
  EXPECT_TRUE(other_texts_.contains(u"removed"_s)) << other_texts_.toStdString();
  EXPECT_EQ(tabNames(), (QStringList{u"Plots"_s}));
  expectAllBoundTo(0, run_b_, 2);
}

// Intended: an automated load (an agent, the QA instance, --layout without a display) never asks: it takes the first
// loaded dataset and one diagnostic names that choice.
TEST_F(LayoutDatasetChoiceTest, AnAutomatedLoadTakesTheFirstDatasetAndSaysSo) {
  answerNextQuestion({1});  // a dialog would be answered with the second: the first proves none was shown
  load(genericLayout(/*with_transform=*/true), /*interactive=*/false);
  EXPECT_EQ(asked_, 0);
  EXPECT_EQ(settles_, std::vector<bool>{true});
  EXPECT_EQ(tabNames(), (QStringList{u"Plots"_s}));
  expectAllBoundTo(0, run_a_, 3);
  const QString chosen = diagnostic(u"layout-dataset-chosen"_s);
  EXPECT_TRUE(chosen.contains(u"run.csv"_s)) << chosen.toStdString();
}

// Intended: a startup layout over explicit files (-d ... -l ...) asks like File > Load Layout when someone is at the
// display; unattended, it takes the first dataset.
TEST_F(LayoutDatasetChoiceTest, AStartupLayoutAsksWhenSomeoneIsThere) {
  const QString first = pj_app_test::makeMockFile(data_, u"first.mock"_s);
  const QString second = pj_app_test::makeMockFile(data_, u"second.mock"_s);
  answerNextQuestion({1});
  window_->loadAtStartup({first, second}, layoutFile(curveLayout({u"mock/file_data"_s})), /*interactive=*/true);
  ASSERT_TRUE(pj_app_test::pumpUntil([this] { return !settles_.empty(); }));
  EXPECT_EQ(asked_, 1);
  EXPECT_EQ(offered_.size(), 2);
  EXPECT_EQ(other_modals_, 0);
  const QStringList bound = curves(0);
  ASSERT_EQ(bound.size(), 1);
  EXPECT_EQ(
      PresetTestPeer::fileLoader(*window_).sourcePathForDataset(bound.front().section(u':', 0, 0).toUInt()), second);
}

TEST_F(LayoutDatasetChoiceTest, AnUnattendedStartupLayoutTakesTheFirstDataset) {
  const QString first = pj_app_test::makeMockFile(data_, u"first.mock"_s);
  const QString second = pj_app_test::makeMockFile(data_, u"second.mock"_s);
  answerNextQuestion({1});
  window_->loadAtStartup({first, second}, layoutFile(curveLayout({u"mock/file_data"_s})), /*interactive=*/false);
  ASSERT_TRUE(pj_app_test::pumpUntil([this] { return !settles_.empty(); }));
  EXPECT_EQ(asked_, 0);
  EXPECT_EQ(other_modals_, 0);
  EXPECT_EQ(settles_, std::vector<bool>{true});
  EXPECT_FALSE(diagnostic(u"layout-dataset-chosen"_s).isEmpty());
  const QStringList bound = curves(0);
  ASSERT_EQ(bound.size(), 1);
  EXPECT_EQ(
      PresetTestPeer::fileLoader(*window_).sourcePathForDataset(bound.front().section(u':', 0, 0).toUInt()), first);
}

// Intended: a layout naming two datasets keeps its own bindings and is not asked about: here neither is loaded, so
// its curves go the missing-curve way, as any unresolved curve does.
TEST_F(LayoutDatasetChoiceTest, ALayoutNamingTwoDatasetsIsNotAsked) {
  answerNextQuestion({0});
  load(curveLayout({u"sensor@x.csv@/nowhere/x.csv"_s, u"sensor@y.csv@/nowhere/y.csv"_s}), /*interactive=*/true);
  EXPECT_EQ(asked_, 0);
  EXPECT_EQ(other_modals_, 1) << "the missing-curve prompt";
}

// Intended: a layout naming a loaded file under a label it no longer has (renamed, or saved from a fan-out entry) still
// names that file, so it keeps its binding, unasked, though another dataset carries the same topics.
TEST_F(LayoutDatasetChoiceTest, ALayoutNamingALoadedFileUnderAnotherLabelKeepsIt) {
  load(curveLayout({u"sensor@old.csv@"_s + data_.filePath(u"a/run.csv"_s)}), /*interactive=*/true);
  EXPECT_EQ(asked_, 0);
  EXPECT_EQ(other_modals_, 0);
  EXPECT_EQ(settles_, std::vector<bool>{true});
  expectAllBoundTo(0, run_a_, 1);
}

// Intended: the entries of one fan-out file share its path and differ by label; a layout naming each entry keeps
// each curve on its own entry, unasked.
TEST_F(LayoutDatasetChoiceTest, ALayoutOverFanOutEntriesKeepsEachEntrysCurves) {
  PJ::AppSession& app = PresetTestPeer::session(*window_);
  const QString file = data_.filePath(u"log.mcap"_s);
  std::vector<PJ::DatasetId> entries;
  for (const char* label : {"log/left", "log/right"}) {
    entries.push_back(pj_test::createDataset(app, label, /*own_time_domain=*/true));
    pj_test::addScalarTopic(app, entries.back(), "sensor");
    app.sessionManager().setDatasetSourcePath(entries.back(), file);
  }
  load(curveLayout({u"sensor@log/left@"_s + file, u"sensor@log/right@"_s + file}), /*interactive=*/true);
  EXPECT_EQ(asked_, 0);
  EXPECT_EQ(other_modals_, 0);
  const QStringList bound = curves(0);
  ASSERT_EQ(bound.size(), 2);
  EXPECT_TRUE(bound[0].startsWith(QString::number(entries[0]) + u':')) << bound[0].toStdString();
  EXPECT_TRUE(bound[1].startsWith(QString::number(entries[1]) + u':')) << bound[1].toStdString();
}

// Intended: loading the same layout again asks again. The first load's filter output, now in the chosen dataset, is
// the layout's own and does not make that dataset fit better.
TEST_F(LayoutDatasetChoiceTest, LoadingTheLayoutAgainAsksAgain) {
  answerNextQuestion({0});
  load(genericLayout(/*with_transform=*/false), /*interactive=*/true);
  answerNextQuestion({1});
  load(genericLayout(/*with_transform=*/false), /*interactive=*/true);
  EXPECT_EQ(asked_, 2);
  EXPECT_EQ(settles_, (std::vector<bool>{true, true}));
  expectAllBoundTo(0, run_b_, 2);
}

// Intended: when one dataset carries more of the layout's topics than any other, it is the one the layout shows,
// unasked: the topics the others share bind to it too.
TEST_F(LayoutDatasetChoiceTest, TheBestFittingDatasetTakesTheTopicsItShares) {
  pj_test::addScalarTopic(PresetTestPeer::session(*window_), run_b_, "extra");
  load(curveLayout({u"sensor"_s, u"extra"_s}), /*interactive=*/true);
  EXPECT_EQ(asked_, 0);
  EXPECT_EQ(other_modals_, 0);
  EXPECT_EQ(settles_, std::vector<bool>{true});
  expectAllBoundTo(0, run_b_, 2);
}

// Intended: a layout whose curves come from two files with no topic in common (what the browser's Save Layout writes
// for such a session) is not asked about: no reference is ambiguous, and each binds to the file carrying its topic.
TEST_F(LayoutDatasetChoiceTest, FilesWithNoTopicInCommonAreNotAsked) {
  PJ::AppSession& app = PresetTestPeer::session(*window_);
  const PJ::DatasetId data = pj_test::createDataset(app, "data.csv", /*own_time_domain=*/true);
  pj_test::addScalarTopic(app, data, "data");
  const PJ::DatasetId run = pj_test::createDataset(app, "other.csv", /*own_time_domain=*/true);
  pj_test::addScalarTopic(app, run, "run");
  load(curveLayout({u"data"_s, u"run"_s}), /*interactive=*/true);
  EXPECT_EQ(asked_, 0);
  EXPECT_EQ(other_modals_, 0) << other_texts_.toStdString();
  EXPECT_EQ(settles_, std::vector<bool>{true});
  const QStringList bound = curves(0);
  ASSERT_EQ(bound.size(), 2) << bound.join(u' ').toStdString();
  EXPECT_TRUE(bound[0].startsWith(QString::number(data) + u':')) << bound[0].toStdString();
  EXPECT_TRUE(bound[1].startsWith(QString::number(run) + u':')) << bound[1].toStdString();
}

// Intended: a layout made for a file that is not loaded, whose topics no two loaded files share, is not asked about
// and binds each curve to the one file carrying its topic, as a generic layout would.
TEST_F(LayoutDatasetChoiceTest, ALayoutForAMissingFileBindsWhereEachTopicIsUnique) {
  PJ::AppSession& app = PresetTestPeer::session(*window_);
  const PJ::DatasetId a = pj_test::createDataset(app, "a.csv", /*own_time_domain=*/true);
  pj_test::addScalarTopic(app, a, "a");
  const PJ::DatasetId b = pj_test::createDataset(app, "b.csv", /*own_time_domain=*/true);
  pj_test::addScalarTopic(app, b, "b");
  load(curveLayout({u"a@x.mcap@/nowhere/x.mcap"_s, u"b@x.mcap@/nowhere/x.mcap"_s}), /*interactive=*/true);
  EXPECT_EQ(asked_, 0);
  EXPECT_EQ(other_modals_, 0) << other_texts_.toStdString();
  EXPECT_EQ(settles_, std::vector<bool>{true});
  const QStringList bound = curves(0);
  ASSERT_EQ(bound.size(), 2) << bound.join(u' ').toStdString();
  EXPECT_TRUE(bound[0].startsWith(QString::number(a) + u':')) << bound[0].toStdString();
  EXPECT_TRUE(bound[1].startsWith(QString::number(b) + u':')) << bound[1].toStdString();
}

}  // namespace
