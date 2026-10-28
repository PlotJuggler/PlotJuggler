// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// The app's three GPU widget families in one Qt-Advanced-Docking manager, fed
// through public APIs only: a PlotWidget, a Scene3D view showing a point cloud
// and the Scene2D media viewer showing an image. Shared by rhi_mixed_dock_test
// and mixed_layout_benchmark.

#include <DockManager.h>
#include <DockWidget.h>

#include <QColor>
#include <QMainWindow>
#include <QString>
#include <memory>

#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TransformService.h"
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"

namespace pj_mixed_layout {

/// Every curve createPlot() draws.
inline const QColor kCurveColour(230, 40, 40);

/// A main window and its dock manager, plus the data the widgets show. Nothing
/// is shown: the caller creates and docks the widgets it wants, sets any test
/// property on them, then shows window(). A data setup failure is a fixture bug
/// and aborts (qFatal). Members die in reverse order, so the window (and every
/// widget pointing at the session) goes before the session.
class MixedLayout {
 public:
  MixedLayout();
  ~MixedLayout();
  MixedLayout(const MixedLayout&) = delete;
  MixedLayout& operator=(const MixedLayout&) = delete;

  [[nodiscard]] QMainWindow& window() {
    return *window_;
  }
  [[nodiscard]] ads::CDockManager& manager() {
    return *manager_;
  }
  [[nodiscard]] PJ::SessionManager& session() {
    return session_;
  }

  /// Docks `content` (reparented to the dock) as `name` in `area`.
  ads::CDockWidget* addDock(const QString& name, QWidget* content, ads::DockWidgetArea area);

  /// A plot of `curves` series of `samples` points each, from a dataset of its
  /// own, every one 3 px wide in kCurveColour, with no grid or legend, zoomed
  /// to fit. Forces the GPU canvas (PlotWidgetBase::setGpuCanvasDisabledOverride),
  /// a process-wide setting.
  PJ::PlotWidget* createPlot(int curves = 1, int samples = 20);

  /// A Scene3D dock showing a solid-green cube of `cloud_side`^3 points
  /// (0.1 m sprites, frame "map", t = 100 ns) with no grid or axes, bound to the
  /// session's TransformService as the app binds it.
  PJ::Scene3DDockWidget* createScene3D(int cloud_side = 8);

  /// A Scene2D dock showing a 64x64 rgba8 image at t = 100 ns: red top half,
  /// blue bottom half. The frame is decoded asynchronously after the viewer shows.
  /// `parent` is set at construction, as the app's dock factory does.
  PJ::Scene2DDockWidget* createMedia(QWidget* parent = nullptr);

 private:
  PJ::SessionManager session_;
  PJ::CatalogModel catalog_{&session_};
  PJ::TransformService transforms_{session_};
  std::unique_ptr<QMainWindow> window_;
  ads::CDockManager* manager_ = nullptr;  // owned by window_
};

}  // namespace pj_mixed_layout
