// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The main window keeps its native window when the first GPU widget (the first
// plot's canvas) joins it. Qt recreates the native window of a top-level that
// switches from raster to QRhi composition, which flashes the window on Wayland;
// MainWindow's hidden QRhiWidget makes it composite through QRhi from the start.

#include <gtest/gtest.h>

#include <QPointer>
#include <QRhiWidget>
#include <QTest>
#include <QWindow>

#include "MainWindow.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_widgets/GraphicsApi.h"
#include "support/gui_test_env.h"

namespace {

// Shows a MainWindow built with the GPU plot canvas enabled or disabled, then
// adds a QRhiWidget the way a plot canvas joins it (constructed parentless,
// then inserted), and reports whether the native window survived.
[[nodiscard]] bool nativeWindowSurvivesFirstGpuWidget(bool gpu_canvas) {
  PJ::PlotWidgetBase::setGpuCanvasDisabledOverride(!gpu_canvas);
  PJ::MainWindow window;
  // GuiTestEnvironment disables the GPU canvas for every other case.
  PJ::PlotWidgetBase::setGpuCanvasDisabledOverride(true);
  window.show();
  EXPECT_TRUE(QTest::qWaitForWindowExposed(&window));
  QPointer<QWindow> native = window.windowHandle();
  EXPECT_NE(native, nullptr);

  auto* canvas = new QRhiWidget;
  canvas->setApi(PJ::preferredGraphicsApi());
  canvas->setParent(window.centralWidget());
  canvas->show();
  QCoreApplication::processEvents();

  return native != nullptr && native == window.windowHandle();
}

TEST(MainWindowRhiWindowTest, FirstGpuWidgetKeepsTheNativeWindow) {
  EXPECT_TRUE(nativeWindowSurvivesFirstGpuWidget(/*gpu_canvas=*/true));
}

// The detector itself: without the anchor, Qt does recreate the native window.
// If this starts failing, Qt no longer recreates and the anchor can go.
TEST(MainWindowRhiWindowTest, WithoutTheGpuCanvasQtRecreatesTheNativeWindow) {
  EXPECT_FALSE(nativeWindowSurvivesFirstGpuWidget(/*gpu_canvas=*/false));
}

}  // namespace
