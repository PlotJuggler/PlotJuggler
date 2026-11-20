// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A workspace rebuild (layout load, a failed load's rollback, undo, a duplicated tab) hides or deletes the plot that
// held the keyboard focus. Focus must then return to that plot if it was kept, else go to the main window, never to
// whichever widget Qt finds next in the tab chain: in the browser that is a filter line edit, which takes Space from
// playback.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QLineEdit>
#include <QPointer>
#include <QTemporaryDir>
#include <QTest>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_widgets/Search.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"

namespace {

using PJ::MainWindowLayoutImportTestPeer;
using pj_app_test::pumpUntil;
using namespace Qt::StringLiterals;

std::string describe(const QWidget* widget) {
  return widget == nullptr
             ? std::string{"nothing"}
             : std::string{widget->metaObject()->className()} + " '" + widget->objectName().toStdString() + "'";
}

class MainWindowRestoreFocusTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(project_dir_.isValid());
    window_ = std::make_unique<PJ::MainWindow>(extensions_dir_.path());
    ASSERT_TRUE(window_->populateTestData());
    window_->show();
    window_->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    plotCurve(0);
    QObject::connect(
        window_.get(), &PJ::MainWindow::layoutRestoreSettled, window_.get(), [this](bool) { settled_ = true; });
    layout_path_ = project_dir_.filePath(u"plots.pj4.xml"_s);
    MainWindowLayoutImportTestPeer::saveLayoutToPath(*window_, layout_path_, /*include_data_source=*/false);
  }

  [[nodiscard]] PJ::TabbedPlotWidget* tabbed() const {
    return window_->findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  }

  // Adds a catalog curve to the current tab's first plot: an empty area has no canvas to focus.
  void plotCurve(std::size_t curve_index) {
    ASSERT_NE(tabbed(), nullptr);
    PJ::PlotDocker* docker = tabbed()->currentTab();
    ASSERT_NE(docker, nullptr);
    ASSERT_GT(docker->plotCount(), 0);
    PJ::DockWidget* dock = docker->plotAt(0);
    ASSERT_NE(dock, nullptr);
    if (dock->plotWidget() == nullptr) {
      dock->setPlotWidget(new PJ::PlotWidget(nullptr, nullptr, dock));
    }
    const auto& curves = MainWindowLayoutImportTestPeer::session(*window_).catalogModel().curves();
    ASSERT_GT(curves.size(), curve_index);
    ASSERT_NE(dock->plotWidget()->addCurve(curves[curve_index].name), nullptr);
  }

  // Focuses the shown plot canvas, the widget a click on a plot focuses.
  QPointer<QWidget> focusPlotCanvas() {
    QWidget* canvas = nullptr;
    EXPECT_TRUE(pumpUntil([this, &canvas]() {
      for (QWidget* candidate : window_->findChildren<QWidget*>(u"qwtCanvas"_s)) {
        if (candidate->isVisible()) {
          canvas = candidate;
          return true;
        }
      }
      return false;
    }));
    if (canvas != nullptr) {
      canvas->setFocus(Qt::MouseFocusReason);
    }
    EXPECT_EQ(QApplication::focusWidget(), canvas);
    return canvas;
  }

  void loadLayout(const QString& path) {
    settled_ = false;
    MainWindowLayoutImportTestPeer::loadLayout(*window_, path, /*interactive=*/false);
    ASSERT_TRUE(pumpUntil([this]() { return settled_; }));
  }

  // The saved layout with `edit` applied to its document, written next to it.
  QString editedLayout(const QString& name, const std::function<void(QDomDocument&)>& edit) {
    QFile in(layout_path_);
    EXPECT_TRUE(in.open(QIODevice::ReadOnly));
    QDomDocument doc;
    EXPECT_TRUE(doc.setContent(&in));
    edit(doc);
    const QString path = project_dir_.filePath(name);
    QFile out(path);
    EXPECT_TRUE(out.open(QIODevice::WriteOnly | QIODevice::Text));
    out.write(doc.toByteArray());
    return path;
  }

  static QDomElement firstTab(QDomDocument& doc) {
    return doc.documentElement().firstChildElement(u"tabbed_widget"_s).firstChildElement(u"Tab"_s);
  }

  void expectFocusOn(const QWidget* expected, const char* step) {
    EXPECT_EQ(QApplication::focusWidget(), expected)
        << step << ": focus is on " << describe(QApplication::focusWidget()) << ", expected " << describe(expected);
  }

  void expectSpaceTogglesPlayback(const char* step) {
    QWidget* focus = QApplication::focusWidget();
    ASSERT_NE(focus, nullptr) << step;
    EXPECT_EQ(qobject_cast<QLineEdit*>(focus), nullptr) << step << ": focus is on " << describe(focus);
    PJ::PlaybackEngine& playback = MainWindowLayoutImportTestPeer::session(*window_).playbackEngine();
    const bool was_playing = playback.isPlaying();
    QTest::keyClick(focus, Qt::Key_Space);
    EXPECT_NE(playback.isPlaying(), was_playing) << step << ": Space did not toggle playback";
    if (playback.isPlaying() != was_playing) {
      QTest::keyClick(QApplication::focusWidget(), Qt::Key_Space);
    }
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir project_dir_;
  std::unique_ptr<PJ::MainWindow> window_;
  QString layout_path_;
  bool settled_ = false;
};

TEST_F(MainWindowRestoreFocusTest, LayoutLoadReusingThePlotKeepsItsFocus) {
  const QPointer<QWidget> canvas = focusPlotCanvas();
  loadLayout(layout_path_);
  ASSERT_NE(canvas, nullptr) << "the restore was expected to reuse the plot";
  expectFocusOn(canvas, "layout load");
  expectSpaceTogglesPlayback("layout load");
}

TEST_F(MainWindowRestoreFocusTest, LayoutLoadDeletingThePlotFocusesTheWindow) {
  // A tab id no live page has: the page holding the focused canvas is removed and deleted.
  const QString path = editedLayout(u"other_tab.pj4.xml"_s, [](QDomDocument& doc) {
    firstTab(doc).setAttribute(u"id"_s, u"a-page-not-on-screen"_s);
  });
  focusPlotCanvas();
  loadLayout(path);
  expectFocusOn(window_.get(), "layout load replacing the page");
  expectSpaceTogglesPlayback("layout load replacing the page");
}

TEST_F(MainWindowRestoreFocusTest, FailedLoadRollbackKeepsThePlotFocus) {
  // The first tab restores into the live page; the second one is invalid, so the plot rebuild fails and the rollback
  // restores the previous workspace.
  const QString path = editedLayout(u"second_tab_invalid.pj4.xml"_s, [](QDomDocument& doc) {
    QDomElement broken = doc.createElement(u"Tab"_s);
    broken.setAttribute(u"id"_s, u"broken-tab"_s);
    broken.appendChild(doc.createElement(u"Container"_s));
    broken.appendChild(doc.createElement(u"Container"_s));
    doc.documentElement().firstChildElement(u"tabbed_widget"_s).appendChild(broken);
  });
  // The restore's own diagnostics are folded into the failure report the user sees.
  QString failure;
  QObject::connect(
      MainWindowLayoutImportTestPeer::diagnosticBridge(*window_), &PJ::QtDiagnosticBridge::diagnosticReported,
      window_.get(), [&failure](int /*level*/, const QString& /*source*/, const QString& id, const QString& message) {
        if (id == u"layout-apply-failed"_s) {
          failure = message;
        }
      });
  const QPointer<QWidget> canvas = focusPlotCanvas();
  loadLayout(path);
  ASSERT_TRUE(pumpUntil([&failure]() { return !failure.isEmpty(); }));
  ASSERT_TRUE(failure.contains(u"plot tabs could not be reconstructed"_s))
      << "the load was expected to fail in the plot rebuild: " << failure.toStdString();
  ASSERT_NE(canvas, nullptr) << "the rollback was expected to reuse the plot";
  expectFocusOn(canvas, "failed layout load");
  expectSpaceTogglesPlayback("failed layout load");
}

TEST_F(MainWindowRestoreFocusTest, UndoReusingThePlotKeepsItsFocus) {
  MainWindowLayoutImportTestPeer::recordUndoState(*window_);
  plotCurve(1);
  MainWindowLayoutImportTestPeer::recordUndoState(*window_);
  ASSERT_EQ(MainWindowLayoutImportTestPeer::totalCurveCount(*window_), 2);
  const std::size_t undo_size = MainWindowLayoutImportTestPeer::undoSize(*window_);
  ASSERT_GE(undo_size, 2U);
  const QPointer<QWidget> canvas = focusPlotCanvas();
  MainWindowLayoutImportTestPeer::undo(*window_);
  ASSERT_EQ(MainWindowLayoutImportTestPeer::undoSize(*window_), undo_size - 1) << "the undo did not run";
  ASSERT_EQ(MainWindowLayoutImportTestPeer::totalCurveCount(*window_), 1);
  ASSERT_NE(canvas, nullptr) << "the undo was expected to reuse the plot";
  expectFocusOn(canvas, "undo");
  expectSpaceTogglesPlayback("undo");
}

TEST_F(MainWindowRestoreFocusTest, LayoutLoadLeavesFocusInAFilter) {
  auto* filter = window_->findChild<PJ::Search*>(u"filterTimeseries"_s);
  ASSERT_NE(filter, nullptr);
  filter->lineEdit()->setFocus(Qt::MouseFocusReason);
  ASSERT_EQ(QApplication::focusWidget(), filter->lineEdit());
  loadLayout(layout_path_);
  expectFocusOn(filter->lineEdit(), "layout load while typing in a filter");
}

TEST_F(MainWindowRestoreFocusTest, DuplicateTabKeepsFocusOffTheTabChain) {
  focusPlotCanvas();
  PJ::PlotDocker* copy = tabbed()->duplicateTab(tabbed()->currentTab());
  ASSERT_NE(copy, nullptr);
  QWidget* focus = QApplication::focusWidget();
  EXPECT_TRUE(focus == window_.get() || (focus != nullptr && (focus == copy || copy->isAncestorOf(focus))))
      << "duplicate tab: focus is on " << describe(focus) << ", expected the copy or the window";
  expectSpaceTogglesPlayback("duplicate tab");
}

}  // namespace
