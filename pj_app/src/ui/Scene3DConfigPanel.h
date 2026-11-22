#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QPointer>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "pj_datastore/object_store.hpp"

class QEvent;
class QToolButton;
class QVariant;
class QVBoxLayout;

namespace PJ {

class ComboBox;
class ConfigPanelHost;
class DoubleScrubber;
class IntScrubber;
class LayerListView;
class Scene3DDockWidget;

// Persisted QSettings group of the scene controls, read by buildSceneControls and written by persistControl.
inline constexpr char kScene3dSceneControlsGroup[] = "pj_scene3d/scene_controls";

// Right-side-panel page for the 3D scene: scene controls (camera, grid, transforms, robot model) above the layer
// list. Scene controls are per dock: binding shows the dock's own look, an edit applies to the bound dock only and
// is persisted in QSettings (kScene3dSceneControlsGroup) as the starting look of a new panel.
//
// Robot-model rows are derived from dock state, not from the act of adding: every kRobotDescription layer on the
// bound dock gets exactly one row, rebuilt on bind, dock switch and layout restore. Robot layers stay out of the
// Topics list; clicking their row deselects the list and binds the Settings host to the layer's config widget (source
// combo, status, Retry). The browser build picks files by content and uses nonmodal topic/URL prompts.
class Scene3DConfigPanel : public QWidget {
  Q_OBJECT
 public:
  explicit Scene3DConfigPanel(QWidget* parent = nullptr);

  void bindDock(Scene3DDockWidget* dock);
#ifdef PJ_TARGET_WASM
  [[nodiscard]] Scene3DDockWidget* boundDockForTest() const;
  [[nodiscard]] LayerListView* layerListForTest() const;
#endif

  // Routes a click on a robot row's name to showRobotLayerConfig, by the widget's "robot_topic_id" property.
  bool eventFilter(QObject* watched, QEvent* event) override;

 public slots:
  void onStylesheetChanged(QString theme);

 private slots:
  void onLayerSelectionChanged();
  void onLayerAdded(ObjectTopicId topic_id);
  void onLayerRemoved(ObjectTopicId topic_id);
  void onLayerVisibilityChanged(ObjectTopicId topic_id, bool visible);
  void onLayerWarningChanged(ObjectTopicId topic_id, bool warn, const QString& reason);

 private:
  void buildSceneControls(QVBoxLayout* root);
  void applySceneControls();
  // Once per settled edit (editingFinished, toggle, style click); applying is live on every tick.
  void persistControl(const QString& key, const QVariant& value);
  // Shows `dock`'s own scene controls in the panel, without echoing back through the change handlers.
  void loadControlsFromDock(Scene3DDockWidget* dock);

 public:
  // Pushes the panel's current control values into `dock`'s view; MainWindow calls it for a dock the panel never
  // bound, once its lazily created view exists. No-op while the view does not exist.
  void applySceneControlsTo(Scene3DDockWidget* dock);

 private:
  void applyIcons();
  void setEyeIcon(QToolButton* eye, bool on);
  // Adds a robot model from the source combo's mode: a file, a robot_description topic, or a URL.
  void onAddModelClicked();
  // Idempotent, so repeated rebuilds and the add-then-layerAdded double fire leave one row.
  void addRobotRow(uint32_t topic_id_value, const QString& label, const QString& tooltip);
  void removeRobotRowFor(uint32_t topic_id_value);
  void showRobotLayerConfig(uint32_t topic_id_value);
  // "None" plus the bound dock's TF frames, reselecting its follow target; does not echo back as a user pick.
  void populateFollowCombo();
  void populateTrailCombo();
  void disconnectFromDock();
  void rebuildLayerList();
  void updateSelectedLayerPane();
  // Copy stashes the selected layer's serialized params and family in an app-wide clipboard; paste applies them when
  // the family matches; apply-to-family pushes them onto every other same-family layer of the dock.
  void onCopyParams();
  void onPasteParams();
  void onApplyParamsToFamily();
  void updateParamsToolbarState();
  [[nodiscard]] std::optional<QString> familyOf(ObjectTopicId topic_id) const;
  [[nodiscard]] std::optional<ObjectTopicId> selectedTopicId() const;
  [[nodiscard]] std::vector<ObjectTopicId> topicOrderFromIds(const std::vector<qint64>& ids) const;

  LayerListView* layer_list_ = nullptr;
  ConfigPanelHost* config_host_ = nullptr;
  QToolButton* params_copy_ = nullptr;
  QToolButton* params_paste_ = nullptr;
  QToolButton* params_apply_all_ = nullptr;
  QPointer<Scene3DDockWidget> bound_dock_;
  QString theme_{QStringLiteral("light")};

  // Drives the bound dock and is saved in its layout XML, never in QSettings. "None" turns following off.
  ComboBox* follow_frame_combo_ = nullptr;
  // Enabled only while a frame is followed.
  QToolButton* recenter_button_ = nullptr;

  QToolButton* grid_lines_button_ = nullptr;
  QToolButton* grid_cells_button_ = nullptr;
  QToolButton* grid_eye_ = nullptr;
  DoubleScrubber* grid_size_ = nullptr;
  IntScrubber* grid_divisions_ = nullptr;
  DoubleScrubber* gizmo_size_ = nullptr;
  // TF parent-connection lines, checked = visible; a static glyph, not an eye toggle.
  QToolButton* tf_lines_button_ = nullptr;
  // Eye toggles are independent of the opacities, so hiding and re-showing keeps the tuned opacity.
  DoubleScrubber* gizmo_opacity_ = nullptr;
  QToolButton* gizmo_eye_ = nullptr;
  DoubleScrubber* mesh_opacity_ = nullptr;
  QToolButton* mesh_eye_ = nullptr;
  DoubleScrubber* collision_opacity_ = nullptr;
  QToolButton* collision_eye_ = nullptr;

  ComboBox* model_source_combo_ = nullptr;
  QToolButton* add_model_button_ = nullptr;
  ComboBox* trail_frame_combo_ = nullptr;
  QToolButton* add_trail_button_ = nullptr;
  // Hidden while robot_rows_ is empty, so the form row collapses without a gap.
  QWidget* robot_rows_host_ = nullptr;
  QVBoxLayout* robot_rows_layout_ = nullptr;
  // Layer topic id -> row; cleared on rebind (rows belong to the bound dock).
  std::vector<std::pair<uint32_t, QWidget*>> robot_rows_;
};

}  // namespace PJ
