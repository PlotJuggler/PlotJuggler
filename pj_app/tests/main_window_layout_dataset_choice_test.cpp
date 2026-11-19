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
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
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

class LayoutDatasetChoiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_.isValid());
    ASSERT_TRUE(data_.isValid());
    window_ = std::make_unique<PJ::MainWindow>(extensions_.path());
    QObject::connect(window_.get(), &PJ::MainWindow::layoutRestoreSettled, window_.get(), [this](bool ok) {
      settles_.push_back(ok);
    });
    QObject::connect(
        window_->diagnosticBridge(), &PJ::QtDiagnosticBridge::diagnosticReported, window_.get(),
        [this](int, const QString&, const QString& id, const QString& message) {
          diagnostics_.push_back(id + u": "_s + message);
        });
    run_a_ = addRun("a");
    run_b_ = addRun("b");
  }

  // A dataset "run.csv" at <data>/<folder>/run.csv carrying the "sensor" series; both runs share the file name, so
  // the session labels them "run.csv" and "run.csv (2)".
  PJ::DatasetId addRun(const char* folder) {
    PJ::AppSession& app = PresetTestPeer::session(*window_);
    const PJ::DatasetId id = pj_test::createDataset(app, "run.csv", /*own_time_domain=*/true);
    pj_test::addScalarTopic(app, id, "sensor");
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

  // Answers the dataset question once: exactly `rows` checked (OK), or Cancel when `rows` is empty. Every other
  // modal box is rejected and counted in other_modals_, so a test never hangs on it.
  void answerNextQuestion(std::vector<int> rows) {
    auto* timer = new QTimer(window_.get());
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, window_.get(), [this, rows, answered = false]() mutable {
      auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
      if (modal == nullptr || modal->property("pjTestSeen").isValid()) {
        return;
      }
      modal->setProperty("pjTestSeen", true);
      auto* list = modal->findChild<QListWidget*>(u"listDatasets"_s);
      if (list == nullptr || answered) {
        ++other_modals_;
        modal->reject();
        return;
      }
      answered = true;
      if (auto* reason = modal->findChild<QLabel*>(u"labelSingleOnly"_s); reason != nullptr && !reason->isHidden()) {
        single_only_ = reason->text();
      }
      for (int row = 0; row < list->count(); ++row) {
        offered_.push_back(list->item(row)->text());
      }
      if (rows.empty()) {
        modal->reject();
        return;
      }
      for (int row = 0; row < list->count(); ++row) {
        list->item(row)->setCheckState(
            std::find(rows.begin(), rows.end(), row) != rows.end() ? Qt::Checked : Qt::Unchecked);
      }
      modal->findChild<QDialogButtonBox*>(u"buttonBox"_s)->button(QDialogButtonBox::Ok)->click();
    });
    timer->start();
  }

  void load(const QByteArray& body, bool interactive) {
    PresetTestPeer::loadLayoutFile(*window_, layoutFile(body), interactive);
    ASSERT_TRUE(pj_app_test::pumpUntil([this] { return !settles_.empty(); }));
  }

  QStringList tabNames() {
    QStringList names;
    auto* tabs = window_->findChild<PJ::TabbedPlotWidget*>();
    for (int index = 0; tabs != nullptr && index < tabs->dockerCount(); ++index) {
      names.push_back(tabs->dockerAt(index)->name());
    }
    return names;
  }

  // "<dataset id>:<colour>" of each curve of plot `state_id`, in plot order; "?" for a curve bound to nothing.
  QStringList curves(const QString& state_id) {
    QStringList found;
    PJ::PlotWidget* plot = PresetTestPeer::plot(*window_, state_id);
    if (plot == nullptr) {
      return {u"no plot "_s + state_id};
    }
    const PJ::CatalogModel& catalog = PresetTestPeer::session(*window_).catalogModel();
    for (const auto& curve : plot->curveList()) {
      const auto item = catalog.itemDescriptor(curve.source_name);
      found.push_back(
          (item.has_value() ? QString::number(item->dataset_id) : u"?"_s) + u':' + curve.curve->pen().color().name());
    }
    return found;
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
  QStringList offered_;
  QString single_only_;
  int other_modals_ = 0;
  PJ::DatasetId run_a_ = 0;
  PJ::DatasetId run_b_ = 0;
  std::unique_ptr<PJ::MainWindow> window_;
};

// Intended: File > Load Layout asks; the one dataset checked is the one every reference names.
TEST_F(LayoutDatasetChoiceTest, OneCheckedDatasetIsTheOneTheLayoutShows) {
  answerNextQuestion({1});
  load(genericLayout(/*with_transform=*/true), /*interactive=*/true);
  EXPECT_EQ(offered_, (QStringList{u"run.csv"_s, u"run.csv (2)"_s}));
  EXPECT_EQ(other_modals_, 0);
  EXPECT_EQ(settles_, std::vector<bool>{true});
  EXPECT_EQ(tabNames(), (QStringList{u"Plots"_s}));
  const QString b = QString::number(run_b_);
  const QStringList bound = curves(u"p1"_s);
  ASSERT_EQ(bound.size(), 3);
  for (const QString& curve : bound) {
    EXPECT_TRUE(curve.startsWith(b + u':')) << curve.toStdString();
  }
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
  const QStringList copy_a = curves(u"p1"_s);
  const QStringList copy_b = curves(u"p1~2"_s);
  ASSERT_EQ(copy_a.size(), 2) << copy_a.join(u' ').toStdString();
  ASSERT_EQ(copy_b.size(), 2) << copy_b.join(u' ').toStdString();
  for (int index = 0; index < 2; ++index) {
    EXPECT_TRUE(copy_a[index].startsWith(QString::number(run_a_) + u':')) << copy_a[index].toStdString();
    EXPECT_TRUE(copy_b[index].startsWith(QString::number(run_b_) + u':')) << copy_b[index].toStdString();
    EXPECT_EQ(copy_a[index].section(u':', 1), copy_b[index].section(u':', 1)) << "curve " << index;
  }
  EXPECT_EQ(copy_a[0].section(u':', 1), u"#d62728"_s) << "a saved colour is copied as is";
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
  const QStringList bound = curves(u"p1"_s);
  ASSERT_EQ(bound.size(), 3);
  for (const QString& curve : bound) {
    EXPECT_TRUE(curve.startsWith(QString::number(run_b_) + u':')) << curve.toStdString();
  }
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

// Intended: an automated load (--layout, an agent, the QA instance) never asks: it takes the first loaded dataset
// and one diagnostic names that choice.
TEST_F(LayoutDatasetChoiceTest, AnAutomatedLoadTakesTheFirstDatasetAndSaysSo) {
  answerNextQuestion({1});  // a dialog would be answered with the second: the first proves none was shown
  load(genericLayout(/*with_transform=*/true), /*interactive=*/false);
  EXPECT_TRUE(offered_.isEmpty());
  EXPECT_EQ(settles_, std::vector<bool>{true});
  EXPECT_EQ(tabNames(), (QStringList{u"Plots"_s}));
  for (const QString& curve : curves(u"p1"_s)) {
    EXPECT_TRUE(curve.startsWith(QString::number(run_a_) + u':')) << curve.toStdString();
  }
  const QString chosen = diagnostic(u"layout-dataset-chosen"_s);
  EXPECT_TRUE(chosen.contains(u"run.csv"_s)) << chosen.toStdString();
}

// Intended: a layout naming two datasets keeps its own bindings and is not asked about: here neither is loaded, so
// its curves go the missing-curve way, as any unresolved curve does.
TEST_F(LayoutDatasetChoiceTest, ALayoutNamingTwoDatasetsIsNotAsked) {
  answerNextQuestion({0});
  load(
      QByteArrayLiteral(R"(<root pj4_version="4"><tabbed_widget id="" name="main" parent="main_window">
<Tab id="t1" tab_name="Compare" containers="1"><Container><DockSplitter count="1" orientation="-" sizes="1">
<DockArea id="a1" name="p"><plot id="p1" mode="TimeSeries">
<curve topic="sensor" field="value" dataset_source="x.csv" dataset_path="/nowhere/x.csv"/>
<curve topic="sensor" field="value" dataset_source="y.csv" dataset_path="/nowhere/y.csv"/>
</plot></DockArea></DockSplitter></Container></Tab></tabbed_widget></root>)"),
      /*interactive=*/true);
  EXPECT_TRUE(offered_.isEmpty());
  EXPECT_EQ(other_modals_, 1) << "the missing-curve prompt";
}

}  // namespace
