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

// Right-side-panel page for the active scene dock: LayerListView and ConfigPanelHost bound to the SceneDockWidget
// base's signals and accessors. The dock owns which layers exist, their order and visibility; each ISceneLayer owns
// its parameters; the panel only turns gestures into dock calls. The one 2D-specific control, the view rotation
// selector, shows only for a Scene2DDockWidget.
class Scene2DConfigPanel : public QWidget {
  Q_OBJECT
 public:
  explicit Scene2DConfigPanel(QWidget* parent = nullptr);
  ~Scene2DConfigPanel() override = default;

  // nullptr detaches. Safe to call repeatedly.
  void bindDock(SceneDockWidget* dock);

 public slots:
  // Re-tints the layer-list glyphs with the theme ink; MainWindow calls it on stylesheetChanged.
  void onStylesheetChanged(QString theme);

 private:
  void disconnectDock();
  void rebuildList();
  void updateConfigPane();
  /// Mirrors the dock's rotation into the selector (restore/undo included).
  void syncRotation(int degrees);

  LayerListView* list_ = nullptr;
  QWidget* rotation_section_ = nullptr;
  DualOptionsWidget* rotation_ = nullptr;
  ConfigPanelHost* config_host_ = nullptr;
  QPointer<SceneDockWidget> dock_;
  QString theme_;
};

}  // namespace PJ
