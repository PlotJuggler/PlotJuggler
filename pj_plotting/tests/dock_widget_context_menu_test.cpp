// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The DockWidget event filter intercepts QEvent::ContextMenu on the hosted
// object widget and its children. The forward-first contract: the content
// widget's own contextMenuEvent gets the first shot (re-sent under the
// forwarding_context_menu_ guard so this filter passes it through instead of
// recursing); only an IGNORED event falls back to the dock's standard menu.
// A regression here either kills every content widget's own menu (e.g. the 3D
// view's frame-gizmo menu) host-wide, or re-enters the filter forever.

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDomDocument>
#include <QDomElement>
#include <QMenu>
#include <QPoint>
#include <QString>
#include <QTimer>
#include <QWidget>

#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_runtime/IDataWidget.h"

namespace {

// Minimal object widget that records context-menu delivery and accepts or
// ignores on command.
class MenuProbeWidget : public QWidget, public PJ::IDataWidget {
 public:
  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double /*time*/) override {}
  bool tryAcceptObjectTopic(
      PJ::ObjectTopicId /*topic_id*/, PJ::sdk::BuiltinObjectType /*object_type*/, const QString& /*title*/) override {
    return false;
  }
  QDomElement xmlSaveState(QDomDocument& doc) const override {
    return doc.createElement(QStringLiteral("menu_probe"));
  }

  int context_menu_hits = 0;
  bool accept_next = false;

 protected:
  void contextMenuEvent(QContextMenuEvent* event) override {
    ++context_menu_hits;
    if (accept_next) {
      event->accept();
    } else {
      event->ignore();
    }
  }
};

QContextMenuEvent makeMenuEvent(const QWidget& target) {
  const QPoint pos(5, 5);
  return QContextMenuEvent(QContextMenuEvent::Mouse, pos, target.mapToGlobal(pos));
}

TEST(DockWidgetContextMenu, ContentWidgetGetsFirstShotAndAcceptSuppressesDockMenu) {
  PJ::DockWidget dock;
  auto* probe = new MenuProbeWidget();
  dock.adoptObjectWidget(probe);
  dock.resize(200, 200);

  probe->accept_next = true;
  QContextMenuEvent event = makeMenuEvent(*probe);
  event.ignore();
  QApplication::sendEvent(probe, &event);

  // Exactly one delivery (the guarded re-send — no re-entrant loop), and the
  // original event is swallowed as accepted, so no dock menu appeared (a dock
  // menu would have blocked in exec() and tripped the watchdog below).
  EXPECT_EQ(probe->context_menu_hits, 1);
  EXPECT_TRUE(event.isAccepted());
}

TEST(DockWidgetContextMenu, IgnoredEventFallsThroughToDockMenu) {
  PJ::DockWidget dock;
  auto* probe = new MenuProbeWidget();
  dock.adoptObjectWidget(probe);
  dock.resize(200, 200);

  // The dock's standard menu blocks in QMenu::exec() when the platform can
  // show it; the watchdog closes it so the test never hangs. Whether the popup
  // materializes is platform-dependent (Wayland refuses a popup for an unshown
  // parent), so the asserted contract is delivery + consumption, not pixels.
  QTimer watchdog;
  watchdog.setInterval(50);
  QObject::connect(&watchdog, &QTimer::timeout, []() {
    if (QWidget* popup = QApplication::activePopupWidget(); popup != nullptr) {
      popup->close();
    }
  });
  watchdog.start();

  probe->accept_next = false;
  QContextMenuEvent event = makeMenuEvent(*probe);
  event.ignore();
  QApplication::sendEvent(probe, &event);

  EXPECT_EQ(probe->context_menu_hits, 1)
      << "the content widget still gets exactly one first shot (no re-entrant filter loop)";
  EXPECT_TRUE(event.isAccepted()) << "the filter consumes the original event after taking the fallback path";
}

TEST(DockWidgetContextMenu, OpenSettingsRequestsTheClickedDocksSettings) {
  PJ::DockWidget dock;
  auto* probe = new MenuProbeWidget();
  dock.adoptObjectWidget(probe);
  dock.resize(200, 200);
  dock.show();
  PJ::DockWidget* requested = nullptr;
  QObject::connect(
      &dock, &PJ::DockWidget::settingsPanelToggleRequested, [&](PJ::DockWidget* target) { requested = target; });
  QTimer click;
  click.setInterval(10);
  QObject::connect(&click, &QTimer::timeout, [&]() {
    if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
      for (auto* action : menu->actions()) {
        if (action->text() == QStringLiteral("Open Settings")) {
          action->trigger();
          break;
        }
      }
      menu->close();
    }
  });
  click.start();
  auto event = makeMenuEvent(*probe);
  QApplication::sendEvent(probe, &event);
  EXPECT_EQ(requested, &dock);
}

TEST(DockWidgetContextMenu, SettingsLabelTracksVisibilityForExistingAndNewDocks) {
  PJ::PlotDocker docker(QStringLiteral("settings"));
  bool settings_open = false;
  int toggle_requests = 0;
  QObject::connect(&docker, &PJ::PlotDocker::settingsPanelToggleRequested, [&](PJ::DockWidget*) { ++toggle_requests; });
  docker.setConfigurationPanelVisibilityQuery([&]() { return settings_open; });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  for (int pass = 0; pass < 2; ++pass) {
    auto* probe = new MenuProbeWidget();
    dock->adoptObjectWidget(probe);
    docker.resize(400, 300);
    docker.show();
    const QString expected = settings_open ? QStringLiteral("Close Settings") : QStringLiteral("Open Settings");
    bool found = false;
    QTimer inspect;
    inspect.setInterval(10);
    QObject::connect(&inspect, &QTimer::timeout, [&]() {
      if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
        for (auto* action : menu->actions()) {
          if (action->text() == expected) {
            found = true;
            EXPECT_TRUE(action->isEnabled());
            action->trigger();
          }
        }
        menu->close();
      }
    });
    inspect.start();
    auto event = makeMenuEvent(*probe);
    QApplication::sendEvent(probe, &event);
    EXPECT_TRUE(found) << expected.toStdString();
    EXPECT_EQ(toggle_requests, pass + 1);
    inspect.stop();
    settings_open = true;
    dock = dock->splitVertical();
    ASSERT_NE(dock, nullptr);
  }
}

}  // namespace
