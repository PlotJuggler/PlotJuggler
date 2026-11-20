// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A workspace rebuild (layout load, a failed load's rollback, undo) hides or deletes the plot that held the keyboard
// focus. Focus must then return to that plot if it was kept, else go to the main window, never to whichever widget Qt
// finds next in the tab chain: in the browser that is a filter line edit, which takes Space from playback.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QLineEdit>
#include <QPointer>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <cstddef>
#include <string>

#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PlaybackEngine.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"

namespace {

using PJ::MainWindowLayoutImportTestPeer;
using pj_app_test::pumpUntil;
using namespace Qt::StringLiterals;

// The current tab's first plot, with a curve: an empty area has no canvas to focus.
void plotFirstCurve(PJ::MainWindow& window, std::size_t curve_index) {
  auto* tabbed = window.findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  ASSERT_NE(tabbed, nullptr);
  ASSERT_NE(tabbed->currentTab(), nullptr);
  ASSERT_GT(tabbed->currentTab()->plotCount(), 0);
  PJ::DockWidget* dock = tabbed->currentTab()->plotAt(0);
  ASSERT_NE(dock, nullptr);
  if (dock->plotWidget() == nullptr) {
    dock->setPlotWidget(new PJ::PlotWidget(nullptr, nullptr, dock));
  }
  const auto& curves = MainWindowLayoutImportTestPeer::session(window).catalogModel().curves();
  ASSERT_GT(curves.size(), curve_index);
  ASSERT_NE(dock->plotWidget()->addCurve(curves[curve_index].name), nullptr);
}

// Focuses the shown plot canvas, the widget a click on a plot focuses.
QPointer<QWidget> focusPlotCanvas(PJ::MainWindow& window) {
  QWidget* canvas = nullptr;
  EXPECT_TRUE(pumpUntil([&window, &canvas]() {
    for (QWidget* candidate : window.findChildren<QWidget*>(u"qwtCanvas"_s)) {
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

// Focus stays on the widget that held it, if the rebuild kept it, else goes to the window; never to whichever widget
// Qt picks next in the tab chain.
void expectFocusKeptForPlayback(PJ::MainWindow& window, const QPointer<QWidget>& had_focus, const char* step) {
  QWidget* focus = QApplication::focusWidget();
  const bool expected = focus == &window || (had_focus != nullptr && focus == had_focus);
  EXPECT_TRUE(expected) << step << ": focus is on " << (focus != nullptr ? focus->metaObject()->className() : "nothing")
                        << " " << (focus != nullptr ? focus->objectName().toStdString() : std::string{})
                        << (had_focus == nullptr ? ", the focused canvas is gone" : ", the focused canvas survived");
  EXPECT_EQ(qobject_cast<QLineEdit*>(focus), nullptr) << step;

  PJ::PlaybackEngine& playback = MainWindowLayoutImportTestPeer::session(window).playbackEngine();
  const bool was_playing = playback.isPlaying();
  QTest::keyClick(focus != nullptr ? focus : &window, Qt::Key_Space);
  EXPECT_NE(playback.isPlaying(), was_playing) << step << ": Space did not toggle playback";
  if (playback.isPlaying() != was_playing) {
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Space);
  }
}

TEST(MainWindowRestoreFocusTest, RebuildKeepsFocusOnThePlotOrTheWindow) {
  QTemporaryDir extensions_dir;
  QTemporaryDir project_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  ASSERT_TRUE(project_dir.isValid());

  PJ::MainWindow window(extensions_dir.path());
  ASSERT_TRUE(window.populateTestData());
  window.show();
  window.activateWindow();
  ASSERT_TRUE(QTest::qWaitForWindowActive(&window));

  plotFirstCurve(window, 0);

  bool settled = false;
  QObject::connect(&window, &PJ::MainWindow::layoutRestoreSettled, &window, [&settled](bool) { settled = true; });

  const QString layout_path = project_dir.filePath(u"plots.pj4.xml"_s);
  MainWindowLayoutImportTestPeer::saveLayoutToPath(window, layout_path, /*include_data_source=*/false);

  // The same layout restores into the live page and reuses its plot, so the focused canvas survives.
  QPointer<QWidget> canvas = focusPlotCanvas(window);
  MainWindowLayoutImportTestPeer::loadLayout(window, layout_path, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&settled]() { return settled; }));
  expectFocusKeptForPlayback(window, canvas, "layout load");

  // A tab id no live page has: the page holding the focused canvas is removed and deleted, so focus goes to the window.
  const QString other_tab_path = project_dir.filePath(u"other_tab.pj4.xml"_s);
  {
    QFile in(layout_path);
    ASSERT_TRUE(in.open(QIODevice::ReadOnly));
    QDomDocument doc;
    ASSERT_TRUE(doc.setContent(&in));
    QDomElement tab = doc.documentElement().firstChildElement(u"tabbed_widget"_s).firstChildElement(u"Tab"_s);
    ASSERT_FALSE(tab.isNull());
    tab.setAttribute(u"id"_s, u"a-page-not-on-screen"_s);
    QFile out(other_tab_path);
    ASSERT_TRUE(out.open(QIODevice::WriteOnly | QIODevice::Text));
    out.write(doc.toByteArray());
  }
  focusPlotCanvas(window);
  settled = false;
  MainWindowLayoutImportTestPeer::loadLayout(window, other_tab_path, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&settled]() { return settled; }));
  expectFocusKeptForPlayback(window, /*had_focus=*/nullptr, "layout load replacing the page");

  // Parses, then fails in the restore: the rollback rebuilds the previous plots.
  const QString broken_path = project_dir.filePath(u"broken.pj4.xml"_s);
  {
    QFile file(broken_path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&file) << "<?xml version='1.0' encoding='UTF-8'?>\n<root binding=\"generic\" pj4_version=\"4\"/>\n";
  }
  canvas = focusPlotCanvas(window);
  settled = false;
  MainWindowLayoutImportTestPeer::loadLayout(window, broken_path, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&settled]() { return settled; }));
  expectFocusKeptForPlayback(window, canvas, "failed layout load");

  plotFirstCurve(window, 1);
  MainWindowLayoutImportTestPeer::recordUndoState(window);
  ASSERT_GE(MainWindowLayoutImportTestPeer::undoSize(window), 2U);
  canvas = focusPlotCanvas(window);
  MainWindowLayoutImportTestPeer::undo(window);
  expectFocusKeptForPlayback(window, canvas, "undo");
}

}  // namespace
