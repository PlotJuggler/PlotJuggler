// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/VisualizationPlaceholderWidget.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QMenu>
#include <QMimeData>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSize>
#include <QToolButton>
#include <algorithm>

#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/OwnedMenu.h"
#include "pj_widgets/SvgUtil.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

/// Side of a family button at its natural size and at the narrowest the row may shrink to.
constexpr int kButtonSide = 66;
constexpr int kMinButtonSide = 44;
/// Icon side inside a kButtonSide button; smaller buttons scale it by the same ratio.
constexpr int kIconSide = 48;

/// Width of a row of `count` buttons of `side`, with the layout's spacing and margins.
int rowWidth(const QLayout* layout, int count, int side) {
  const QMargins margins = layout->contentsMargins();
  return count * side + std::max(count - 1, 0) * layout->spacing() + margins.left() + margins.right();
}

QToolButton* makeIconButton(const QString& icon_path, const QString& tooltip, QWidget* parent) {
  auto* button = new QToolButton(parent);
  // Icon is theme-tinted by the caller via LoadSvg+setIcon after the
  // button is created; the placeholder also re-tints on theme changes.
  Q_UNUSED(icon_path);
  button->setIconSize(QSize(kIconSide, kIconSide));
  button->setFixedSize(kButtonSide, kButtonSide);
  button->setAutoRaise(true);
  button->setFocusPolicy(Qt::NoFocus);
  button->setToolTip(tooltip);
  // The pointing-hand cursor signals that every family icon is clickable.
  button->setCursor(Qt::PointingHandCursor);
  return button;
}

bool acceptsCatalogItems(const QMimeData* mime_data) {
  return mime_data != nullptr && mime_data->hasFormat(CurveTreeView::catalogItemsMimeType());
}

bool acceptCatalogDrag(QDropEvent* event) {
  if (event != nullptr && acceptsCatalogItems(event->mimeData())) {
    event->acceptProposedAction();
    return true;
  }
  return false;
}

bool dropCatalogItems(QDropEvent* event, VisualizationPlaceholderWidget* target) {
  const QMimeData* mime_data = event != nullptr ? event->mimeData() : nullptr;
  const QStringList keys = CurveTreeView::decodeCatalogKeys(mime_data);
  if (keys.empty()) {
    return false;
  }
  // A right-drag of exactly two curves carries the new_XY_axis mime — the
  // "create XY plot" gesture. Route it so the host builds an XY plot instead of
  // two ordinary curves; everything else is a normal catalog drop.
  if (keys.size() == 2 && mime_data != nullptr && mime_data->hasFormat(CurveTreeView::newXyAxisMimeType())) {
    emit target->catalogItemsXyRequested(keys);
  } else {
    emit target->catalogItemsDropped(keys);
  }
  event->acceptProposedAction();
  return true;
}

}  // namespace

VisualizationPlaceholderWidget::VisualizationPlaceholderWidget(QWidget* parent) : QWidget(parent) {
  setAcceptDrops(true);
  setObjectName(u"VisualizationPlaceholderWidget"_s);

  action_split_horizontal_ = new QAction(tr("&Split Horizontally"), this);
  connect(action_split_horizontal_, &QAction::triggered, this, [this]() { emit splitHorizontalRequested(); });

  action_split_vertical_ = new QAction(tr("&Split Vertically"), this);
  connect(action_split_vertical_, &QAction::triggered, this, [this]() { emit splitVerticalRequested(); });

  action_paste_ = new QAction(tr("&Paste"), this);
  action_paste_->setEnabled(false);
  connect(action_paste_, &QAction::triggered, this, [this]() { emit pasteRequested(); });

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(
      theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
      theme::space(theme::Space::None));
  layout->setSpacing(theme::space(theme::Space::Comfortable));
  // The buttons' current fixed size must not become the widget's minimum: minimumSizeHint
  // reports the narrowest row, and fitButtonsToWidth shrinks the buttons down to it.
  layout->setSizeConstraint(QLayout::SetNoConstraint);
  layout->addStretch(1);
  const struct {
    const char* path;
    const char* tooltip;
    const char* object_name;
    VisualizationKind kind;
  } specs[] = {
      {":/resources/svg/line_axis.svg", QT_TR_NOOP("Plot"), "buttonVizPlot", VisualizationKind::kPlot},
      {":/resources/svg/state_transitions.svg", QT_TR_NOOP("State Transitions"), "buttonVizStateTransitions",
       VisualizationKind::kStateTransitions},
      {":/resources/svg/image.svg", QT_TR_NOOP("2D"), "buttonVizScene2D", VisualizationKind::kScene2D},
      {":/resources/svg/cube.svg", QT_TR_NOOP("3D"), "buttonVizScene3D", VisualizationKind::kScene3D},
      // Always offered, even with no Widget plugin installed: the host's picker then says so and
      // points at the Marketplace, which is how the feature is discovered.
      {":/resources/svg/custom_widget.svg", QT_TR_NOOP("Custom Widget"), "buttonVizCustomWidget",
       VisualizationKind::kCustomWidget},
  };
  icon_buttons_.reserve(std::size(specs));
  for (const auto& spec : specs) {
    auto* button = makeIconButton(QString::fromLatin1(spec.path), tr(spec.tooltip), this);
    button->setObjectName(QString::fromLatin1(spec.object_name));
    button->setAcceptDrops(true);
    button->installEventFilter(this);
    const VisualizationKind kind = spec.kind;
    connect(button, &QToolButton::clicked, this, [this, kind]() { emit visualizationRequested(kind); });
    layout->addWidget(button);
    icon_buttons_.push_back({button, QString::fromLatin1(spec.path)});
  }
  layout->addStretch(1);
  // Initial paint at whatever theme is currently active. Subsequent
  // changes flow in through onStylesheetChanged.
  onStylesheetChanged(currentTheme());
}

void VisualizationPlaceholderWidget::setPasteActionEnabled(bool enabled) {
  if (action_paste_ != nullptr) {
    action_paste_->setEnabled(enabled);
  }
}

void VisualizationPlaceholderWidget::onStylesheetChanged(const QString& theme) {
  updateSplitActionIcons(theme);

  // Cache the Data Backdrop fill for the active theme and repaint. Qualified
  // PJ::theme:: to see past the `theme` parameter.
  const bool light = theme.contains(QStringLiteral("light"));
  backdrop_color_ = PJ::theme::surface(PJ::theme::Surface::DataBackdrop, PJ::theme::themeFor(light));
  update();

  theme_ = theme;
  renderIcons();
}

void VisualizationPlaceholderWidget::renderIcons() {
  // RenderSvgPixmap (not LoadSvg) so the central icons rasterize at
  // exactly their display size (with DPR baked in) and stay crisp. The
  // shared LoadSvg cache always renders to 64x64, which is downsampled
  // to 48x48 here -- visible blur on the larger placeholder buttons.
  for (const auto& entry : icon_buttons_) {
    const QSize icon_size = entry.button->iconSize();
    const QPixmap pixmap = renderSvgPixmap(entry.icon_path, theme_, icon_size, devicePixelRatioF());
    entry.button->setIcon(QIcon(pixmap));
  }
}

void VisualizationPlaceholderWidget::fitButtonsToWidth() {
  const auto count = static_cast<int>(icon_buttons_.size());
  const QMargins margins = layout()->contentsMargins();
  const int available = width() - margins.left() - margins.right();
  const int side = std::clamp((available - (count - 1) * layout()->spacing()) / count, kMinButtonSide, kButtonSide);
  if (icon_buttons_.front().button->width() == side) {
    return;
  }
  const int icon = qRound(side * double(kIconSide) / double(kButtonSide));
  for (const auto& entry : icon_buttons_) {
    entry.button->setFixedSize(side, side);
    entry.button->setIconSize(QSize(icon, icon));
  }
  renderIcons();
}

QSize VisualizationPlaceholderWidget::rowSize(int side) const {
  const QMargins margins = layout()->contentsMargins();
  return {rowWidth(layout(), static_cast<int>(icon_buttons_.size()), side), side + margins.top() + margins.bottom()};
}

QSize VisualizationPlaceholderWidget::sizeHint() const {
  return rowSize(kButtonSide);
}

QSize VisualizationPlaceholderWidget::minimumSizeHint() const {
  return rowSize(kMinButtonSide);
}

void VisualizationPlaceholderWidget::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  fitButtonsToWidth();
}

void VisualizationPlaceholderWidget::paintEvent(QPaintEvent* event) {
  if (backdrop_color_.isValid()) {
    QPainter painter(this);
    painter.fillRect(event->rect(), backdrop_color_);
  }
  QWidget::paintEvent(event);
}

void VisualizationPlaceholderWidget::contextMenuEvent(QContextMenuEvent* event) {
  if (event == nullptr) {
    return;
  }
  showSplitContextMenu(event->globalPos());
  event->accept();
}

bool VisualizationPlaceholderWidget::eventFilter(QObject* watched, QEvent* event) {
  Q_UNUSED(watched)
  if (event == nullptr) {
    return QWidget::eventFilter(watched, event);
  }

  switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove:
      return acceptCatalogDrag(static_cast<QDropEvent*>(event));
    case QEvent::Drop:
      return dropCatalogItems(static_cast<QDropEvent*>(event), this);
    case QEvent::ContextMenu:
      showSplitContextMenu(static_cast<QContextMenuEvent*>(event)->globalPos());
      event->accept();
      return true;
    default:
      break;
  }
  return QWidget::eventFilter(watched, event);
}

void VisualizationPlaceholderWidget::dragEnterEvent(QDragEnterEvent* event) {
  if (acceptCatalogDrag(event)) {
    return;
  }
  QWidget::dragEnterEvent(event);
}

void VisualizationPlaceholderWidget::dragMoveEvent(QDragMoveEvent* event) {
  if (acceptCatalogDrag(event)) {
    return;
  }
  QWidget::dragMoveEvent(event);
}

void VisualizationPlaceholderWidget::dropEvent(QDropEvent* event) {
  if (dropCatalogItems(event, this)) {
    return;
  }
  QWidget::dropEvent(event);
}

void VisualizationPlaceholderWidget::showSplitContextMenu(const QPoint& global_pos) {
  emit contextMenuAboutToShow();
  updateSplitActionIcons(currentTheme());

  auto* menu = new QMenu(this);
  menu->setObjectName(u"PJMenu"_s);
  menu->setProperty("categorySeparators", true);
  menu->addAction(action_paste_);
  menu->addSeparator();
  menu->addAction(action_split_horizontal_);
  menu->addAction(action_split_vertical_);
  showOwnedMenu(menu, global_pos);
}

void VisualizationPlaceholderWidget::updateSplitActionIcons(const QString& theme) {
  action_paste_->setIcon(QIcon(loadSvg(":/resources/svg/paste.svg", theme)));
  action_split_horizontal_->setIcon(QIcon(loadSvg(":/resources/svg/add_column.svg", theme)));
  action_split_vertical_->setIcon(QIcon(loadSvg(":/resources/svg/add_row.svg", theme)));
}

}  // namespace PJ
