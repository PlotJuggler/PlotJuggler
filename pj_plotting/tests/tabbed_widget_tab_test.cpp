// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Widget tabs (TabbedPlotWidget::addWidgetTab): hosting, selection semantics,
// close/teardown ordering, the keep-one-plot-tab invariant, and the
// serialization contract (never saved, always survive xmlLoadState).

#include <gtest/gtest.h>
#include <qwt_plot_curve.h>

#include <QApplication>
#include <QColor>
#include <QDomDocument>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPen>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QtGlobal>
#include <string>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
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

TEST(TabbedWidgetTab, TakeWidgetTabPreservesLiveContentWithoutClosingOrChangingHistory) {
  PJ::TabbedPlotWidget tabbed;
  QPointer<QLabel> content(new QLabel(u"live state"_s));
  int closes = 0;
  int vetoes = 0;
  int history_changes = 0;
  QObject::connect(&tabbed, &PJ::TabbedPlotWidget::undoableChange, [&]() { ++history_changes; });
  tabbed.addWidgetTab(u"Toolbox"_s, content, [&]() { ++closes; });
  tabbed.setWidgetTabPreClose(content, [&]() {
    ++vetoes;
    return false;
  });

  QWidget* page_parent = content->parentWidget();
  ASSERT_NE(page_parent, nullptr);
  EXPECT_EQ(tabbed.takeWidgetTab(content), content.data());
  EXPECT_EQ(tabbed.tabCount(), 1);
  EXPECT_EQ(tabbed.currentTab(), tabbed.dockerAt(0));
  EXPECT_EQ(content->parentWidget(), page_parent) << "left for its destination to reparent, once";
  EXPECT_TRUE(content->isHidden());
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  ASSERT_FALSE(content.isNull());
  EXPECT_EQ(content->text(), u"live state"_s);
  EXPECT_EQ(closes, 0);
  EXPECT_EQ(vetoes, 0);
  EXPECT_EQ(history_changes, 0);
  EXPECT_EQ(tabbed.takeWidgetTab(content), nullptr);
  EXPECT_EQ(tabbed.takeWidgetTab(tabbed.dockerAt(0)), nullptr);
  EXPECT_EQ(tabbed.takeWidgetTab(nullptr), nullptr);

  // The same widget can be hosted again with a fresh teardown callback.
  tabbed.addWidgetTab(u"Toolbox"_s, content, [&]() { ++closes; });
  tabbed.closeWidgetTab(content);
  EXPECT_EQ(closes, 1);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  EXPECT_TRUE(content.isNull());
}

TEST(TabbedWidgetTab, TakeInactiveWidgetTabRestoresSizePolicyAndKeepsSelection) {
  PJ::TabbedPlotWidget tabbed;
  auto* content = new QLabel(u"background"_s);
  content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
  const QSizePolicy original_policy = content->sizePolicy();
  tabbed.addWidgetTab(u"Background"_s, content, {});
  auto* foreground = new QWidget;
  tabbed.addWidgetTab(u"Foreground"_s, foreground, {});
  ASSERT_EQ(content->sizePolicy().horizontalPolicy(), QSizePolicy::Ignored);

  ASSERT_EQ(tabbed.takeWidgetTab(content), content);
  EXPECT_EQ(content->sizePolicy(), original_policy);
  EXPECT_TRUE(foreground->isVisibleTo(&tabbed));
  EXPECT_EQ(tabbed.tabCount(), 2);
  delete content;
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

// The labels the tab strip shows, in tab order.
QStringList stripLabels(const PJ::TabbedPlotWidget& tabbed) {
  QStringList labels;
  for (const QFrame* frame : tabbed.findChildren<QFrame*>(u"plotTabFrame"_s)) {
    for (const QLabel* label : frame->findChildren<QLabel*>()) {
      labels << label->text();
    }
  }
  return labels;
}

// A new tab's default name is never one an open tab already shows, also after a restore brought the names back
// (an undo, a layout load) and after a rename; a closed tab's name is not handed out again either.
TEST(TabbedWidgetTab, DefaultTabNameIsNeverAnOpenTabsName) {
  PJ::TabbedPlotWidget tabbed;  // starts with tab1
  tabbed.addTab({})->setStateId(u"tab-second"_s);
  QDomDocument doc;
  const QDomElement saved = tabbed.xmlSaveState(doc, PJ::SnapshotScope::kHistory);
  ASSERT_TRUE(tabbed.xmlLoadState(saved, PJ::RestoreIntent::kHistory));
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  ASSERT_EQ(stripLabels(tabbed), QStringList({u"tab1"_s, u"tab2"_s}));

  EXPECT_EQ(tabbed.addTab({})->name(), u"tab3"_s) << stripLabels(tabbed).join(u',').toStdString();
  tabbed.renameTab(tabbed.addTab({}), u"tab7"_s);
  PJ::PlotDocker* after_rename = tabbed.addTab({});
  EXPECT_EQ(after_rename->name(), u"tab8"_s);
  tabbed.closeTab(after_rename);
  EXPECT_EQ(tabbed.addTab({})->name(), u"tab9"_s) << "a closed tab's name comes back only with an undo";
}

// A layout load starts the numbering again from what it opened: the default tab it replaces uses up no name.
TEST(TabbedWidgetTab, LayoutLoadRestartsTheDefaultTabNames) {
  PJ::TabbedPlotWidget tabbed;  // starts with tab1
  tabbed.renameTab(tabbed.dockerAt(0), u"Signals"_s);
  QDomDocument doc;
  const QDomElement layout = tabbed.xmlSaveState(doc, PJ::SnapshotScope::kFull);
  tabbed.addTab({});
  tabbed.addTab({});
  ASSERT_TRUE(tabbed.xmlLoadState(layout, PJ::RestoreIntent::kReplace));
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  ASSERT_EQ(stripLabels(tabbed), QStringList({u"Signals"_s}));
  EXPECT_EQ(tabbed.addTab({})->name(), u"tab1"_s);
}

TEST(TabbedWidgetTab, RenameTabUpdatesDockerAndStripLabel) {
  PJ::TabbedPlotWidget tabbed;
  PJ::PlotDocker* docker = tabbed.addTab(u"Before"_s);
  ASSERT_TRUE(stripLabels(tabbed).contains(u"Before"_s));

  tabbed.renameTab(docker, u"After"_s);

  EXPECT_EQ(docker->name(), u"After"_s);
  EXPECT_TRUE(stripLabels(tabbed).contains(u"After"_s));
  EXPECT_FALSE(stripLabels(tabbed).contains(u"Before"_s));
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

TEST(TabbedWidgetTab, HistoryExemptTabDoesNotShiftTheSavedCurrentIndex) {
  PJ::TabbedPlotWidget tabbed;
  PJ::PlotDocker* first = tabbed.dockerAt(0);
  ASSERT_NE(first, nullptr);
  first->setHistoryExempt(true);
  tabbed.addTab(u"second"_s);
  PJ::PlotDocker* third = tabbed.addTab(u"third"_s);
  ASSERT_EQ(tabbed.currentTab(), third);  // addTab selects the tab it creates

  QDomDocument doc;
  const QDomElement saved = tabbed.xmlSaveState(doc, PJ::SnapshotScope::kHistory);
  const QDomElement current_tab_index = saved.firstChildElement(u"currentTabIndex"_s);
  ASSERT_FALSE(current_tab_index.isNull());
  // Third docker is current; the first (exempt, unserialized at history scope)
  // docker does not occupy a slot, so the third is the SECOND serialized tab.
  EXPECT_EQ(current_tab_index.attribute(u"index"_s).toInt(), 1);
}

TEST(TabbedWidgetTab, HistoryExemptTabRoundTripsThroughFullLayout) {
  PJ::TabbedPlotWidget tabbed;
  PJ::PlotDocker* owned = tabbed.addTab(u"Assistant view"_s);
  owned->setStateId(u"assistant/run"_s);
  owned->setHistoryExempt(true);
  owned->setOwnerMetadata(u"assistant"_s, u"run"_s);
  QPointer<PJ::PlotDocker> original(owned);

  QDomDocument doc;
  const QDomElement saved = tabbed.xmlSaveState(doc, PJ::SnapshotScope::kFull);
  QDomElement owned_element;
  for (QDomElement tab = saved.firstChildElement(u"Tab"_s); !tab.isNull(); tab = tab.nextSiblingElement(u"Tab"_s)) {
    if (tab.attribute(u"id"_s) == u"assistant/run"_s) {
      owned_element = tab;
      break;
    }
  }
  ASSERT_FALSE(owned_element.isNull());
  EXPECT_EQ(owned_element.attribute(u"history_exempt"_s), u"1"_s);
  EXPECT_EQ(owned_element.attribute(u"owner_plugin"_s), u"assistant"_s);
  EXPECT_EQ(owned_element.attribute(u"tab_id"_s), u"run"_s);

  // The restore reuses the page with the same id: change what it must bring back.
  owned->setHistoryExempt(false);
  owned->setOwnerMetadata(u"other"_s, u"other-run"_s);
  ASSERT_TRUE(tabbed.xmlLoadState(saved, PJ::RestoreIntent::kReplace));
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  EXPECT_EQ(tabbed.currentTab(), original.data());
  PJ::PlotDocker* restored = tabbed.currentTab();
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->stateId(), u"assistant/run"_s);
  EXPECT_TRUE(restored->isHistoryExempt());
  EXPECT_EQ(restored->ownerPlugin(), u"assistant"_s);
  EXPECT_EQ(restored->ownerTabId(), u"run"_s);
}

TEST(TabbedWidgetTab, HistoryRestorePreservesExactExemptPageAndSkipsPersistedCopy) {
  PJ::TabbedPlotWidget tabbed;
  PJ::PlotDocker* exempt = tabbed.addTab(u"Assistant view"_s);
  exempt->setStateId(u"assistant/run"_s);
  exempt->setHistoryExempt(true);
  exempt->setOwnerMetadata(u"assistant"_s, u"run"_s);
  QPointer<PJ::PlotDocker> exempt_guard(exempt);

  QDomDocument full_doc;
  const QDomElement full = tabbed.xmlSaveState(full_doc, PJ::SnapshotScope::kFull);
  QDomDocument history_doc;
  const QDomElement history = tabbed.xmlSaveState(history_doc, PJ::SnapshotScope::kHistory);
  for (QDomElement tab = history.firstChildElement(u"Tab"_s); !tab.isNull(); tab = tab.nextSiblingElement(u"Tab"_s)) {
    EXPECT_NE(tab.attribute(u"id"_s), u"assistant/run"_s);
  }

  exempt->setName(u"Live contents"_s);
  ASSERT_TRUE(tabbed.xmlLoadState(full, PJ::RestoreIntent::kHistory));
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  ASSERT_FALSE(exempt_guard.isNull());
  EXPECT_EQ(exempt_guard->name(), u"Live contents"_s);

  int owned_count = 0;
  for (int i = 0; i < tabbed.dockerCount(); ++i) {
    if (tabbed.dockerAt(i)->stateId() == u"assistant/run"_s) {
      ++owned_count;
      EXPECT_EQ(tabbed.dockerAt(i), exempt_guard.data());
    }
  }
  EXPECT_EQ(owned_count, 1);
}

// Selecting a tab is not an undo step, so a snapshot's current tab is the one shown when it was taken. An undo or
// redo keeps the tab the user is on when it still exists; a layout load, or an undo that removes that tab, selects
// the saved one.
TEST(TabbedWidgetTab, HistoryRestoreKeepsTheTabTheUserIsOn) {
  PJ::TabbedPlotWidget tabbed;
  PJ::PlotDocker* first = tabbed.addTab(u"one"_s);
  first->setStateId(u"tab-one"_s);
  PJ::PlotDocker* second = tabbed.addTab(u"two"_s);
  second->setStateId(u"tab-two"_s);
  tabbed.focusTab(second);
  ASSERT_EQ(tabbed.currentTab(), second);
  QDomDocument doc;
  const QDomElement saved = tabbed.xmlSaveState(doc, PJ::SnapshotScope::kHistory);

  tabbed.focusTab(first);
  ASSERT_TRUE(tabbed.xmlLoadState(saved, PJ::RestoreIntent::kHistory));
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  ASSERT_NE(tabbed.currentTab(), nullptr);
  EXPECT_EQ(tabbed.currentTab()->stateId(), u"tab-one"_s);

  ASSERT_TRUE(tabbed.xmlLoadState(saved, PJ::RestoreIntent::kReplace));
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  ASSERT_NE(tabbed.currentTab(), nullptr);
  EXPECT_EQ(tabbed.currentTab()->stateId(), u"tab-two"_s) << "a layout load selects the saved tab";
}

TEST(TabbedWidgetTab, HistoryRestoreKeepsACurrentWidgetTab) {
  PJ::TabbedPlotWidget tabbed;
  PJ::PlotDocker* plot_tab = tabbed.addTab(u"plots"_s);
  plot_tab->setStateId(u"tab-plots"_s);
  tabbed.focusTab(plot_tab);
  QDomDocument doc;
  const QDomElement saved = tabbed.xmlSaveState(doc, PJ::SnapshotScope::kHistory);

  auto* panel = new QWidget;
  QPointer<QWidget> panel_guard(panel);
  tabbed.addWidgetTab(u"toolbox"_s, panel, [] {});
  ASSERT_EQ(tabbed.currentTab(), nullptr) << "the widget tab is current";
  ASSERT_TRUE(tabbed.xmlLoadState(saved, PJ::RestoreIntent::kHistory));
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  ASSERT_FALSE(panel_guard.isNull());
  EXPECT_EQ(tabbed.currentTab(), nullptr) << "the undo switched away from the widget tab";
  EXPECT_TRUE(panel_guard->isVisibleTo(&tabbed));
}

TEST(TabbedWidgetTab, HistoryRestoreThatRemovesTheCurrentTabSelectsTheSavedOne) {
  PJ::TabbedPlotWidget tabbed;
  PJ::PlotDocker* first = tabbed.addTab(u"one"_s);
  first->setStateId(u"tab-one"_s);
  PJ::PlotDocker* second = tabbed.addTab(u"two"_s);
  second->setStateId(u"tab-two"_s);
  tabbed.focusTab(second);
  QDomDocument doc;
  const QDomElement saved = tabbed.xmlSaveState(doc, PJ::SnapshotScope::kHistory);

  PJ::PlotDocker* added = tabbed.addTab(u"three"_s);
  added->setStateId(u"tab-three"_s);
  tabbed.focusTab(added);
  ASSERT_TRUE(tabbed.xmlLoadState(saved, PJ::RestoreIntent::kHistory));
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  ASSERT_NE(tabbed.currentTab(), nullptr);
  EXPECT_EQ(tabbed.currentTab()->stateId(), u"tab-two"_s);
}

TEST(TabbedWidgetTab, ClosingLastOrdinaryTabCreatesOneWhenExemptTabRemains) {
  PJ::TabbedPlotWidget tabbed;
  PJ::PlotDocker* ordinary = tabbed.dockerAt(0);
  ASSERT_NE(ordinary, nullptr);
  PJ::PlotDocker* exempt = tabbed.addTab(u"Assistant view"_s);
  exempt->setHistoryExempt(true);

  tabbed.closeTab(ordinary);

  EXPECT_EQ(tabbed.dockerCount(), 2);
  int ordinary_count = 0;
  for (int i = 0; i < tabbed.dockerCount(); ++i) {
    PJ::PlotDocker* docker = tabbed.dockerAt(i);
    ordinary_count += docker->isHistoryExempt() ? 0 : 1;
  }
  EXPECT_EQ(ordinary_count, 1);

  QDomDocument history_doc;
  const QDomElement history = tabbed.xmlSaveState(history_doc, PJ::SnapshotScope::kHistory);
  int history_tabs = 0;
  for (QDomElement tab = history.firstChildElement(u"Tab"_s); !tab.isNull(); tab = tab.nextSiblingElement(u"Tab"_s)) {
    ++history_tabs;
  }
  EXPECT_EQ(history_tabs, 1);
}

// The mirror of the rule above: an exempt tab is not part of a history
// snapshot, so closing one takes nothing away from what history serializes and
// must not leave the user with a spare empty tab.
TEST(TabbedWidgetTab, ClosingAHistoryExemptTabDoesNotSpawnAnything) {
  PJ::TabbedPlotWidget tabbed;  // one normal tab
  PJ::PlotDocker* exempt = tabbed.addTab(u"live"_s);
  exempt->setHistoryExempt(true);
  ASSERT_EQ(tabbed.dockerCount(), 2);

  tabbed.closeTab(exempt);

  EXPECT_EQ(tabbed.dockerCount(), 1) << "closing an unserialized tab must not manufacture a replacement";
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

// The tab's XML with every state id removed, to compare layouts across tabs.
QString idFreeXml(const PJ::PlotDocker& docker) {
  QDomDocument doc;
  QDomElement tab = docker.xmlSaveState(doc);
  tab.removeAttribute(u"id"_s);
  for (const QString& tag : {u"DockArea"_s, u"plot"_s}) {
    const QDomNodeList nodes = tab.elementsByTagName(tag);
    for (int index = 0; index < nodes.size(); ++index) {
      nodes.at(index).toElement().removeAttribute(u"id"_s);
    }
  }
  doc.appendChild(tab);
  return doc.toString();
}

TEST(TabbedPlotDuplicate, CopiesLayoutNextToSourceWithFreshIdsAndOneUndoStep) {
  PJ::TabbedPlotWidget tabbed;
  PJ::PlotDocker* source = tabbed.dockerAt(0);
  ASSERT_NE(source, nullptr);
  QDomDocument layout;
  ASSERT_TRUE(
      layout.setContent(uR"(
      <Tab id="src" containers="1"><Container>
      <DockSplitter orientation="|" count="2" sizes="0.25;0.75">
      <DockArea id="a" name="pos"><plot id="p" mode="TimeSeries" line_width="3.0"/></DockArea>
      <DockArea id="b" name="vel"><placeholder/></DockArea>
      </DockSplitter></Container></Tab>)"_s));
  ASSERT_TRUE(source->xmlLoadState(layout.documentElement()));
  source->setName(u"joints"_s);
  source->setHistoryExempt(true);
  source->setOwnerMetadata(u"assistant"_s, u"run"_s);
  ASSERT_EQ(source->plotCount(), 2);
  PJ::PlotWidget* source_plot = source->plotAt(0)->plotWidget();
  ASSERT_NE(source_plot, nullptr);
  PJ::PlotDocker* last = tabbed.addTab(u"last"_s);

  int undo_steps = 0;
  QObject::connect(&tabbed, &PJ::TabbedPlotWidget::undoableChange, [&undo_steps]() { ++undo_steps; });

  PJ::PlotDocker* copy = tabbed.duplicateTab(source);

  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(undo_steps, 1);
  EXPECT_EQ(copy->plotCount(), 2);
  EXPECT_EQ(tabbed.dockerCount(), 3);
  EXPECT_EQ(tabbed.dockerAt(0), source);
  EXPECT_EQ(tabbed.dockerAt(1), copy);
  EXPECT_EQ(tabbed.dockerAt(2), last);
  EXPECT_EQ(tabbed.currentTab(), copy);
  EXPECT_EQ(copy->name(), u"joints (copy)"_s);
  EXPECT_FALSE(copy->isHistoryExempt());
  EXPECT_TRUE(copy->ownerPlugin().isEmpty());

  EXPECT_NE(copy->stateId(), source->stateId());
  PJ::PlotWidget* copy_plot = copy->plotAt(0)->plotWidget();
  ASSERT_NE(copy_plot, nullptr);
  EXPECT_NE(copy_plot, source_plot);
  EXPECT_NE(copy_plot->stateId(), source_plot->stateId());
  EXPECT_NE(copy->plotAt(0)->stateId(), source->plotAt(0)->stateId());

  copy->setHistoryExempt(true);  // the only intended difference was the dropped exempt/owner flags
  copy->setOwnerMetadata(u"assistant"_s, u"run"_s);
  EXPECT_EQ(idFreeXml(*copy), idFreeXml(*source));
}

// A session with data and a tab widget bound to it.
struct DuplicateFixture {
  PJ::SessionManager session;
  PJ::CatalogModel catalog{&session};
  PJ::TabbedPlotWidget tabbed;

  DuplicateFixture() {
    tabbed.setDataServices(&session, &catalog);
  }

  // A dataset from `source`, registered as loaded from `path` when that is not empty; 0 on failure.
  PJ::DatasetId addDataset(const std::string& source, const QString& path = {}) {
    auto domain = session.dataEngine().createTimeDomain(source);
    if (!domain.has_value()) {
      return 0;
    }
    auto dataset =
        session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = source, .time_domain_id = *domain});
    if (!dataset.has_value()) {
      return 0;
    }
    if (!path.isEmpty()) {
      session.setDatasetSourcePath(*dataset, path);
    }
    return *dataset;
  }

  // The catalog key of a new two-sample scalar series, empty on failure.
  QString addSeries(PJ::DatasetId dataset, std::string_view topic) {
    PJ::DataWriter writer = session.dataEngine().createWriter();
    auto handle = writer.registerScalarSeries(dataset, topic, PJ::NumericType::kFloat64);
    if (!handle.has_value()) {
      return {};
    }
    writer.appendScalar(*handle, 0, 0.0);
    writer.appendScalar(*handle, 1'000'000'000, 1.0);
    if (session.commitChunks(writer.flushAll()).empty()) {
      return {};
    }
    for (const auto& curve : catalog.curves()) {
      if (const auto descriptor = catalog.curveDescriptor(curve.name);
          descriptor && descriptor->topic_id == handle->topic_id) {
        return curve.name;
      }
    }
    return {};
  }
};

// The plot's curve keys, comma-joined (a readable gtest failure).
std::string curveNames(const PJ::PlotWidget& plot) {
  QStringList names;
  for (const auto& info : plot.curveList()) {
    names << info.source_name;
  }
  return names.join(u',').toStdString();
}

// The copy shows the source's curves (time series and XY) in the source's colours, not empty plots.
TEST(TabbedPlotDuplicate, CopiesCurvesOnCatalogSeries) {
  DuplicateFixture fixture;
  PJ::TabbedPlotWidget& tabbed = fixture.tabbed;
  const PJ::DatasetId dataset = fixture.addDataset("run.csv");
  ASSERT_NE(dataset, 0U);
  const QStringList keys{fixture.addSeries(dataset, "/sine"), fixture.addSeries(dataset, "/cosine")};
  ASSERT_FALSE(keys[0].isEmpty());
  ASSERT_FALSE(keys[1].isEmpty());

  PJ::PlotDocker* source = tabbed.dockerAt(0);
  QDomDocument layout;
  ASSERT_TRUE(
      layout.setContent(uR"(
      <Tab id="src" containers="1"><Container>
      <DockSplitter orientation="|" count="2" sizes="0.5;0.5">
      <DockArea id="a" name="ts"><plot id="p" mode="TimeSeries"/></DockArea>
      <DockArea id="b" name="xy"><plot id="q" mode="XYPlot"/></DockArea>
      </DockSplitter></Container></Tab>)"_s));
  ASSERT_TRUE(source->xmlLoadState(layout.documentElement()));
  PJ::PlotWidget* source_ts = source->plotAt(0)->plotWidget();
  PJ::PlotWidget* source_xy = source->plotAt(1)->plotWidget();
  ASSERT_NE(source_ts, nullptr);
  ASSERT_NE(source_xy, nullptr);
  ASSERT_NE(source_ts->addCurve(keys[0], QColor(u"#ff0000"_s)), nullptr);
  ASSERT_NE(source_ts->addCurve(keys[1], QColor(u"#00ff00"_s)), nullptr);
  ASSERT_NE(source_xy->addCurveXY(keys[1], keys[0], u"phase"_s, QColor(u"#0000ff"_s)), nullptr);

  int undo_steps = 0;
  QObject::connect(&tabbed, &PJ::TabbedPlotWidget::undoableChange, [&undo_steps]() { ++undo_steps; });

  PJ::PlotDocker* copy = tabbed.duplicateTab(source);

  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(undo_steps, 1);
  ASSERT_EQ(copy->plotCount(), 2);
  for (int index = 0; index < 2; ++index) {
    const PJ::PlotWidget* from = source->plotAt(index)->plotWidget();
    const PJ::PlotWidget* to = copy->plotAt(index)->plotWidget();
    ASSERT_NE(to, nullptr);
    ASSERT_EQ(to->curveList().size(), from->curveList().size()) << "plot " << index;
    auto from_it = from->curveList().begin();
    for (const auto& info : to->curveList()) {
      EXPECT_EQ(info.source_name, from_it->source_name);
      EXPECT_EQ(info.curve->pen().color(), from_it->curve->pen().color());
      ++from_it;
    }
  }
  EXPECT_EQ(idFreeXml(*copy), idFreeXml(*source));
}

// Two plots sharing one id (a hand-edited layout) each keep their own curves in the copy.
TEST(TabbedPlotDuplicate, PlotsSharingAnIdKeepTheirOwnCurves) {
  DuplicateFixture fixture;
  const PJ::DatasetId dataset = fixture.addDataset("run.csv");
  ASSERT_NE(dataset, 0U);
  const std::string first = fixture.addSeries(dataset, "/first").toStdString();
  const std::string second = fixture.addSeries(dataset, "/second").toStdString();
  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  PJ::PlotDocker* source = fixture.tabbed.dockerAt(0);
  QDomDocument layout;
  ASSERT_TRUE(
      layout.setContent(uR"(
      <Tab id="src" containers="1"><Container>
      <DockSplitter orientation="|" count="2" sizes="0.5;0.5">
      <DockArea id="a" name="one"><plot id="p" mode="TimeSeries"/></DockArea>
      <DockArea id="b" name="two"><plot id="p" mode="TimeSeries"/></DockArea>
      </DockSplitter></Container></Tab>)"_s));
  ASSERT_TRUE(source->xmlLoadState(layout.documentElement()));
  ASSERT_EQ(source->plotCount(), 2);
  ASSERT_NE(source->plotAt(0)->plotWidget()->addCurve(QString::fromStdString(first)), nullptr);
  ASSERT_NE(source->plotAt(1)->plotWidget()->addCurve(QString::fromStdString(second)), nullptr);

  PJ::PlotDocker* copy = fixture.tabbed.duplicateTab(source);

  ASSERT_NE(copy, nullptr);
  ASSERT_EQ(copy->plotCount(), 2);
  EXPECT_EQ(curveNames(*copy->plotAt(0)->plotWidget()), first);
  EXPECT_EQ(curveNames(*copy->plotAt(1)->plotWidget()), second);
}

// A hidden marker scope still waiting for its dataset, whose saved id now names another source's dataset, must not
// take that dataset's path in the copy: the path would then never resolve, and a layout save would persist it.
TEST(TabbedPlotDuplicate, PendingMarkerScopeGetsNoForeignDatasetPath) {
  DuplicateFixture fixture;
  const PJ::DatasetId live = fixture.addDataset("a.csv", u"/data/a.csv"_s);
  ASSERT_NE(live, 0U);
  ASSERT_NE(fixture.addDataset("b.csv", u"/data/b.csv"_s), 0U);
  PJ::PlotDocker* source = fixture.tabbed.dockerAt(0);
  QDomDocument layout;
  ASSERT_TRUE(
      layout.setContent(uR"(
      <Tab id="src" containers="1"><Container>
      <DockArea id="a" name="one"><plot id="p" mode="TimeSeries">
      <marker_scope dataset_id="%1" dataset_source="other.csv" scope="dataset" visible="false"/>
      </plot></DockArea></Container></Tab>)"_s.arg(live)));
  ASSERT_TRUE(source->xmlLoadState(layout.documentElement()));

  PJ::PlotDocker* copy = fixture.tabbed.duplicateTab(source);

  ASSERT_NE(copy, nullptr);
  QDomDocument doc;
  const QDomElement scope = copy->xmlSaveState(doc).elementsByTagName(u"marker_scope"_s).at(0).toElement();
  ASSERT_FALSE(scope.isNull()) << "the pending scope was dropped";
  EXPECT_EQ(scope.attribute(u"dataset_source"_s), u"other.csv"_s);
  EXPECT_FALSE(scope.hasAttribute(u"dataset_path"_s)) << scope.attribute(u"dataset_path"_s).toStdString();
}

TEST(TabbedPlotDuplicate, IgnoresWidgetTabsAndForeignDockers) {
  PJ::TabbedPlotWidget tabbed;
  PJ::PlotDocker foreign(u"foreign"_s);
  EXPECT_EQ(tabbed.duplicateTab(&foreign), nullptr);
  EXPECT_EQ(tabbed.duplicateTab(nullptr), nullptr);
  EXPECT_EQ(tabbed.dockerCount(), 1);
}

// The tab's right-click menu is one "Duplicate tab" entry. Desktop shows it modally and the browser as a popup, so the
// choice has to act through the action's signal: nothing may read a result after the menu returns.
TEST(TabbedPlotDuplicate, TabContextMenuDuplicatesTheTab) {
  PJ::TabbedPlotWidget tabbed;
  tabbed.resize(600, 400);
  tabbed.show();
  const QList<QFrame*> frames = tabbed.findChildren<QFrame*>(u"plotTabFrame"_s);
  ASSERT_EQ(frames.size(), 1);
  PJ::PlotDocker* source = tabbed.dockerAt(0);

  QStringList entries;
  QTimer choose;
  choose.setInterval(10);
  QObject::connect(&choose, &QTimer::timeout, [&]() {
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    if (menu == nullptr) {
      return;
    }
    for (QAction* action : menu->actions()) {
      entries.push_back(action->text());
    }
    menu->actions().value(0)->trigger();
    menu->close();
  });
  choose.start();
  emit frames.front()->customContextMenuRequested(QPoint(4, 4));
  choose.stop();

  EXPECT_EQ(entries, QStringList{u"Duplicate tab"_s});
  ASSERT_EQ(tabbed.dockerCount(), 2);
  EXPECT_EQ(tabbed.dockerAt(0), source);
  EXPECT_EQ(tabbed.dockerAt(1), tabbed.currentTab());
}

// A tab whose content fails to load must not leave the live page it would have reused renamed or re-owned.
TEST(TabbedWidgetTab, FailedTabLoadKeepsTheReusedPageMetadata) {
  PJ::TabbedPlotWidget tabbed;
  PJ::PlotDocker* page = tabbed.currentTab();
  ASSERT_NE(page, nullptr);
  page->setStateId(u"page"_s);
  tabbed.renameTab(page, u"live"_s);

  QDomDocument doc;
  ASSERT_TRUE(
      static_cast<bool>(doc.setContent(
          u"<tabbed_widget id=\"\" name=\"main\"><Tab id=\"page\" tab_name=\"rejected\" owner_plugin=\"x\" "
          u"tab_id=\"y\">"
          u"<NotAContainer/></Tab><currentTabIndex index=\"0\"/></tabbed_widget>"_s)));
  EXPECT_FALSE(tabbed.xmlLoadState(doc.documentElement(), PJ::RestoreIntent::kReplace));
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

  ASSERT_EQ(tabbed.dockerAt(0), page);
  EXPECT_EQ(page->name(), u"live"_s);
  EXPECT_TRUE(page->ownerPlugin().isEmpty());
}

// Sends a left-button drag of `frame` to the strip x position `to_x` (in the frame's parent coordinates).
void dragTab(QFrame* frame, int to_x) {
  QWidget* strip = frame->parentWidget();
  const QPoint start = frame->rect().center();
  const QPoint end = frame->mapFrom(strip, QPoint(to_x, frame->geometry().center().y()));
  const auto send = [frame](QEvent::Type type, QPoint local, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, local, frame->mapToGlobal(local), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(frame, &event);
  };
  send(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
  send(QEvent::MouseMove, (start + end) / 2, Qt::NoButton, Qt::LeftButton);
  send(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
  send(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
}

QStringList savedTabNames(const PJ::TabbedPlotWidget& tabbed) {
  QDomDocument doc;
  QStringList names;
  const QDomElement area = tabbed.xmlSaveState(doc);
  for (QDomElement tab = area.firstChildElement(u"Tab"_s); !tab.isNull(); tab = tab.nextSiblingElement(u"Tab"_s)) {
    names.push_back(tab.attribute(u"tab_name"_s));
  }
  return names;
}

TEST(TabbedPlotReorder, DraggingATabMovesItAndSavesTheNewOrderAsOneUndoStep) {
  PJ::TabbedPlotWidget tabbed;
  tabbed.renameTab(tabbed.dockerAt(0), u"a"_s);
  PJ::PlotDocker* b = tabbed.addTab(u"b"_s);
  PJ::PlotDocker* c = tabbed.addTab(u"c"_s);
  tabbed.resize(800, 400);
  tabbed.show();
  QCoreApplication::processEvents();
  const QList<QFrame*> frames = tabbed.findChildren<QFrame*>(u"plotTabFrame"_s);
  ASSERT_EQ(frames.size(), 3);
  QFrame* frame_a = frames.at(0);
  QFrame* frame_c = frames.at(2);
  int undo_steps = 0;
  QObject::connect(&tabbed, &PJ::TabbedPlotWidget::undoableChange, [&undo_steps]() { ++undo_steps; });

  dragTab(frame_c, frame_a->geometry().left() + 2);

  EXPECT_EQ(tabbed.dockerAt(0), c);
  EXPECT_EQ(tabbed.dockerAt(2), b);
  EXPECT_EQ(savedTabNames(tabbed), (QStringList{u"c"_s, u"a"_s, u"b"_s}));
  EXPECT_LT(frame_c->geometry().left(), frame_a->geometry().left());
  EXPECT_EQ(undo_steps, 1);

  // A drag that crosses no neighbour's midpoint moves nothing and is no undo step.
  dragTab(frame_c, frame_c->geometry().center().x() + 15);
  EXPECT_EQ(undo_steps, 1);
}

TEST(TabbedPlotReorder, WidgetTabsAreNotDraggable) {
  PJ::TabbedPlotWidget tabbed;
  PJ::PlotDocker* plot = tabbed.dockerAt(0);
  auto* content = new QLabel(u"pinned"_s);
  tabbed.addWidgetTab(u"Toolbox"_s, content, {});
  tabbed.resize(800, 400);
  tabbed.show();
  QCoreApplication::processEvents();
  const QList<QFrame*> frames = tabbed.findChildren<QFrame*>(u"plotTabFrame"_s);
  ASSERT_EQ(frames.size(), 2);

  dragTab(frames.at(1), frames.at(0)->geometry().left() + 2);

  EXPECT_EQ(tabbed.dockerAt(0), plot);
  EXPECT_EQ(tabbed.dockerAt(1), nullptr);
}

}  // namespace
