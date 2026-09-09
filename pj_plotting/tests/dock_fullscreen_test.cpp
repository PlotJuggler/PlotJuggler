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
#include <QPushButton>

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
