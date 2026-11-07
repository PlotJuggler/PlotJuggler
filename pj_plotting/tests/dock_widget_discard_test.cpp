// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// IDataWidget::aboutToBeDiscarded: the dock calls it, once, right before it schedules the hosted
// widget for deferred deletion, and never when it only hands the widget to another owner.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QEvent>
#include <QPointer>
#include <QWidget>

#include "pj_plotting/DockWidget.h"
#include "pj_runtime/IDataWidget.h"

namespace {

class DiscardProbe : public QWidget, public PJ::IDataWidget {
 public:
  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double /*time*/) override {}
  QDomElement xmlSaveState(QDomDocument& doc) const override {
    return doc.createElement(QStringLiteral("discard_probe"));
  }
  void aboutToBeDiscarded() override {
    ++discards;
    parent_at_discard = parentWidget();
  }

  int discards = 0;
  QWidget* parent_at_discard = nullptr;
};

TEST(DockWidgetDiscard, ClearingTheContentCallsTheHookBeforeTheDeferredDelete) {
  PJ::DockWidget dock;
  auto* probe = new DiscardProbe();
  const QPointer<DiscardProbe> guard(probe);
  dock.adoptObjectWidget(probe);

  dock.setPlaceholderWidget();
  EXPECT_EQ(probe->discards, 1);
  EXPECT_EQ(probe->parent_at_discard, nullptr) << "already taken out of the dock when the hook runs";
  EXPECT_FALSE(guard.isNull()) << "deletion is deferred";
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  EXPECT_TRUE(guard.isNull());
}

TEST(DockWidgetDiscard, HandingTheWidgetToAnotherOwnerDoesNotDiscardIt) {
  PJ::DockWidget dock;
  auto* probe = new DiscardProbe();
  dock.adoptObjectWidget(probe);

  PJ::IDataWidget* released = dock.releaseObjectWidget();
  EXPECT_EQ(released, probe);
  EXPECT_EQ(probe->discards, 0);
  delete probe;
}

}  // namespace
