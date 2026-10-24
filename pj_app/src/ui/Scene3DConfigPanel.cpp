// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/Scene3DConfigPanel.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QSize>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/layers/robot_model_layer.h"
#ifdef PJ_TARGET_WASM
#include "FileSelectionService.h"
#endif
#include "pj_scene3d_widgets/layers/trail_layer.h"
#include "pj_scene3d_widgets/mesh_shading_params.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene_common/layer_params.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_scene_common/scene_layer.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/ConfigPanelHost.h"
#include "pj_widgets/Dialog.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FileDialog.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/IntScrubber.h"
#include "pj_widgets/LayerListView.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/ScrubberBase.h"
#include "pj_widgets/SectionHeaderBand.h"
#include "pj_widgets/Style.h"  // PJ::Style::kInputHeight (uniform row height)
#include "pj_widgets/SvgUtil.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

#ifndef PJ_TARGET_WASM
constexpr char kUrdfBrowseDirKey[] = "pj_scene3d/urdf_browse_dir";
#endif
constexpr auto kTrashIconPath = ":/resources/svg/trash.svg";
constexpr auto kAddIconPath = ":/resources/svg/add.svg";
constexpr auto kVisibilityOnPath = ":/resources/svg/visibility.svg";
constexpr auto kVisibilityOffPath = ":/resources/svg/visibility_off.svg";
constexpr auto kTfConnectionsIconPath = ":/resources/svg/graph_4.svg";

// Trailing eye/add/trash button column: the scene-control grids reserve this
// width in their 3rd column so every field's right edge lines up whether or not
// the row carries a trailing button. kTrailingIconPx is the glyph size inside it.
// Every side-panel SVG-icon button is a 20x20 square (matching the Topics table's
// eye/trash, which are kDefaultRowHeight==20 square), so the icon column reads as
// one consistent size and the rows stay compact.
constexpr int kTrailingSlotWidth = 20;
constexpr int kTrailingIconPx = 20;
// Horizontal gap between grid columns; the robot-row HBox reuses it so the robot
// name's right edge lands on the same x as the field column above.
constexpr auto kGridHSpacing = theme::Space::Comfortable;

// Uniform sizing for the inline eye/add/trash buttons so the trailing column is
// pixel-aligned regardless of the platform style's default tool-button metrics.
void sizeTrailingButton(QToolButton* button) {
  button->setIconSize(QSize(kTrailingIconPx, kTrailingIconPx));
  // Fixed HEIGHT too (== the input height), else the tool button's natural
  // height (~24-26px) inflates its grid row above the 20px inputs and loosens
  // the row spacing.
  button->setFixedSize(kTrailingSlotWidth, PJ::Style::kInputHeight);
}

// Add one [label | field | trailing] row to a 3-column scene-control grid.
// Using a real grid column (not an in-cell spacer) keeps every field's right
// edge and the trailing eye/add button aligned across rows by construction.
// `trailing` may be null (column 2 stays reserved via setColumnMinimumWidth).
void addGridRow(QGridLayout* grid, int& row, const QString& label, QWidget* field, QWidget* trailing = nullptr) {
  grid->addWidget(new QLabel(label + u":"_s), row, 0);
  grid->addWidget(field, row, 1);
  if (trailing != nullptr) {
    grid->addWidget(trailing, row, 2);
  }
  ++row;
}

// App-wide clipboard for layer parameters: Copy stores the source layer's
// serialized params plus its family; Paste applies them to a same-family layer.
// Static (process-wide) so a copy in one 3D dock can be pasted into another.
struct LayerParamClipboard {
  QString xml;
  QString family;
};
LayerParamClipboard& layerParamClipboard() {
  static LayerParamClipboard clipboard;
  return clipboard;
}

// Small modal prompt on the shared Dialog chrome: a single field + OK/Cancel.
// The field is parented into the dialog; values must be read before `dialog`
// leaves scope, which is why each picker below returns the value, not a bool.
#ifndef PJ_TARGET_WASM
bool execFieldDialog(Dialog& dialog, const QString& title, QWidget* field) {
  dialog.setDialogTitle(title);
  auto* layout = new QVBoxLayout(dialog.contentWidget());
  layout->addWidget(field);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog.contentWidget());
  layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  return dialog.exec() == QDialog::Accepted;
}

// Topic mode: pick one of the dataset's robot_description topics.
std::optional<std::pair<ObjectTopicId, QString>> pickRobotDescriptionTopic(
    QWidget* parent, const QList<Scene3DDockWidget::RobotDescriptionTopic>& topics) {
  Dialog dialog(parent);
  auto* combo = new ComboBox(dialog.contentWidget());
  for (const auto& topic : topics) {
    combo->addItem(topic.name, QVariant::fromValue(static_cast<uint>(topic.topic_id.id)));
  }
  if (!execFieldDialog(dialog, QObject::tr("Robot description topic"), combo) || combo->currentIndex() < 0) {
    return std::nullopt;
  }
  ObjectTopicId topic_id;
  topic_id.id = combo->currentData().toUInt();
  return std::make_pair(topic_id, combo->currentText());
}

// URL mode: free-text http(s) URDF location.
std::optional<QString> promptUrdfUrl(QWidget* parent) {
  Dialog dialog(parent);
  auto* edit = new QLineEdit(dialog.contentWidget());
  edit->setPlaceholderText(u"https://example.com/robot.urdf"_s);
  edit->setMinimumWidth(360);
  if (!execFieldDialog(dialog, QObject::tr("Load URDF from URL"), edit)) {
    return std::nullopt;
  }
  const QString url = edit->text().trimmed();
  return url.isEmpty() ? std::nullopt : std::optional<QString>(url);
}
#endif

#ifdef PJ_TARGET_WASM
void showBrowserDialog(Dialog* dialog) {
  // QDialog::open() forces WindowModal even after another modality was set.
  // Browser workflows must remain nonblocking and nonmodal, matching the
  // dialog host's explicit-modality + show() contract.
  dialog->setWindowModality(Qt::NonModal);
  dialog->show();
  dialog->activateWindow();
}

// Chrome shared by the browser-only prompts: a self-deleting titled Dialog the
// caller fills, then finishes with a named button box (Cancel pre-wired) that
// is appended, sized, and shown nonmodal.
Dialog* makeBrowserDialog(QWidget* parent, const QString& title) {
  auto* dialog = new Dialog(parent);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setDialogTitle(title);
  return dialog;
}

QDialogButtonBox* finishBrowserDialog(Dialog* dialog, QDialogButtonBox::StandardButtons standard_buttons) {
  auto* buttons = new QDialogButtonBox(standard_buttons, dialog->contentWidget());
  buttons->setObjectName(u"buttonBox"_s);
  dialog->contentLayout()->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  dialog->contentLayout()->activate();
  dialog->adjustSize();
  showBrowserDialog(dialog);
  return buttons;
}

void openBrowserInfo(QWidget* parent, const QString& title, const QString& text) {
  Dialog* dialog = makeBrowserDialog(parent, title);
  auto* label = new QLabel(text, dialog->contentWidget());
  label->setWordWrap(true);
  dialog->contentLayout()->addWidget(label);
  auto* buttons = finishBrowserDialog(dialog, QDialogButtonBox::Ok);
  QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
}

void openRobotDescriptionTopicPicker(
    QWidget* parent, const QList<Scene3DDockWidget::RobotDescriptionTopic>& topics,
    std::function<void(ObjectTopicId, QString)> callback) {
  Dialog* dialog = makeBrowserDialog(parent, QObject::tr("Robot description topic"));
  auto* combo = new ComboBox(dialog->contentWidget());
  for (const auto& topic : topics) {
    combo->addItem(topic.name, QVariant::fromValue(static_cast<uint>(topic.topic_id.id)));
  }
  dialog->contentLayout()->addWidget(combo);
  auto* buttons = finishBrowserDialog(dialog, QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  QObject::connect(
      buttons, &QDialogButtonBox::accepted, dialog, [dialog, combo, callback = std::move(callback)]() mutable {
        if (combo->currentIndex() >= 0) {
          callback(ObjectTopicId{combo->currentData().toUInt()}, combo->currentText());
        }
        dialog->accept();
      });
}

void openUrdfUrlPrompt(QWidget* parent, std::function<void(QString)> callback) {
  Dialog* dialog = makeBrowserDialog(parent, QObject::tr("Load URDF from URL"));
  auto* edit = new QLineEdit(dialog->contentWidget());
  edit->setObjectName(u"wasmUrdfUrlEdit"_s);
  edit->setPlaceholderText(u"https://example.com/robot.urdf"_s);
  edit->setMinimumWidth(360);
  dialog->contentLayout()->addWidget(edit);
  auto* buttons = finishBrowserDialog(dialog, QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  QObject::connect(
      buttons, &QDialogButtonBox::accepted, dialog, [dialog, edit, callback = std::move(callback)]() mutable {
        const QString url = edit->text().trimmed();
        if (!url.isEmpty()) {
          callback(url);
        }
        dialog->accept();
      });
}
#endif

[[nodiscard]] ObjectTopicId topicFromRowId(qint64 id) {
  ObjectTopicId topic_id;
  topic_id.id = static_cast<uint32_t>(id);
  return topic_id;
}

[[nodiscard]] LayerRow rowFromLayerInfo(const SceneLayerInfo& info) {
  return LayerRow{
      .id = static_cast<qint64>(info.topic_id.id),
      .name = info.display_name,
      .visible = info.visible,
  };
}

DoubleScrubber* makeScrubber(double min, double max, double step, double value) {
  auto* scrubber = new DoubleScrubber;
  scrubber->setRange(min, max);
  scrubber->setSingleStep(step);
  scrubber->setDecimals(2);
  scrubber->setValue(value);
  return scrubber;
}

}  // namespace

Scene3DConfigPanel::Scene3DConfigPanel(QWidget* parent) : QWidget(parent) {
  // Scene-control changes apply live every tick (valueChanged) but persist
  // only when the edit settles — scrubbers on ScrubberBase::editingFinished,
  // toggles/style on click — so a drag is one INI write, not one per tick.

  // Zero outer margins so the section header bands (Grid · Transforms and
  // RobotModel · Topics · Settings) span edge-to-edge like the plotting
  // panel's Curve Width / Curve Style bands; each content block under a band
  // re-adds its own 8-px horizontal inset.
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  outer->setSpacing(PJ::theme::space(theme::Space::None));

  buildSceneControls(outer);

  outer->addWidget(new SectionHeaderBand(tr("Topics"), this));
  auto* topics_host = new QWidget(this);
  auto* topics_layout = new QVBoxLayout(topics_host);
  topics_layout->setContentsMargins(
      PJ::theme::space(theme::Space::Comfortable), PJ::theme::space(theme::Space::Snug),
      PJ::theme::space(theme::Space::Comfortable), PJ::theme::space(theme::Space::Snug));
  layer_list_ = new LayerListView(topics_host);
  topics_layout->addWidget(layer_list_);
  outer->addWidget(topics_host);

  outer->addWidget(new SectionHeaderBand(tr("Settings"), this));
  auto* settings_host = new QWidget(this);
  auto* settings_layout = new QVBoxLayout(settings_host);
  settings_layout->setContentsMargins(
      PJ::theme::space(theme::Space::Comfortable), PJ::theme::space(theme::Space::Snug),
      PJ::theme::space(theme::Space::Comfortable), PJ::theme::space(theme::Space::Snug));

  // Right-aligned copy / paste / apply-to-family row, just below the Settings
  // header. Glyphs are set theme-aware in applyIcons(); enabled state tracks the
  // selection + clipboard via updateParamsToolbarState().
  auto* params_toolbar = new QWidget(settings_host);
  auto* params_toolbar_layout = new QHBoxLayout(params_toolbar);
  params_toolbar_layout->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  params_toolbar_layout->setSpacing(PJ::theme::space(theme::Space::Tight));
  params_toolbar_layout->addStretch(1);
  const auto make_param_button = [params_toolbar](const QString& tip) {
    auto* button = new QToolButton(params_toolbar);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setIconSize(QSize(PJ::Style::kInputHeight, PJ::Style::kInputHeight));
    button->setFixedSize(PJ::Style::kInputHeight, PJ::Style::kInputHeight);  // 20x20 like every icon button
    button->setToolTip(tip);
    return button;
  };
  params_copy_ = make_param_button(tr("Copy parameters"));
  params_paste_ = make_param_button(tr("Paste parameters"));
  params_apply_all_ = make_param_button(tr("Apply these parameters to all topics of the same type"));
  params_toolbar_layout->addWidget(params_copy_);
  params_toolbar_layout->addWidget(params_paste_);
  params_toolbar_layout->addWidget(params_apply_all_);
  settings_layout->addWidget(params_toolbar);
  connect(params_copy_, &QToolButton::clicked, this, &Scene3DConfigPanel::onCopyParams);
  connect(params_paste_, &QToolButton::clicked, this, &Scene3DConfigPanel::onPasteParams);
  connect(params_apply_all_, &QToolButton::clicked, this, &Scene3DConfigPanel::onApplyParamsToFamily);

  config_host_ = new ConfigPanelHost(settings_host);
  settings_layout->addWidget(config_host_);
  settings_layout->addStretch(1);
  outer->addWidget(settings_host, /*stretch=*/1);

  connect(layer_list_, &LayerListView::selectionChanged, this, &Scene3DConfigPanel::onLayerSelectionChanged);
  connect(layer_list_, &LayerListView::visibilityToggled, this, [this](qint64 id, bool visible) {
    if (bound_dock_ != nullptr) {
      bound_dock_->setLayerVisible(topicFromRowId(id), visible);
    }
  });
  connect(layer_list_, &LayerListView::removeRequested, this, [this](qint64 id) {
    if (bound_dock_ != nullptr) {
      bound_dock_->removeTopic(topicFromRowId(id));
    }
  });
  connect(layer_list_, &LayerListView::reordered, this, [this](const std::vector<qint64>& ids) {
    if (bound_dock_ != nullptr) {
      bound_dock_->reorderLayers(topicOrderFromIds(ids));
    }
  });

  applyIcons();
  updateSelectedLayerPane();
}

#ifdef PJ_TARGET_WASM
Scene3DDockWidget* Scene3DConfigPanel::boundDockForTest() const {
  return bound_dock_.data();
}

LayerListView* Scene3DConfigPanel::layerListForTest() const {
  return layer_list_;
}
#endif

void Scene3DConfigPanel::buildSceneControls(QVBoxLayout* root) {
  QSettings settings;
  settings.beginGroup(QString::fromLatin1(kScene3dSceneControlsGroup));
  // Each control: init from QSettings (defaults = the User's look-dev pick),
  // persist + re-apply to the bound dock on every change.
  const auto wire = [this, &settings](auto* widget, const char* key, auto read, auto signal) {
    widget->setProperty("settings_key", QString::fromLatin1(key));
    if (const QVariant saved = settings.value(QString::fromLatin1(key)); saved.isValid()) {
      read(saved);
    }
    // Apply live every tick so the view tracks the scrubber.
    connect(widget, signal, this, [this]() { applySceneControls(); });
    // Persist only when the scrub/edit settles — one INI rewrite per drag.
    connect(widget, &ScrubberBase::editingFinished, this, [this, widget]() {
      const QString settings_key = widget->property("settings_key").toString();
      if (auto* dscrub = qobject_cast<DoubleScrubber*>(widget)) {
        persistControl(settings_key, dscrub->value());
      } else if (auto* iscrub = qobject_cast<IntScrubber*>(widget)) {
        persistControl(settings_key, iscrub->value());
      }
    });
  };

  // Eye toggles: checked = visible. Persisted like the other controls; the
  // icon mirrors the checked state (visibility / visibility_off). The shared
  // curveVisibilityToggle objectName picks up the QSS rule that keeps these
  // flat in every state — no checked/hover wash, the glyph is the indicator.
  const auto make_eye = [this, &settings](const char* key, const QString& tip) {
    auto* eye = new QToolButton(this);
    eye->setObjectName(u"curveVisibilityToggle"_s);
    eye->setCheckable(true);
    eye->setAutoRaise(true);
    eye->setFocusPolicy(Qt::NoFocus);
    eye->setToolTip(tip);
    sizeTrailingButton(eye);
    eye->setProperty("settings_key", QString::fromLatin1(key));
    eye->setChecked(settings.value(QString::fromLatin1(key), true).toBool());
    connect(eye, &QToolButton::toggled, this, [this, eye](bool checked) {
      persistControl(eye->property("settings_key").toString(), checked);
      setEyeIcon(eye, checked);
      applySceneControls();
    });
    return eye;
  };
  const auto add_band = [this, root](const QString& text) { root->addWidget(new SectionHeaderBand(text, this)); };

  // Each band's controls live in a 3-column grid: label | field | trailing
  // button. Column 1 stretches; column 2 is pinned to the trailing-button width
  // so plain rows line up with rows that carry an eye/add button — and the Grid
  // and Transforms sections align with each other. (A QFormLayout can't share a
  // trailing column across its rows, which is what caused the right-edge drift.)
  // The label column (0) is pinned to a shared width after both grids are built
  // (shareLabelColumn below) so the two sections' labels — and therefore fields —
  // line up vertically even though they are separate layouts.
  const auto add_grid = [this, root]() {
    auto* host = new QWidget(this);
    auto* grid = new QGridLayout(host);
    grid->setContentsMargins(
        PJ::theme::space(theme::Space::Comfortable), PJ::theme::space(theme::Space::Snug),
        PJ::theme::space(theme::Space::Comfortable), PJ::theme::space(theme::Space::Snug));
    grid->setHorizontalSpacing(PJ::theme::space(kGridHSpacing));
    grid->setVerticalSpacing(PJ::theme::space(theme::Space::Snug));
    grid->setColumnStretch(1, 1);
    grid->setColumnMinimumWidth(2, kTrailingSlotWidth);
    root->addWidget(host);
    return grid;
  };

  // --- Camera --------------------------------------------------------------
  // "Follow frame": make the camera track a TF frame's position (Position-only
  // follow). "None" = off. Per-dock (drives the bound dock, persisted in its
  // layout XML) — NOT a shared-look QSettings control, so it is wired in bindDock
  // (populateFollowCombo + availableFramesChanged/followFrameChanged), not here.
  add_band(tr("Camera"));
  QGridLayout* camera_grid = add_grid();
  int camera_row = 0;
  follow_frame_combo_ = new ComboBox(this);
  follow_frame_combo_->setObjectName(u"scene3dFollowFrameCombo"_s);
  follow_frame_combo_->setFocusPolicy(Qt::ClickFocus);
  follow_frame_combo_->setToolTip(tr("Make the camera follow a TF frame's position"));
  follow_frame_combo_->addItem(tr("None"), QString());
  connect(follow_frame_combo_, &QComboBox::currentIndexChanged, this, [this](int /*index*/) {
    if (bound_dock_ != nullptr && follow_frame_combo_ != nullptr) {
      bound_dock_->setFollowFrame(follow_frame_combo_->currentData().toString());
    }
  });
  // Trailing recenter button: snap the camera onto the followed frame on demand.
  // Glyph set theme-aware in applyIcons(); enabled state tracks the follow target
  // (populateFollowCombo). Sized like every other trailing icon button.
  recenter_button_ = new QToolButton(this);
  recenter_button_->setAutoRaise(true);
  recenter_button_->setFocusPolicy(Qt::NoFocus);
  recenter_button_->setToolTip(tr("Recenter the camera on the followed frame"));
  recenter_button_->setEnabled(false);  // no follow target yet; populateFollowCombo updates this
  sizeTrailingButton(recenter_button_);
  connect(recenter_button_, &QToolButton::clicked, this, [this]() {
    if (bound_dock_ != nullptr) {
      bound_dock_->recenterOnFollowFrame();
    }
  });
  addGridRow(camera_grid, camera_row, tr("Follow frame"), follow_frame_combo_, recenter_button_);

  // --- Grid ---------------------------------------------------------------
  add_band(tr("Grid"));
  QGridLayout* grid_grid = add_grid();
  int grid_row = 0;

  auto* style_row = new QHBoxLayout;
  style_row->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  const auto make_style_button = [this](const QString& tip) {
    auto* button = new QToolButton(this);
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(tip);
    button->setIconSize(QSize(PJ::Style::kInputHeight, PJ::Style::kInputHeight));
    button->setFixedSize(PJ::Style::kInputHeight, PJ::Style::kInputHeight);  // 20x20 like every icon button
    return button;
  };
  grid_lines_button_ = make_style_button(tr("Line grid"));
  grid_cells_button_ = make_style_button(tr("Checkerboard"));
  auto* style_group = new QButtonGroup(this);
  style_group->setExclusive(true);
  style_group->addButton(grid_lines_button_, 0);
  style_group->addButton(grid_cells_button_, 1);
  const int saved_style = settings.value(u"grid_style"_s, 0).toInt();
  (saved_style == 1 ? grid_cells_button_ : grid_lines_button_)->setChecked(true);
  connect(style_group, &QButtonGroup::idClicked, this, [this](int id) {
    persistControl(u"grid_style"_s, id);
    applySceneControls();
  });
  grid_eye_ = make_eye("grid_visible", tr("Show/hide the grid"));
  style_row->addWidget(grid_lines_button_);
  style_row->addWidget(grid_cells_button_);
  style_row->addWidget(grid_eye_);
  style_row->addStretch(1);
  // The style toggles are a strip, not a single field: keep the label in col 0
  // and let the strip span the field + trailing columns.
  grid_grid->addWidget(new QLabel(tr("Style") + u":"_s), grid_row, 0);
  grid_grid->addLayout(style_row, grid_row, 1, 1, 2);
  ++grid_row;

  grid_size_ = makeScrubber(1.0, 1000.0, 1.0, 10.0);
  grid_size_->setDecimals(0);
  wire(
      grid_size_, "grid_size", [this](const QVariant& v) { grid_size_->setValue(v.toDouble()); },
      qOverload<double>(&DoubleScrubber::valueChanged));
  addGridRow(grid_grid, grid_row, tr("Size (m)"), grid_size_);

  grid_divisions_ = new IntScrubber;
  grid_divisions_->setRange(1, 200);
  grid_divisions_->setValue(10);
  wire(
      grid_divisions_, "grid_divisions", [this](const QVariant& v) { grid_divisions_->setValue(v.toInt()); },
      qOverload<int>(&IntScrubber::valueChanged));
  addGridRow(grid_grid, grid_row, tr("Divisions"), grid_divisions_);

  // --- Transforms and RobotModel --------------------------------------------
  add_band(tr("Transforms and RobotModel"));
  QGridLayout* tm_grid = add_grid();
  int tm_row = 0;

  // "Frames" in the UI = the TF frame axis triads (gizmo_* internally and in
  // the persisted settings keys, kept for compatibility).
  // Trailing toggle on this row: show/hide the magenta lines connecting each TF
  // frame to its parent (rviz2/Foxglove "show parent connections"). Sized like
  // the eye toggles (sizeTrailingButton → 20x20, column 2) but WITHOUT the
  // curveVisibilityToggle objectName, so the default :checked QSS wash gives the
  // on/off feedback its static graph glyph can't. Default on. Persisted under
  // tf_parent_lines (one of the mirrored scene-control sites — see
  // applySceneControlsTo).
  tf_lines_button_ = new QToolButton(this);
  tf_lines_button_->setCheckable(true);
  tf_lines_button_->setAutoRaise(true);
  tf_lines_button_->setFocusPolicy(Qt::NoFocus);
  tf_lines_button_->setToolTip(tr("Show/hide lines connecting each TF frame to its parent"));
  sizeTrailingButton(tf_lines_button_);
  tf_lines_button_->setProperty("settings_key", u"tf_parent_lines"_s);
  tf_lines_button_->setChecked(settings.value(u"tf_parent_lines"_s, true).toBool());
  connect(tf_lines_button_, &QToolButton::toggled, this, [this](bool checked) {
    persistControl(u"tf_parent_lines"_s, checked);
    applySceneControls();
  });

  gizmo_size_ = makeScrubber(0.01, 5.0, 0.05, 0.15);
  wire(
      gizmo_size_, "gizmo_size", [this](const QVariant& v) { gizmo_size_->setValue(v.toDouble()); },
      qOverload<double>(&DoubleScrubber::valueChanged));
  addGridRow(tm_grid, tm_row, tr("Frames size (m)"), gizmo_size_, tf_lines_button_);

  gizmo_opacity_ = makeScrubber(0.0, 1.0, 0.1, 1.0);
  gizmo_eye_ = make_eye("gizmos_visible", tr("Show/hide the TF frames"));
  wire(
      gizmo_opacity_, "gizmo_opacity", [this](const QVariant& v) { gizmo_opacity_->setValue(v.toDouble()); },
      qOverload<double>(&DoubleScrubber::valueChanged));
  addGridRow(tm_grid, tm_row, tr("Frames opacity"), gizmo_opacity_, gizmo_eye_);

  model_source_combo_ = new ComboBox;
#ifdef PJ_TARGET_WASM
  model_source_combo_->setObjectName(u"scene3dModelSource"_s);
#endif
  model_source_combo_->addItem(tr("File"));
  model_source_combo_->addItem(tr("Topic"));
  model_source_combo_->addItem(tr("URL"));
  add_model_button_ = new QToolButton(this);
#ifdef PJ_TARGET_WASM
  add_model_button_->setObjectName(u"scene3dAddRobotModel"_s);
#endif
  add_model_button_->setAutoRaise(true);
  add_model_button_->setFocusPolicy(Qt::NoFocus);
  add_model_button_->setToolTip(tr("Add a robot model from the selected source"));
  sizeTrailingButton(add_model_button_);
  addGridRow(tm_grid, tm_row, tr("Model/URDF"), model_source_combo_, add_model_button_);
  connect(add_model_button_, &QToolButton::clicked, this, &Scene3DConfigPanel::onAddModelClicked);

  // One row per panel-added robot model (name + bin), appended below the
  // Model/URDF row by addRobotRow. Hosted in a widget that spans all three
  // columns and stays hidden while empty — an empty grid row would otherwise
  // reserve vertical spacing and leave a phantom gap under Model/URDF.
  robot_rows_host_ = new QWidget(this);
  robot_rows_layout_ = new QVBoxLayout(robot_rows_host_);
  robot_rows_layout_->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  robot_rows_layout_->setSpacing(PJ::theme::space(theme::Space::Tight));
  robot_rows_host_->hide();
  tm_grid->addWidget(robot_rows_host_, tm_row, 0, 1, 3);
  ++tm_row;

  mesh_opacity_ = makeScrubber(0.0, 1.0, 0.1, 1.0);
#ifdef PJ_TARGET_WASM
  mesh_opacity_->setObjectName(u"scene3dMeshOpacity"_s);
#endif
  mesh_eye_ = make_eye("meshes_visible", tr("Show/hide visual meshes"));
#ifdef PJ_TARGET_WASM
  mesh_eye_->setObjectName(u"scene3dMeshVisible"_s);
#endif
  wire(
      mesh_opacity_, "mesh_opacity", [this](const QVariant& v) { mesh_opacity_->setValue(v.toDouble()); },
      qOverload<double>(&DoubleScrubber::valueChanged));
  addGridRow(tm_grid, tm_row, tr("Meshes opacity"), mesh_opacity_, mesh_eye_);

  collision_opacity_ = makeScrubber(0.0, 1.0, 0.1, 0.4);
#ifdef PJ_TARGET_WASM
  collision_opacity_->setObjectName(u"scene3dCollisionOpacity"_s);
#endif
  collision_eye_ = make_eye("collisions_visible", tr("Show/hide collision meshes"));
#ifdef PJ_TARGET_WASM
  collision_eye_->setObjectName(u"scene3dCollisionVisible"_s);
#endif
  wire(
      collision_opacity_, "collision_opacity",
      [this](const QVariant& v) { collision_opacity_->setValue(v.toDouble()); },
      qOverload<double>(&DoubleScrubber::valueChanged));
  addGridRow(tm_grid, tm_row, tr("Collision opacity"), collision_opacity_, collision_eye_);

  // Last row: unlike the rows above it does not edit existing scene state, it
  // creates a new trail layer.
  trail_frame_combo_ = new ComboBox;
  trail_frame_combo_->setFocusPolicy(Qt::ClickFocus);
  trail_frame_combo_->setToolTip(tr("TF frame whose motion trail to draw"));
  add_trail_button_ = new QToolButton(this);
  add_trail_button_->setAutoRaise(true);
  add_trail_button_->setFocusPolicy(Qt::NoFocus);
  add_trail_button_->setToolTip(tr("Add a motion trail for the selected frame"));
  sizeTrailingButton(add_trail_button_);
  addGridRow(tm_grid, tm_row, tr("Create Trail"), trail_frame_combo_, add_trail_button_);
  connect(add_trail_button_, &QToolButton::clicked, this, [this]() {
    if (bound_dock_ == nullptr || trail_frame_combo_ == nullptr) {
      return;
    }
    const QString frame = trail_frame_combo_->currentData().toString();
    if (!frame.isEmpty()) {
      bound_dock_->addTrailLayer(pj::scene3d::TrailSource::tfFrame(frame));
    }
  });

  // Pin both grids' label column to the widest label across BOTH sections, read
  // back from the labels just added (no separate string list to keep in sync).
  // Equal column 0 + fixed column 2 ⇒ the stretchy field column also lines up,
  // so the Grid and Transforms sections align even though they are separate grids.
  const auto widest_label = [](QGridLayout* grid) {
    int width = 0;
    for (int r = 0; r < grid->rowCount(); ++r) {
      if (auto* item = grid->itemAtPosition(r, 0); item != nullptr) {
        if (auto* label = qobject_cast<QLabel*>(item->widget()); label != nullptr) {
          width = std::max(width, label->sizeHint().width());
        }
      }
    }
    return width;
  };
  const int label_col_w = std::max({widest_label(camera_grid), widest_label(grid_grid), widest_label(tm_grid)});
  camera_grid->setColumnMinimumWidth(0, label_col_w);
  grid_grid->setColumnMinimumWidth(0, label_col_w);
  tm_grid->setColumnMinimumWidth(0, label_col_w);
}

void Scene3DConfigPanel::applySceneControls() {
  applySceneControlsTo(bound_dock_.data());
}

void Scene3DConfigPanel::persistControl(const QString& key, const QVariant& value) {
  QSettings settings;
  settings.beginGroup(QString::fromLatin1(kScene3dSceneControlsGroup));
  settings.setValue(key, value);
}

void Scene3DConfigPanel::applySceneControlsTo(Scene3DDockWidget* dock) {
  if (dock == nullptr) {
    return;
  }
  auto* view = dock->sceneView();
  if (view == nullptr) {
    return;
  }
  // The scene-control field set is mirrored across FOUR sites — keep them in sync
  // when adding a control (a miss is silent, no compile error): this push,
  // loadControlsFromDock (reflect-on-bind), and Scene3DDockWidget's
  // xmlSaveState/xmlLoadState (per-dock layout persistence).
  view->setGridVisible(grid_eye_->isChecked());
  view->setGridStyle(
      grid_cells_button_->isChecked() ? pj::scene3d::SceneViewWidget::GridStyle::kFilledCells
                                      : pj::scene3d::SceneViewWidget::GridStyle::kLines);
  view->setGridExtentMetres(static_cast<float>(grid_size_->value()));
  view->setGridDivisions(grid_divisions_->value());
  view->setAxesVisible(gizmo_eye_->isChecked());
  view->setGizmoSize(static_cast<float>(gizmo_size_->value()));
  view->setGizmoOpacity(static_cast<float>(gizmo_opacity_->value()));
  view->setTfConnectionsVisible(tf_lines_button_->isChecked());

  // Per-view look knobs: drives only the bound dock's view. Sibling docks keep
  // their own MeshShadingParams and converge when the panel rebinds and applies.
  auto& shading = view->meshShadingParams();
  shading.meshes_visible = mesh_eye_->isChecked();
  shading.mesh_opacity = static_cast<float>(mesh_opacity_->value());
  shading.collisions_visible = collision_eye_->isChecked();
  shading.collision_opacity = static_cast<float>(collision_opacity_->value());
  view->invalidateContent();
}

void Scene3DConfigPanel::loadControlsFromDock(Scene3DDockWidget* dock) {
  if (dock == nullptr) {
    return;
  }
  auto* view = dock->sceneView();
  if (view == nullptr) {
    return;
  }
  // Inverse of applySceneControlsTo (keep the field set in sync — 4 sites).
  // Reflect the dock's OWN look in the widgets without echoing it straight back
  // through the change handlers (which would re-apply + re-persist). Scrubbers
  // are blocked; the eye toggles are set + their icon refreshed by hand (the
  // toggled handler that normally swaps the icon is suppressed by the blocker).
  const auto set_eye = [this](QToolButton* eye, bool on) {
    const QSignalBlocker block(eye);
    eye->setChecked(on);
    setEyeIcon(eye, on);
  };

  {
    const QSignalBlocker b_grid_size(grid_size_);
    const QSignalBlocker b_grid_div(grid_divisions_);
    const QSignalBlocker b_gizmo_size(gizmo_size_);
    const QSignalBlocker b_gizmo_op(gizmo_opacity_);
    const QSignalBlocker b_mesh_op(mesh_opacity_);
    const QSignalBlocker b_coll_op(collision_opacity_);

    grid_size_->setValue(view->gridExtentMetres());
    grid_divisions_->setValue(view->gridDivisions());
    gizmo_size_->setValue(view->gizmoSize());
    gizmo_opacity_->setValue(view->gizmoOpacity());

    const auto& shading = view->meshShadingParams();
    mesh_opacity_->setValue(shading.mesh_opacity);
    collision_opacity_->setValue(shading.collision_opacity);
    set_eye(mesh_eye_, shading.meshes_visible);
    set_eye(collision_eye_, shading.collisions_visible);
  }

  // idClicked (the connected signal) fires only on user clicks, not programmatic
  // setChecked, so the exclusive style group needs no blocker.
  (view->gridStyle() == pj::scene3d::SceneViewWidget::GridStyle::kFilledCells ? grid_cells_button_ : grid_lines_button_)
      ->setChecked(true);
  set_eye(grid_eye_, view->gridVisible());
  set_eye(gizmo_eye_, view->axesVisible());
  // Not an eye toggle (static glyph): just reflect the checked state without
  // echoing back through the toggled handler (which would re-persist/re-apply).
  {
    const QSignalBlocker block(tf_lines_button_);
    tf_lines_button_->setChecked(view->tfConnectionsVisible());
  }
}

void Scene3DConfigPanel::setEyeIcon(QToolButton* eye, bool on) {
  eye->setIcon(loadSvg(QLatin1String(on ? kVisibilityOnPath : kVisibilityOffPath), theme_));
}

void Scene3DConfigPanel::applyIcons() {
  if (grid_lines_button_ != nullptr) {
    grid_lines_button_->setIcon(loadSvg(u":/resources/svg/grid_4x4.svg"_s, theme_));
  }
  if (grid_cells_button_ != nullptr) {
    grid_cells_button_->setIcon(loadSvg(u":/resources/svg/grid_view.svg"_s, theme_));
  }
  for (QToolButton* eye : {grid_eye_, gizmo_eye_, mesh_eye_, collision_eye_}) {
    if (eye != nullptr) {
      setEyeIcon(eye, eye->isChecked());
    }
  }
  if (add_model_button_ != nullptr) {
    add_model_button_->setIcon(loadSvg(QLatin1String(kAddIconPath), theme_));
  }
  if (add_trail_button_ != nullptr) {
    add_trail_button_->setIcon(loadSvg(QLatin1String(kAddIconPath), theme_));
  }
  if (tf_lines_button_ != nullptr) {
    tf_lines_button_->setIcon(loadSvg(QLatin1String(kTfConnectionsIconPath), theme_));
  }
  if (recenter_button_ != nullptr) {
    recenter_button_->setIcon(loadSvg(u":/resources/svg/recenter.svg"_s, theme_));
  }
  if (params_copy_ != nullptr) {
    params_copy_->setIcon(loadSvg(u":/resources/svg/copy.svg"_s, theme_));
  }
  if (params_paste_ != nullptr) {
    params_paste_->setIcon(loadSvg(u":/resources/svg/paste.svg"_s, theme_));
  }
  if (params_apply_all_ != nullptr) {
    params_apply_all_->setIcon(loadSvg(u":/resources/svg/format_paint.svg"_s, theme_));
  }
  for (const auto& [id, row] : robot_rows_) {
    if (auto* trash = row->findChild<QToolButton*>()) {
      trash->setIcon(loadSvg(QLatin1String(kTrashIconPath), theme_));
    }
  }
}

void Scene3DConfigPanel::onAddModelClicked() {
  if (bound_dock_ == nullptr) {
    return;
  }
  switch (model_source_combo_->currentIndex()) {
    case 0: {  // File
#ifdef PJ_TARGET_WASM
      // Browser policy: the sandbox grants the URDF's bytes, never a path.
      QPointer<Scene3DConfigPanel> self(this);
      QPointer<Scene3DDockWidget> dock(bound_dock_);
      FileSelectionService::selectFileContent(
          this, tr("URDF files (*.urdf *.xml);;All files (*)"),
          [self, dock](FileSelectionService::Selection selection) mutable {
            if (self.isNull()) {
              return;
            }
            if (dock.isNull() || self->bound_dock_ != dock || selection.browser_name.isEmpty()) {
              return;
            }
            const ObjectTopicId id =
                dock->addRobotModelLayerFromContent(selection.browser_name, std::move(selection.bytes));
            if (id.id != 0) {
              self->addRobotRow(id.id, selection.browser_name, selection.browser_name);
            }
          });
#else
      QSettings settings;
      const QString start_dir = settings.value(QString::fromLatin1(kUrdfBrowseDirKey)).toString();
      const QString path = PJ::FileDialog::getOpenFileName(
          this, tr("Load URDF"), start_dir, tr("URDF files (*.urdf *.xml);;All files (*)"));
      if (path.isEmpty()) {
        return;
      }
      if (bound_dock_ == nullptr) {
        return;  // the modal event loop can outlive the dock
      }
      settings.setValue(QString::fromLatin1(kUrdfBrowseDirKey), QFileInfo(path).absolutePath());
      const uint32_t id = bound_dock_->addRobotModelLayer(path).id;
      if (id != 0) {
        addRobotRow(id, QFileInfo(path).fileName(), path);
      }
#endif
      break;
    }
    case 1: {  // Topic
      const auto topics = bound_dock_->robotDescriptionTopics();
      if (topics.isEmpty()) {
#ifdef PJ_TARGET_WASM
        openBrowserInfo(this, tr("Load robot model"), tr("No robot description topic in this dataset."));
#else
        MessageBox::information(this, tr("Load robot model"), tr("No robot description topic in this dataset."));
#endif
        return;
      }
#ifdef PJ_TARGET_WASM
      QPointer<Scene3DConfigPanel> self(this);
      QPointer<Scene3DDockWidget> dock(bound_dock_);
      openRobotDescriptionTopicPicker(this, topics, [self, dock](ObjectTopicId topic_id, const QString& name) {
        if (self.isNull() || dock.isNull() || self->bound_dock_ != dock) {
          return;
        }
        if (dock->addTopic(topic_id, sdk::BuiltinObjectType::kRobotDescription, name)) {
          self->addRobotRow(topic_id.id, name, name);
        }
      });
#else
      const auto picked = pickRobotDescriptionTopic(this, topics);
      if (!picked.has_value()) {
        return;
      }
      if (bound_dock_ == nullptr) {
        return;  // the modal event loop can outlive the dock
      }
      // addRobotRow is idempotent, so the row created here is harmless when the
      // layerAdded signal (or a pre-existing layer) would have added it anyway.
      if (bound_dock_->addTopic(picked->first, sdk::BuiltinObjectType::kRobotDescription, picked->second)) {
        addRobotRow(picked->first.id, picked->second, picked->second);
      }
#endif
      break;
    }
    case 2: {  // URL
#ifdef PJ_TARGET_WASM
      QPointer<Scene3DConfigPanel> self(this);
      QPointer<Scene3DDockWidget> dock(bound_dock_);
      openUrdfUrlPrompt(this, [self, dock](const QString& url) {
        if (self.isNull() || dock.isNull() || self->bound_dock_ != dock) {
          return;
        }
        const ObjectTopicId id = dock->addRobotModelLayerFromUrl(url);
        if (id.id != 0) {
          const QString file_name = QUrl(url).fileName();
          self->addRobotRow(id.id, file_name.isEmpty() ? url : file_name, url);
        }
      });
#else
      const auto url = promptUrdfUrl(this);
      if (!url.has_value()) {
        return;
      }
      if (bound_dock_ == nullptr) {
        return;  // the modal event loop can outlive the dock
      }
      const uint32_t id = bound_dock_->addRobotModelLayerFromUrl(*url).id;
      if (id != 0) {
        const QString file_name = QUrl(*url).fileName();
        addRobotRow(id, file_name.isEmpty() ? *url : file_name, *url);
      }
#endif
      break;
    }
    default:
      break;
  }
}

void Scene3DConfigPanel::addRobotRow(uint32_t topic_id_value, const QString& label, const QString& tooltip) {
  // Idempotent: rows are derived from dock state and rebuilt on every bind, and
  // both onAddModelClicked and the layerAdded signal can target the same id.
  const auto existing = std::find_if(
      robot_rows_.begin(), robot_rows_.end(), [&](const auto& pair) { return pair.first == topic_id_value; });
  if (existing != robot_rows_.end()) {
    return;
  }

  auto* row = new QWidget(this);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  // Match the grid's column gap so the name's right edge lines up.
  layout->setSpacing(PJ::theme::space(kGridHSpacing));
  auto* name = new QLineEdit(label, row);
#ifdef PJ_TARGET_WASM
  name->setObjectName(u"scene3dRobotRowName"_s);
#endif
  name->setReadOnly(true);
  name->setFocusPolicy(Qt::NoFocus);
  name->setAlignment(Qt::AlignCenter);
  name->setToolTip(tooltip);
  // Clicking the name field selects the robot: it binds the Settings host to the
  // layer's config widget (source combo / status / Retry). The id rides on a
  // property so the panel's eventFilter can route the press without per-row state.
  name->setProperty("robot_topic_id", topic_id_value);
  name->setCursor(Qt::PointingHandCursor);
  name->installEventFilter(this);
  layout->addWidget(name, 1);
  auto* trash = new QToolButton(row);
#ifdef PJ_TARGET_WASM
  trash->setObjectName(u"scene3dRobotRowTrash"_s);
  // Same flat styling as the topic-row trash buttons; the QSS also names this
  // robot-specific id so diagnostics and accessibility retain a stable handle.
#else
  // Same flat styling as the topic-row trash buttons.
  trash->setObjectName(u"curveTrashToggle"_s);
#endif
  trash->setAutoRaise(true);
  trash->setFocusPolicy(Qt::NoFocus);
  trash->setToolTip(tr("Remove this robot model"));
  trash->setIcon(loadSvg(QLatin1String(kTrashIconPath), theme_));
  sizeTrailingButton(trash);  // align with the Model/URDF add button column above
  layout->addWidget(trash);
  connect(trash, &QToolButton::clicked, this, [this, topic_id_value]() {
    if (bound_dock_ != nullptr) {
      ObjectTopicId topic_id;
      topic_id.id = topic_id_value;
      bound_dock_->removeTopic(topic_id);  // the row is dropped by onLayerRemoved
    }
  });
  robot_rows_layout_->addWidget(row);
  robot_rows_.emplace_back(topic_id_value, row);
  robot_rows_host_->show();  // first row makes the (otherwise collapsed) host visible

  // Mirror the layer's status onto the name tooltip — the cheap error surface
  // for load failures ("Fetch failed …" / "Failed to read URDF …") that would
  // otherwise be invisible until the row is clicked. Kept fresh via the signal.
  if (bound_dock_ != nullptr) {
    ObjectTopicId topic_id;
    topic_id.id = topic_id_value;
    if (auto* robot = qobject_cast<pj::scene3d::RobotModelLayer*>(bound_dock_->layerFor(topic_id))) {
      const QString status = robot->statusText();
      if (!status.isEmpty()) {
        name->setToolTip(status);
      }
      connect(
          robot, &pj::scene3d::RobotModelLayer::statusTextChanged, name, [name, tooltip](const QString& status_text) {
            name->setToolTip(status_text.isEmpty() ? tooltip : status_text);
          });
    }
  }
}

void Scene3DConfigPanel::showRobotLayerConfig(uint32_t topic_id_value) {
  if (bound_dock_ == nullptr) {
    return;
  }
  ObjectTopicId topic_id;
  topic_id.id = topic_id_value;
  ISceneLayer* layer = bound_dock_->layerFor(topic_id);
  if (layer == nullptr) {
    return;
  }
  // Robot layers aren't in the Topics list, so the list selection is unrelated;
  // last click wins on config_host_ (the list selection stays as-is).
  config_host_->setConfigWidget(layer->createConfigWidget(config_host_));
}

bool Scene3DConfigPanel::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::MouseButtonPress) {
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() == Qt::LeftButton) {
      const QVariant topic_id = watched->property("robot_topic_id");
      if (topic_id.isValid()) {
        showRobotLayerConfig(topic_id.toUInt());
      }
    }
  }
  return QWidget::eventFilter(watched, event);
}

void Scene3DConfigPanel::removeRobotRowFor(uint32_t topic_id_value) {
  const auto it = std::find_if(
      robot_rows_.begin(), robot_rows_.end(), [&](const auto& pair) { return pair.first == topic_id_value; });
  if (it == robot_rows_.end()) {
    return;
  }
  it->second->deleteLater();
  robot_rows_.erase(it);
  if (robot_rows_.empty() && robot_rows_host_ != nullptr) {
    robot_rows_host_->hide();  // collapse the form row again so no phantom gap remains
  }
}

void Scene3DConfigPanel::bindDock(Scene3DDockWidget* dock) {
  if (bound_dock_.data() == dock) {
    return;
  }
  disconnectFromDock();
  bound_dock_ = dock;

  layer_list_->clearRows();
  config_host_->clear();
  // Robot rows belong to the dock they were added to; a rebind starts from a
  // clean slate (the previous dock keeps its layers).
  for (const auto& [id, row] : robot_rows_) {
    row->deleteLater();
  }
  robot_rows_.clear();
  if (robot_rows_host_ != nullptr) {
    robot_rows_host_->hide();  // back to collapsed until this dock's rows are rebuilt
  }

  if (dock == nullptr) {
    populateFollowCombo();  // reset to "None"
    populateTrailCombo();
    updateSelectedLayerPane();
    return;
  }

  rebuildLayerList();
  loadControlsFromDock(dock);  // reflect THIS dock's look; controls are per-dock
  populateFollowCombo();       // reflect THIS dock's follow target + frame set
  populateTrailCombo();

  connect(dock, &SceneDockWidget::layerAdded, this, &Scene3DConfigPanel::onLayerAdded);
  connect(dock, &SceneDockWidget::layerRemoved, this, &Scene3DConfigPanel::onLayerRemoved);
  connect(dock, &SceneDockWidget::layerVisibilityChanged, this, &Scene3DConfigPanel::onLayerVisibilityChanged);
  connect(dock, &SceneDockWidget::layerWarningChanged, this, &Scene3DConfigPanel::onLayerWarningChanged);
  connect(dock, &SceneDockWidget::layerStatusChanged, this, [this](ObjectTopicId topic_id, const QString& status) {
    layer_list_->setRowStatus(static_cast<qint64>(topic_id.id), status);
  });
  // Keep the follow combo in step with the dock's frame set and follow target.
  connect(dock, &Scene3DDockWidget::availableFramesChanged, this, [this](const QList<pj::tf::FrameRow>&) {
    populateFollowCombo();
    populateTrailCombo();
  });
  connect(dock, &Scene3DDockWidget::followFrameChanged, this, [this](const QString&) { populateFollowCombo(); });
}

void Scene3DConfigPanel::disconnectFromDock() {
  if (bound_dock_ != nullptr) {
    disconnect(bound_dock_.data(), nullptr, this, nullptr);
  }
  bound_dock_ = nullptr;
}

void Scene3DConfigPanel::populateTrailCombo() {
  if (trail_frame_combo_ == nullptr) {
    return;
  }
  QSignalBlocker block(trail_frame_combo_);
  const QString previous = trail_frame_combo_->currentData().toString();
  trail_frame_combo_->clear();
  if (bound_dock_ != nullptr) {
    for (const auto& row : bound_dock_->availableFrames()) {
      const QString name = QString::fromStdString(row.name);
      const QString display = QString(row.depth * 2, QLatin1Char(' ')) + name;
      trail_frame_combo_->addItem(display, name);
    }
  }
  const int idx = trail_frame_combo_->findData(previous);
  if (idx >= 0) {
    trail_frame_combo_->setCurrentIndex(idx);  // keep the user's pick across refreshes
  }
  if (add_trail_button_ != nullptr) {
    add_trail_button_->setEnabled(trail_frame_combo_->count() > 0);
  }
}

void Scene3DConfigPanel::populateFollowCombo() {
  if (follow_frame_combo_ == nullptr) {
    return;
  }
  QSignalBlocker block(follow_frame_combo_);
  follow_frame_combo_->clear();
  follow_frame_combo_->addItem(tr("None"), QString());
  QString current;
  if (bound_dock_ != nullptr) {
    current = bound_dock_->currentFollowFrame();
    for (const auto& row : bound_dock_->availableFrames()) {
      const QString name = QString::fromStdString(row.name);
      const QString display = QString(row.depth * 2, QLatin1Char(' ')) + name;
      follow_frame_combo_->addItem(display, name);
    }
  }
  const int idx = follow_frame_combo_->findData(current);
  follow_frame_combo_->setCurrentIndex(idx >= 0 ? idx : 0);
  // Recenter is enabled only when the SELECTED item is a real frame — track the
  // combo's resolved selection, not the dock's raw follow string, so a follow
  // target that isn't (yet) in the tree shows "None" AND a disabled button rather
  // than an enabled button over a "None" label.
  if (recenter_button_ != nullptr) {
    recenter_button_->setEnabled(!follow_frame_combo_->currentData().toString().isEmpty());
  }
}

void Scene3DConfigPanel::rebuildLayerList() {
  if (bound_dock_ == nullptr) {
    layer_list_->clearRows();
    updateSelectedLayerPane();
    return;
  }

  std::vector<LayerRow> rows;
  const auto layers = bound_dock_->layers();
  rows.reserve(layers.size());
  for (const SceneLayerInfo& info : layers) {
    // Robot-model layers stay out of the Topics list; they get a dedicated row
    // under the Model/URDF selector, rebuilt here from dock state (so layout
    // restore and dock switches recover the rows — bindDock cleared them).
    if (info.object_type == sdk::BuiltinObjectType::kRobotDescription) {
      addRobotRow(info.topic_id.id, info.display_name, info.display_name);
      continue;
    }
    rows.push_back(rowFromLayerInfo(info));
  }
  layer_list_->setRows(rows);

  for (const LayerRow& row : rows) {
    const auto warning = bound_dock_->orphanState(topicFromRowId(row.id));
    layer_list_->setRowWarning(row.id, warning.is_orphan, warning.reason);
    layer_list_->setRowStatus(row.id, bound_dock_->layerStatus(topicFromRowId(row.id)));
  }
  updateSelectedLayerPane();
}

void Scene3DConfigPanel::onLayerSelectionChanged() {
  updateSelectedLayerPane();
}

void Scene3DConfigPanel::onLayerAdded(ObjectTopicId topic_id) {
  if (bound_dock_ == nullptr) {
    return;
  }
  ISceneLayer* layer = bound_dock_->layerFor(topic_id);
  if (layer == nullptr) {
    return;
  }
  const SceneLayerInfo info = layer->info();
  if (info.object_type == sdk::BuiltinObjectType::kRobotDescription) {
    // Robot layers get a Model/URDF row, not a Topics-list row. Idempotent, so
    // a row already created by onAddModelClicked is not duplicated here.
    addRobotRow(info.topic_id.id, info.display_name, info.display_name);
    return;
  }
  layer_list_->addRow(rowFromLayerInfo(info));
  const auto warning = bound_dock_->orphanState(topic_id);
  layer_list_->setRowWarning(static_cast<qint64>(topic_id.id), warning.is_orphan, warning.reason);
  layer_list_->setRowStatus(static_cast<qint64>(topic_id.id), bound_dock_->layerStatus(topic_id));
  // A new same-family sibling can enable the apply-to-family button.
  updateParamsToolbarState();
}

void Scene3DConfigPanel::onLayerRemoved(ObjectTopicId topic_id) {
  layer_list_->removeRow(static_cast<qint64>(topic_id.id));
  // Single removal path for robot rows: the bin button only calls removeTopic
  // and this signal drops the row, so other teardown paths stay consistent.
  removeRobotRowFor(topic_id.id);
  updateSelectedLayerPane();
}

void Scene3DConfigPanel::onLayerVisibilityChanged(ObjectTopicId topic_id, bool visible) {
  layer_list_->setRowVisible(static_cast<qint64>(topic_id.id), visible);
}

void Scene3DConfigPanel::onLayerWarningChanged(ObjectTopicId topic_id, bool warn, const QString& reason) {
  layer_list_->setRowWarning(static_cast<qint64>(topic_id.id), warn, reason);
}

void Scene3DConfigPanel::onStylesheetChanged(QString theme) {
  theme_ = theme;
  layer_list_->setTheme(std::move(theme));
  applyIcons();
}

void Scene3DConfigPanel::updateSelectedLayerPane() {
  config_host_->clear();
  if (bound_dock_ == nullptr) {
    return;
  }
  const auto selected = selectedTopicId();
  if (!selected.has_value()) {
    return;
  }
  ISceneLayer* layer = bound_dock_->layerFor(*selected);
  if (layer == nullptr) {
    return;
  }
  config_host_->setConfigWidget(layer->createConfigWidget(config_host_));
  updateParamsToolbarState();
}

void Scene3DConfigPanel::onCopyParams() {
  if (bound_dock_ == nullptr) {
    return;
  }
  const auto selected = selectedTopicId();
  if (!selected.has_value()) {
    return;
  }
  ISceneLayer* layer = bound_dock_->layerFor(*selected);
  const auto family = familyOf(*selected);
  if (layer == nullptr || !family.has_value()) {
    return;
  }
  layerParamClipboard() = LayerParamClipboard{serializeLayerParams(*layer), *family};
  updateParamsToolbarState();
}

void Scene3DConfigPanel::onPasteParams() {
  if (bound_dock_ == nullptr) {
    return;
  }
  const auto selected = selectedTopicId();
  if (!selected.has_value()) {
    return;
  }
  ISceneLayer* layer = bound_dock_->layerFor(*selected);
  const auto family = familyOf(*selected);
  const LayerParamClipboard& clip = layerParamClipboard();
  if (layer == nullptr || clip.xml.isEmpty() || !family.has_value() || clip.family != *family) {
    return;
  }
  if (applyLayerParams(*layer, clip.xml)) {
    // The visible config widget was built from the pre-paste values; rebuild it
    // so its controls reflect what was just applied.
    updateSelectedLayerPane();
  }
}

void Scene3DConfigPanel::onApplyParamsToFamily() {
  if (bound_dock_ == nullptr) {
    return;
  }
  const auto selected = selectedTopicId();
  if (!selected.has_value()) {
    return;
  }
  ISceneLayer* source = bound_dock_->layerFor(*selected);
  const auto family = familyOf(*selected);
  if (source == nullptr || !family.has_value()) {
    return;
  }
  const QString xml = serializeLayerParams(*source);
  if (xml.isEmpty()) {
    return;
  }
  // Push the source's params onto every other layer of the same family. The
  // source itself is left untouched, so its open config widget stays valid.
  for (const SceneLayerInfo& info : bound_dock_->layers()) {
    if (info.topic_id == *selected || info.family_name != *family) {
      continue;
    }
    if (ISceneLayer* target = bound_dock_->layerFor(info.topic_id); target != nullptr) {
      // Best-effort across siblings: the blob came from a valid serialize of a
      // same-family layer, so a single failure shouldn't abort the rest.
      [[maybe_unused]] const bool applied = applyLayerParams(*target, xml);
    }
  }
}

void Scene3DConfigPanel::updateParamsToolbarState() {
  if (params_copy_ == nullptr) {
    return;
  }
  std::optional<ObjectTopicId> selected;
  ISceneLayer* layer = nullptr;
  if (bound_dock_ != nullptr) {
    selected = selectedTopicId();
    if (selected.has_value()) {
      layer = bound_dock_->layerFor(*selected);
    }
  }
  const bool has_layer = layer != nullptr;

  QString selected_family;
  int same_family_siblings = 0;
  if (has_layer) {
    if (const auto family = familyOf(*selected); family.has_value()) {
      selected_family = *family;
      for (const SceneLayerInfo& info : bound_dock_->layers()) {
        if (!(info.topic_id == *selected) && info.family_name == selected_family) {
          ++same_family_siblings;
        }
      }
    }
  }

  const LayerParamClipboard& clip = layerParamClipboard();
  params_copy_->setEnabled(has_layer);
  params_paste_->setEnabled(
      has_layer && !clip.xml.isEmpty() && !selected_family.isEmpty() && clip.family == selected_family);
  params_apply_all_->setEnabled(has_layer && same_family_siblings > 0);
}

std::optional<QString> Scene3DConfigPanel::familyOf(ObjectTopicId topic_id) const {
  if (bound_dock_ == nullptr) {
    return std::nullopt;
  }
  for (const SceneLayerInfo& info : bound_dock_->layers()) {
    if (info.topic_id == topic_id) {
      return info.family_name;
    }
  }
  return std::nullopt;
}

std::optional<ObjectTopicId> Scene3DConfigPanel::selectedTopicId() const {
  if (layer_list_ == nullptr) {
    return std::nullopt;
  }
  const auto id = layer_list_->currentId();
  if (!id.has_value()) {
    return std::nullopt;
  }
  return topicFromRowId(*id);
}

std::vector<ObjectTopicId> Scene3DConfigPanel::topicOrderFromIds(const std::vector<qint64>& ids) const {
  std::vector<ObjectTopicId> ordered;
  ordered.reserve(ids.size());
  for (const qint64 id : ids) {
    ordered.push_back(topicFromRowId(id));
  }
  return ordered;
}

}  // namespace PJ
