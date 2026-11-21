// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QPointer>
#include <QTemporaryDir>
#include <memory>

#include "MainWindow.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/IDataWidget.h"
#include "pj_widgets/VisualizationKind.h"

namespace {

using namespace Qt::StringLiterals;

// A dock's replaced content is parentless and only deleted later. When no event loop runs between the replacement and
// the window's destruction (a layout loaded right before quitting, a test), teardown must still delete it before the
// session goes: a Scene2D image layer's decode thread would otherwise keep reading the destroyed object store.
TEST(MainWindowTeardown, DiscardedDockContentDiesBeforeTheSession) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  auto window = std::make_unique<PJ::MainWindow>(extensions_dir.path());
  auto* tabs = window->findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  ASSERT_NE(tabs, nullptr);
  PJ::PlotDocker* docker = tabs->addTab(u"states"_s);
  ASSERT_NE(docker, nullptr);
  PJ::DockWidget* dock = docker->plotAt(0);
  ASSERT_NE(dock, nullptr);
  emit docker->objectFamilyRequested(dock, PJ::VisualizationKind::kStateTransitions);
  ASSERT_NE(dock->objectWidget(), nullptr);
  const QPointer<QWidget> discarded = dock->objectWidget()->widget();

  dock->setPlaceholderWidget();
  ASSERT_FALSE(discarded.isNull()) << "the replaced content is deleted later, not at once";

  window.reset();
  EXPECT_TRUE(discarded.isNull()) << "teardown left the discarded content alive past the session";
}

}  // namespace
