// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "ui/Scene2DConfigPanel.h"

#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#include "pj_scene2d_widgets/media_viewer_widget.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_scene_common/scene_layer.h"
#include "pj_widgets/ConfigPanelHost.h"
#include "pj_widgets/DualOptionsWidget.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/LayerListView.h"
#include "pj_widgets/SectionHeaderBand.h"

namespace PJ {

namespace {
// SceneDockWidget keys layers by ObjectTopicId; LayerListView keys rows by an
// opaque qint64. The dock's topic id IS that opaque key.
[[nodiscard]] ObjectTopicId toTopicId(qint64 id) {
  ObjectTopicId tid;
  tid.id = static_cast<uint32_t>(id);
  return tid;
}
}  // namespace

Scene2DConfigPanel::Scene2DConfigPanel(QWidget* parent) : QWidget(parent) {
  // Zero outer margins so the section header bands span edge-to-edge, like
  // the plotting panel's Curve Width / Curve Style strips; each content block
  // under a band re-adds its own inset.
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  root->setSpacing(PJ::theme::space(theme::Space::None));

  root->addWidget(new SectionHeaderBand(tr("Layers"), this));
  auto* layers_host = new QWidget(this);
  auto* layers_layout = new QVBoxLayout(layers_host);
  layers_layout->setContentsMargins(
      PJ::theme::space(theme::Space::Comfortable), PJ::theme::space(theme::Space::Snug),
      PJ::theme::space(theme::Space::Comfortable), PJ::theme::space(theme::Space::Snug));
  list_ = new LayerListView(layers_host);
  layers_layout->addWidget(list_);
  root->addWidget(layers_host);

  rotation_section_ = new QWidget(this);
  auto* rotation_layout = new QVBoxLayout(rotation_section_);
  rotation_layout->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  rotation_layout->setSpacing(PJ::theme::space(theme::Space::None));
  rotation_layout->addWidget(new SectionHeaderBand(tr("Rotation"), rotation_section_));
  auto* rotation_host = new QWidget(rotation_section_);
  auto* rotation_host_layout = new QVBoxLayout(rotation_host);
  rotation_host_layout->setContentsMargins(
      PJ::theme::space(theme::Space::Comfortable), PJ::theme::space(theme::Space::Snug),
      PJ::theme::space(theme::Space::Comfortable), PJ::theme::space(theme::Space::Snug));
  rotation_ = new DualOptionsWidget(
      QStringList{QStringLiteral("0°"), QStringLiteral("90°"), QStringLiteral("-90°"), QStringLiteral("180°")},
      rotation_host);
  rotation_->setObjectName(QStringLiteral("scene2dRotation"));
  rotation_->setToolTip(tr("Rotate every layer clockwise"));
  rotation_host_layout->addWidget(rotation_);
  rotation_layout->addWidget(rotation_host);
  rotation_section_->setVisible(false);
  root->addWidget(rotation_section_);

  root->addWidget(new SectionHeaderBand(tr("Settings"), this));
  auto* settings_host = new QWidget(this);
  auto* settings_layout = new QVBoxLayout(settings_host);
  settings_layout->setContentsMargins(
      PJ::theme::space(theme::Space::Comfortable), PJ::theme::space(theme::Space::Snug),
      PJ::theme::space(theme::Space::Comfortable), PJ::theme::space(theme::Space::Snug));
  config_host_ = new ConfigPanelHost(settings_host);
  settings_layout->addWidget(config_host_);
  settings_layout->addStretch(1);
  root->addWidget(settings_host, /*stretch=*/1);

  connect(list_, &LayerListView::selectionChanged, this, &Scene2DConfigPanel::updateConfigPane);
  connect(rotation_, &DualOptionsWidget::selectionChanged, this, [this](int index) {
    if (auto* dock = qobject_cast<Scene2DDockWidget*>(dock_.data()); dock != nullptr && index >= 0) {
      dock->setViewRotation(kViewRotations.at(static_cast<size_t>(index)));
    }
  });
  connect(list_, &LayerListView::visibilityToggled, this, [this](qint64 id, bool visible) {
    if (dock_ != nullptr) {
      dock_->setLayerVisible(toTopicId(id), visible);
    }
  });
  connect(list_, &LayerListView::removeRequested, this, [this](qint64 id) {
    if (dock_ != nullptr) {
      dock_->removeTopic(toTopicId(id));
    }
  });
  connect(list_, &LayerListView::reordered, this, [this](const std::vector<qint64>& ordered_ids) {
    if (dock_ == nullptr) {
      return;
    }
    std::vector<ObjectTopicId> ordered;
    ordered.reserve(ordered_ids.size());
    for (const qint64 id : ordered_ids) {
      ordered.push_back(toTopicId(id));
    }
    dock_->reorderLayers(ordered);
  });
}

void Scene2DConfigPanel::bindDock(SceneDockWidget* dock) {
  if (dock_.data() == dock) {
    return;
  }
  disconnectDock();
  dock_ = dock;

  list_->clearRows();
  config_host_->clear();

  auto* dock_2d = qobject_cast<Scene2DDockWidget*>(dock);
  rotation_section_->setVisible(dock_2d != nullptr);
  if (dock == nullptr) {
    return;
  }

  rebuildList();
  if (dock_2d != nullptr) {
    syncRotation(dock_2d->viewRotation());
    connect(dock_2d, &Scene2DDockWidget::viewRotationChanged, this, &Scene2DConfigPanel::syncRotation);
  }

  connect(dock, &SceneDockWidget::layerAdded, this, [this](ObjectTopicId topic_id) {
    if (dock_ == nullptr) {
      return;
    }
    if (ISceneLayer* layer = dock_->layerFor(topic_id)) {
      const auto info = layer->info();
      list_->addRow(LayerRow{info.topic_id.id, info.display_name, info.visible});
    }
    updateConfigPane();
  });
  connect(dock, &SceneDockWidget::layerRemoved, this, [this](ObjectTopicId topic_id) {
    list_->removeRow(topic_id.id);
    updateConfigPane();
  });
  // Programmatic reorders (e.g. a dropped background moving beneath overlays)
  // would otherwise leave the rows, and the next row drag, in a stale order.
  connect(dock, &SceneDockWidget::layersReordered, this, [this]() {
    if (dock_ == nullptr) {
      return;
    }
    std::vector<qint64> ids;
    for (const auto& info : dock_->layers()) {
      ids.push_back(info.topic_id.id);
    }
    list_->setOrder(ids);
  });
  connect(dock, &SceneDockWidget::layerVisibilityChanged, this, [this](ObjectTopicId topic_id, bool visible) {
    list_->setRowVisible(topic_id.id, visible);
  });
  connect(dock, &SceneDockWidget::layerWarningChanged, this, [this](ObjectTopicId topic_id, bool warn, QString reason) {
    list_->setRowWarning(topic_id.id, warn, reason);
  });
}

void Scene2DConfigPanel::disconnectDock() {
  if (dock_ != nullptr) {
    disconnect(dock_.data(), nullptr, this, nullptr);
  }
  dock_ = nullptr;
}

void Scene2DConfigPanel::rebuildList() {
  std::vector<LayerRow> rows;
  if (dock_ != nullptr) {
    for (const auto& info : dock_->layers()) {
      rows.push_back(LayerRow{info.topic_id.id, info.display_name, info.visible});
    }
  }
  list_->setRows(rows);
  updateConfigPane();
}

void Scene2DConfigPanel::updateConfigPane() {
  config_host_->clear();
  if (dock_ == nullptr) {
    return;
  }
  const auto selected = list_->currentId();
  if (!selected.has_value()) {
    return;
  }
  ISceneLayer* layer = dock_->layerFor(toTopicId(*selected));
  if (layer == nullptr) {
    return;
  }
  if (QWidget* config = layer->createConfigWidget(config_host_)) {
    config_host_->setConfigWidget(config);
  }
}

void Scene2DConfigPanel::syncRotation(int degrees) {
  // setSelectedIndex echoes selectionChanged back into setViewRotation, which is
  // a no-op for the dock's current value.
  const auto match = std::find(kViewRotations.begin(), kViewRotations.end(), degrees);
  if (match != kViewRotations.end()) {
    rotation_->setSelectedIndex(static_cast<int>(match - kViewRotations.begin()));
  }
}

void Scene2DConfigPanel::onStylesheetChanged(QString theme) {
  theme_ = std::move(theme);
  if (list_ != nullptr) {
    list_->setTheme(theme_);
  }
}

}  // namespace PJ
