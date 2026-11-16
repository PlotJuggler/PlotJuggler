// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// "Maximize one plot" (the dock title-bar Fullscreen button) must let the
// surviving dock reflow to fill the whole tab, even in heterogeneous /
// asymmetric nested-splitter layouts. The regression it guards against: hiding
// sibling areas with a raw CDockAreaWidget::setVisible(false) never collapses
// the emptied intermediate CDockSplitters, so the survivor is stranded in its
// own sub-branch and the rest of the tab goes blank.

#include <DockAreaWidget.h>
#include <DockSplitter.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QPushButton>
#include <algorithm>
#include <string>

#include "pj_plotting/DockToolbar.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace {

// Counts CDockSplitters that are still shown yet hold no visible content — i.e.
// "ghost" branches occupying blank space. Zero is the layout invariant the
// fullscreen toggle must preserve on enter and exit.
int visibleEmptySplitters(QWidget& docker) {
  int count = 0;
  for (auto* splitter : docker.findChildren<ads::CDockSplitter*>()) {
    if (!splitter->isHidden() && !splitter->hasVisibleContent()) {
      ++count;
    }
  }
  return count;
}

void clickFullscreen(PJ::DockWidget* dock) {
  dock->toolBar()->buttonFullscreen()->click();
}

}  // namespace

// Tree shape: H[ dock0 , V[dock1, dock2] ]. dock0 is a leaf in the left branch
// while the docks to hide live in a *separate* nested splitter — the asymmetric
// shape that strands the survivor under the old setVisible(false) approach.
TEST(DockFullscreenTest, MaximizingALeafCollapsesTheEmptiedSiblingBranch) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);

  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  auto* dock1 = dock0->splitHorizontal();  // H[dock0, dock1]
  ASSERT_NE(dock1, nullptr);
  auto* dock2 = dock1->splitVertical();  // H[dock0, V[dock1, dock2]]
  ASSERT_NE(dock2, nullptr);
  ASSERT_EQ(docker.plotCount(), 3);

  docker.resize(1000, 600);
  docker.show();
  QApplication::processEvents();
  ASSERT_EQ(visibleEmptySplitters(docker), 0) << "precondition: no ghost branches before fullscreen";

  clickFullscreen(dock0);
  QApplication::processEvents();

  // The survivor stays open; its siblings are hidden (closed view)...
  EXPECT_FALSE(dock0->isClosed());
  EXPECT_TRUE(dock1->isClosed());
  EXPECT_TRUE(dock2->isClosed());
  // ...and crucially the emptied V-branch is collapsed, not left occupying blank space.
  EXPECT_EQ(visibleEmptySplitters(docker), 0) << "fullscreen left a ghost (blank) splitter branch";

  clickFullscreen(dock0);  // exit fullscreen
  QApplication::processEvents();

  // Exiting restores every sibling and leaves the tree clean.
  EXPECT_FALSE(dock0->isClosed());
  EXPECT_FALSE(dock1->isClosed());
  EXPECT_FALSE(dock2->isClosed());
  EXPECT_EQ(visibleEmptySplitters(docker), 0) << "exit fullscreen left the tree dirty";
}

namespace {

// The `sizes` attribute of every <DockSplitter> in a saved tab, in document
// order, joined by '|'.
std::string savedSplitterSizes(const PJ::PlotDocker& docker) {
  QDomDocument doc;
  const QDomElement tab = docker.xmlSaveState(doc);
  QStringList sizes;
  const QDomNodeList splitters = tab.elementsByTagName(u"DockSplitter"_s);
  for (int index = 0; index < splitters.count(); ++index) {
    sizes.push_back(splitters.at(index).toElement().attribute(u"sizes"_s));
  }
  return sizes.join(u'|').toStdString();
}

}  // namespace

// Saving while a dock is maximized must store the layout the user will get back
// on exit, not the folded one ("1;0" turns the other areas into thin strips on
// load) — and must not leave fullscreen as a side effect.
TEST(DockFullscreenTest, SaveWhileFullscreenKeepsThePreFullscreenSplitSizes) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);

  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  auto* dock1 = dock0->splitVertical();  // V[dock0, dock1]
  ASSERT_NE(dock1, nullptr);
  auto* dock2 = dock1->splitHorizontal();  // V[dock0, H[dock1, dock2]]
  ASSERT_NE(dock2, nullptr);

  docker.resize(1000, 600);
  docker.show();
  QApplication::processEvents();
  const std::string normal_sizes = savedSplitterSizes(docker);
  ASSERT_EQ(std::count(normal_sizes.begin(), normal_sizes.end(), '|'), 1) << "two splitters expected";

  clickFullscreen(dock0);
  QApplication::processEvents();

  EXPECT_EQ(savedSplitterSizes(docker), normal_sizes);
  EXPECT_EQ(docker.fullscreenDock(), dock0) << "saving must not leave fullscreen";
  EXPECT_TRUE(dock1->isClosed());
  EXPECT_TRUE(dock2->isClosed());
}

namespace {

// The smallest fraction any <DockSplitter> of a saved tab gives one of its
// children; 0 means a collapsed (hidden) branch got serialized.
double smallestSavedSplitFraction(const QDomElement& tab) {
  double smallest = 1.0;
  const QDomNodeList splitters = tab.elementsByTagName(u"DockSplitter"_s);
  for (int index = 0; index < splitters.count(); ++index) {
    for (const QString& fraction : splitters.at(index).toElement().attribute(u"sizes"_s).split(u';')) {
      smallest = std::min(smallest, fraction.toDouble());
    }
  }
  return smallest;
}

}  // namespace

// The context menu's "Split Vertically" (and the placeholder's / PlotWidget's)
// stay reachable on the maximized dock and land in DockWidget::splitInto. The
// new dock changes the outer splitter's child count, so the fullscreen snapshot
// no longer matches it: the split must leave fullscreen first, or the save falls
// back to the folded live sizes ("1;0") and loads back as thin strips.
TEST(DockFullscreenTest, SplittingWhileFullscreenLeavesFullscreenAndSavesASaneLayout) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);

  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  auto* dock1 = dock0->splitVertical();  // V[dock0, dock1]
  ASSERT_NE(dock1, nullptr);
  auto* dock2 = dock1->splitHorizontal();  // V[dock0, H[dock1, dock2]]
  ASSERT_NE(dock2, nullptr);

  docker.resize(1000, 600);
  docker.show();
  QApplication::processEvents();

  clickFullscreen(dock0);
  QApplication::processEvents();
  ASSERT_EQ(docker.fullscreenDock(), dock0);

  auto* dock3 = dock0->splitVertical();  // the context menu's entry point
  ASSERT_NE(dock3, nullptr);
  QApplication::processEvents();

  EXPECT_EQ(docker.fullscreenDock(), nullptr) << "a split leaves fullscreen first";
  EXPECT_FALSE(dock0->toolBar()->isFullscreen()) << "the maximized dock's toolbar must drop its fullscreen state";
  EXPECT_FALSE(dock1->isClosed());
  EXPECT_FALSE(dock2->isClosed());
  EXPECT_EQ(visibleEmptySplitters(docker), 0);

  QDomDocument doc;
  const QDomElement tab = docker.xmlSaveState(doc);
  EXPECT_GT(smallestSavedSplitFraction(tab), 0.05) << "a collapsed branch was saved";

  // Reloading the saved layout (what undo does) must bring every plot back visible.
  ASSERT_TRUE(docker.xmlLoadState(tab));
  QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QApplication::processEvents();
  ASSERT_EQ(docker.plotCount(), 4);
  for (int index = 0; index < docker.plotCount(); ++index) {
    EXPECT_TRUE(docker.plotAt(index)->isVisible()) << "plot " << index;
  }
}

// A layout load (undo/redo, Load Layout) while a dock is maximized rebuilds the
// tab; it must not inherit the old fullscreen bookkeeping.
TEST(DockFullscreenTest, LoadingALayoutWhileFullscreenStartsFromANormalTab) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);

  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  auto* dock1 = dock0->splitVertical();  // V[dock0, dock1]
  ASSERT_NE(dock1, nullptr);
  auto* dock2 = dock1->splitHorizontal();  // V[dock0, H[dock1, dock2]]
  ASSERT_NE(dock2, nullptr);

  docker.resize(1000, 600);
  docker.show();
  QApplication::processEvents();
  QDomDocument doc;
  const QDomElement tab = docker.xmlSaveState(doc);

  clickFullscreen(dock0);
  QApplication::processEvents();
  ASSERT_EQ(docker.fullscreenDock(), dock0);

  ASSERT_TRUE(docker.xmlLoadState(tab));
  QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QApplication::processEvents();

  EXPECT_EQ(docker.fullscreenDock(), nullptr);
  EXPECT_EQ(docker.plotCount(), 3) << "the hidden docks of the old tab must not linger";
  EXPECT_EQ(visibleEmptySplitters(docker), 0);
  EXPECT_GT(smallestSavedSplitFraction(docker.xmlSaveState(doc)), 0.05);
}
