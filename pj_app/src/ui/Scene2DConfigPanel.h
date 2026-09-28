// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

#include "pj_datastore/object_store.hpp"

namespace PJ {

class SceneDockWidget;
class LayerListView;
class ConfigPanelHost;
class DualOptionsWidget;

// Right-sidepanel content for the active Scene2DDockWidget (or any
// SceneDockWidget). A thin reactive shell: it reuses the generic
// pj_widgets controls (LayerListView for the reorderable layer list,
// ConfigPanelHost for the selected layer's options) and binds them to the
// dock's backend-agnostic SceneDockWidget signals/accessors. The dock owns
// the truth (which layers, their order, visibility); per-layer parameters are
// owned by each ISceneLayer. The panel translates user input into dock slots.
//
// This mirrors Scene3DConfigPanel but talks only to the shared SceneDockWidget
// base, so it works for the 2D family and could host any future family. The one
// 2D-specific control, the view rotation selector, shows only for a Scene2DDockWidget.
class Scene2DConfigPanel : public QWidget {
  Q_OBJECT
 public:
  explicit Scene2DConfigPanel(QWidget* parent = nullptr);
  ~Scene2DConfigPanel() override = default;

  // Attach to a dock; pass nullptr to detach. Safe to call repeatedly.
  void bindDock(SceneDockWidget* dock);

 public slots:
  // Re-tint the layer-list row glyphs through the active theme ink. Wired by
  // MainWindow through its stylesheetChanged signal, like Scene3DConfigPanel.
  void onStylesheetChanged(QString theme);

 private:
  void disconnectDock();
  void rebuildList();
  void updateConfigPane();
  /// Mirrors the dock's rotation into the selector (restore/undo included).
  void syncRotation(int degrees);

  LayerListView* list_ = nullptr;
  // Band + selector, shown only when the bound dock is a Scene2DDockWidget.
  QWidget* rotation_section_ = nullptr;
  DualOptionsWidget* rotation_ = nullptr;
  ConfigPanelHost* config_host_ = nullptr;
  QPointer<SceneDockWidget> dock_;
  QString theme_;
};

}  // namespace PJ
