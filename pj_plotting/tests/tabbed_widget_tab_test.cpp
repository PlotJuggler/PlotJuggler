// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Widget tabs (TabbedPlotWidget::addWidgetTab): hosting, selection semantics,
// close/teardown ordering, the keep-one-plot-tab invariant, and the
// serialization contract (never saved, always survive xmlLoadState).

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QFrame>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QtGlobal>

#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/TabbedPlotWidget.h"
using namespace Qt::StringLiterals;

namespace {

TEST(TabbedWidgetTab, HostsWidgetAsCurrentTabWithNullDocker) {
  PJ::TabbedPlotWidget tabbed;  // ctor adds one plot tab
  ASSERT_EQ(tabbed.dockerCount(), 1);
  ASSERT_EQ(tabbed.tabCount(), 1);

  auto* content = new QLabel(u"pinned"_s);
  int current_changed_count = 0;
  PJ::PlotDocker* last_current = nullptr;
  QObject::connect(&tabbed, &PJ::TabbedPlotWidget::currentTabChanged, [&](PJ::PlotDocker* docker) {
    ++current_changed_count;
    last_current = docker;
  });

  tabbed.addWidgetTab(u"Toolbox"_s, content, {});

  // The widget tab counts as a tab but not as a docker, and is selected.
  EXPECT_EQ(tabbed.tabCount(), 2);
  EXPECT_EQ(tabbed.dockerCount(), 1);
  EXPECT_NE(tabbed.dockerAt(0), nullptr);
  EXPECT_EQ(tabbed.dockerAt(1), nullptr);
  EXPECT_EQ(tabbed.currentTab(), nullptr);
  EXPECT_TRUE(content->isVisibleTo(&tabbed));
  EXPECT_EQ(current_changed_count, 1);
  EXPECT_EQ(last_current, nullptr);
}

TEST(TabbedWidgetTab, CloseRunsOnCloseBeforeDeletionExactlyOnce) {
  PJ::TabbedPlotWidget tabbed;
  QPointer<QLabel> content(new QLabel(u"pinned"_s));
  int close_count = 0;
  bool content_alive_during_close = false;
  tabbed.addWidgetTab(u"Toolbox"_s, content, [&]() {
    ++close_count;
    content_alive_during_close = !content.isNull();
  });

  tabbed.closeWidgetTab(content);
  EXPECT_EQ(close_count, 1);
  EXPECT_TRUE(content_alive_during_close);
  EXPECT_EQ(tabbed.tabCount(), 1);

  // Deletion is deferred (deleteLater); after the event loop drains, gone.
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  EXPECT_TRUE(content.isNull());

  // A second close of the vanished widget is a no-op.
  tabbed.closeWidgetTab(content);
  EXPECT_EQ(close_count, 1);
}

TEST(TabbedWidgetTab, CloseWidgetTabLeavesPlotTabIntactAndSelected) {
  PJ::TabbedPlotWidget tabbed;
  auto* content = new QLabel(u"pinned"_s);
  tabbed.addWidgetTab(u"Toolbox"_s, content, {});
  ASSERT_EQ(tabbed.tabCount(), 2);

  tabbed.closeWidgetTab(content);
  EXPECT_EQ(tabbed.tabCount(), 1);
  EXPECT_EQ(tabbed.dockerCount(), 1);
  // Selection fell back to the surviving plot tab.
  EXPECT_EQ(tabbed.currentTab(), tabbed.dockerAt(0));
}

TEST(TabbedWidgetTab, ClosingLastPlotTabSpawnsFreshOneDespiteWidgetTab) {
  PJ::TabbedPlotWidget tabbed;
  QPointer<QLabel> content(new QLabel(u"pinned"_s));
  tabbed.addWidgetTab(u"Toolbox"_s, content, {});
  ASSERT_EQ(tabbed.dockerCount(), 1);

  // Close the only PLOT tab via its frame's X button (first tab frame
  // created). A widget tab must not satisfy the keep-one-open invariant:
  // the workspace must always serialize at least one <Tab>.
  const auto frames = tabbed.findChildren<QFrame*>(u"plotTabFrame"_s);
  ASSERT_GE(frames.size(), 2);
  auto* plot_close = frames.first()->findChild<QPushButton*>();
  ASSERT_NE(plot_close, nullptr);
  plot_close->click();

  EXPECT_EQ(tabbed.dockerCount(), 1);  // a fresh plot tab was spawned
  EXPECT_EQ(tabbed.tabCount(), 2);
  EXPECT_FALSE(content.isNull());

  QDomDocument doc;
  const QDomElement saved = tabbed.xmlSaveState(doc);
  EXPECT_FALSE(saved.firstChildElement(u"Tab"_s).isNull());
}

TEST(TabbedWidgetTab, WidgetTabNameReadableWritableAndSurvivesRestore) {
  PJ::TabbedPlotWidget tabbed;
  auto* content = new QLabel(u"pinned"_s);
  tabbed.addWidgetTab(u"Toolbox"_s, content, {});
  EXPECT_EQ(tabbed.widgetTabName(content), u"Toolbox"_s);

  // Rename (same in-place mechanism as plot tabs; owner reads it back for
  // persistence).
  tabbed.setWidgetTabName(content, u"My FFT"_s);
  EXPECT_EQ(tabbed.widgetTabName(content), u"My FFT"_s);

  // The custom name rides the preserved frame through a restore.
  QDomDocument doc;
  const QDomElement saved = tabbed.xmlSaveState(doc);
  ASSERT_TRUE(tabbed.xmlLoadState(saved));
  EXPECT_EQ(tabbed.widgetTabName(content), u"My FFT"_s);

  // A docker page is not a widget tab: the accessors ignore it.
  EXPECT_TRUE(tabbed.widgetTabName(tabbed.dockerAt(0)).isEmpty());
}

TEST(TabbedWidgetTab, SerializerSkipsWidgetTabsAndLoadPreservesThem) {
  PJ::TabbedPlotWidget tabbed;
  tabbed.addTab(u"plots2"_s);
  QPointer<QLabel> content(new QLabel(u"pinned"_s));
  tabbed.addWidgetTab(u"Toolbox"_s, content, {});
  ASSERT_EQ(tabbed.dockerCount(), 2);
  ASSERT_EQ(tabbed.tabCount(), 3);

  // Save: only the two docker tabs serialize.
  QDomDocument doc;
  const QDomElement saved = tabbed.xmlSaveState(doc);
  int tab_elements = 0;
  for (QDomElement tab = saved.firstChildElement(u"Tab"_s); !tab.isNull(); tab = tab.nextSiblingElement(u"Tab"_s)) {
    ++tab_elements;
  }
  EXPECT_EQ(tab_elements, 2);

  // Load (the undo/layout restore path): docker tabs are rebuilt, the live
  // widget tab survives and lands after them.
  ASSERT_TRUE(tabbed.xmlLoadState(saved));
  EXPECT_EQ(tabbed.dockerCount(), 2);
  EXPECT_EQ(tabbed.tabCount(), 3);
  EXPECT_NE(tabbed.dockerAt(0), nullptr);
  EXPECT_NE(tabbed.dockerAt(1), nullptr);
  EXPECT_FALSE(content.isNull());

  // The preserved widget tab is still functional: it can be focused...
  tabbed.focusWidgetTab(content);
  EXPECT_EQ(tabbed.currentTab(), nullptr);
  // ...and closed.
  tabbed.closeWidgetTab(content);
  EXPECT_EQ(tabbed.tabCount(), 2);
}

// A pinned toolbox tab whose plugin has work in flight must be able to decline
// its own close, so the host can ask "cancel the download and close?" first.
TEST(TabbedWidgetTab, WidgetTabPreCloseCanVetoClose) {
  PJ::TabbedPlotWidget tabbed;
  auto* content = new QLabel(u"pinned"_s);
  bool torn_down = false;
  tabbed.addWidgetTab(u"Panel"_s, content, [&torn_down]() { torn_down = true; });
  tabbed.setWidgetTabPreClose(content, []() { return false; });

  tabbed.closeWidgetTab(content);

  EXPECT_FALSE(torn_down);
  // Declining leaves the tab set untouched — in particular the veto is
  // consulted before the keep-one-plot-tab spawn, which would otherwise add a
  // tab for a close that never happened.
  EXPECT_EQ(tabbed.widgetTabName(content), u"Panel"_s);
  EXPECT_EQ(tabbed.tabCount(), 2);
  EXPECT_EQ(tabbed.dockerCount(), 1);

  // The veto survives a declined attempt, so the next close asks again.
  tabbed.closeWidgetTab(content);
  EXPECT_FALSE(torn_down);
  EXPECT_EQ(tabbed.tabCount(), 2);
}

// Shutdown and layout replacement must not be refusable.
TEST(TabbedWidgetTab, ForcedCloseIgnoresVeto) {
  PJ::TabbedPlotWidget tabbed;
  auto* content = new QLabel(u"pinned"_s);
  bool torn_down = false;
  tabbed.addWidgetTab(u"Panel"_s, content, [&torn_down]() { torn_down = true; });
  tabbed.setWidgetTabPreClose(content, []() { return false; });

  tabbed.closeWidgetTabForced(content);

  EXPECT_TRUE(torn_down);
  EXPECT_EQ(tabbed.tabCount(), 1);
}

// Absent pre-close keeps the plain close behavior.
TEST(TabbedWidgetTab, WidgetTabWithoutPreCloseClosesNormally) {
  PJ::TabbedPlotWidget tabbed;
  auto* content = new QLabel(u"pinned"_s);
  bool torn_down = false;
  tabbed.addWidgetTab(u"Panel"_s, content, [&torn_down]() { torn_down = true; });

  tabbed.closeWidgetTab(content);

  EXPECT_TRUE(torn_down);
  EXPECT_EQ(tabbed.tabCount(), 1);
}

TEST(TabbedWidgetTab, PreCloseThatClosesTheTabItselfDoesNotTearDownTwice) {
  PJ::TabbedPlotWidget tabbed;
  QPointer<QLabel> content(new QLabel(u"pinned"_s));
  int close_count = 0;
  tabbed.addWidgetTab(u"Panel"_s, content, [&close_count]() { ++close_count; });
  // Stands in for a confirmation that runs a modal event loop: the tab (and its
  // frame) can be gone by the time the callback returns, and the accepted close
  // must not run a second teardown on the freed entry.
  tabbed.setWidgetTabPreClose(content, [&tabbed, content]() {
    tabbed.closeWidgetTabForced(content);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return true;
  });

  tabbed.closeWidgetTab(content);

  EXPECT_EQ(close_count, 1);
  EXPECT_EQ(tabbed.tabCount(), 1);
  EXPECT_TRUE(content.isNull());
}

}  // namespace
