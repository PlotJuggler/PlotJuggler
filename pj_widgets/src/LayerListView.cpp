// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/LayerListView.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDropEvent>
#include <QListView>
#include <QListWidget>
#include <QModelIndex>
#include <QMouseEvent>
#include <QPoint>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>

#include "pj_widgets/ElidingLabel.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/Scrollbar.h"
#include "pj_widgets/SvgUtil.h"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {
constexpr int kLayerIdRole = Qt::UserRole + 1;

constexpr auto kVisibilityOnPath = ":/resources/svg/visibility.svg";
constexpr auto kVisibilityOffPath = ":/resources/svg/visibility_off.svg";
constexpr auto kTrashIconPath = ":/resources/svg/trash.svg";

constexpr int kDefaultRowHeight = 20;
constexpr int kRowSpacing = 2;

class LayerRowWidget : public QWidget {
  Q_OBJECT
 public:
  LayerRowWidget(qint64 id, QString display_name, bool visible, QString theme, int row_height, QWidget* parent)
      : QWidget(parent),
        id_(id),
        current_theme_(std::move(theme)),
        row_height_(row_height),
        full_name_(std::move(display_name)) {
    name_ = new ElidingLabel(this);
    name_->setElideMode(Qt::ElideLeft);
    name_->setFullText(full_name_);
    name_->setToolTip(full_name_);
    name_->setAttribute(Qt::WA_TransparentForMouseEvents);
    // This widget is the sole authority on the label's visibility (see
    // resizeEvent); disable ElidingLabel's own hide-when-narrow so the two
    // don't fight over it in the sub-16px band.
    name_->setHideBelowWidth(0);

    eye_ = new QToolButton(this);
    eye_->setObjectName(u"curveVisibilityToggle"_s);
    eye_->setCheckable(true);
    eye_->setChecked(visible);
    eye_->setAutoRaise(true);
    eye_->setFocusPolicy(Qt::NoFocus);
    eye_->setToolTip(tr("Toggle layer visibility"));

    trash_ = new QToolButton(this);
    trash_->setObjectName(u"curveTrashToggle"_s);
    trash_->setAutoRaise(true);
    trash_->setFocusPolicy(Qt::NoFocus);
    trash_->setToolTip(tr("Remove this layer"));

    refreshIcons();

    connect(eye_, &QToolButton::toggled, this, [this](bool checked) {
      eye_->setIcon(loadSvg(checked ? kVisibilityOnPath : kVisibilityOffPath, current_theme_));
      emit visibilityToggled(id_, checked);
    });
    connect(trash_, &QToolButton::clicked, this, [this]() { emit removeClicked(id_); });
  }

  [[nodiscard]] LayerRow row() const {
    return {
        .id = id_,
        .name = full_name_,
        .visible = eye_->isChecked(),
        .warn = is_warning_,
        .warning_reason = warning_reason_};
  }

  [[nodiscard]] QString effectiveToolTip() const {
    if (is_warning_ && !warning_reason_.isEmpty()) {
      return warning_reason_;
    }
    return full_name_;
  }

  void setVisibleState(bool visible) {
    if (eye_->isChecked() == visible) {
      return;
    }
    QSignalBlocker block(eye_);
    eye_->setChecked(visible);
    eye_->setIcon(loadSvg(visible ? kVisibilityOnPath : kVisibilityOffPath, current_theme_));
  }

  void setDisplayName(const QString& name) {
    full_name_ = name;
    name_->setFullText(name);
    name_->setToolTip(effectiveToolTip());
  }

  void setWarningState(bool is_warning, const QString& reason) {
    if (is_warning_ == is_warning && warning_reason_ == reason) {
      return;
    }
    is_warning_ = is_warning;
    warning_reason_ = reason;
    applyWarningStyle();
    name_->setToolTip(effectiveToolTip());
  }

  void setTheme(const QString& theme) {
    current_theme_ = theme;
    refreshIcons();
    applyWarningStyle();
  }

  void setRowHeight(int row_height) {
    if (row_height == row_height_) {
      return;
    }
    row_height_ = row_height;
    refreshIcons();
    updateGeometry();
  }

  [[nodiscard]] QSize sizeHint() const override {
    return {0, row_height_};
  }

 signals:
  void visibilityToggled(qint64 id, bool visible);
  void removeClicked(qint64 id);
#ifdef PJ_TARGET_WASM
  void reorderPressed(QPoint global_pos);
  void reorderMoved(QPoint global_pos);
  void reorderReleased(QPoint global_pos);
#endif

 protected:
#ifdef PJ_TARGET_WASM
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton) {
      emit reorderPressed(event->globalPosition().toPoint());
      event->accept();
      return;
    }
    QWidget::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (event->buttons().testFlag(Qt::LeftButton)) {
      emit reorderMoved(event->globalPosition().toPoint());
      event->accept();
      return;
    }
    QWidget::mouseMoveEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton) {
      emit reorderReleased(event->globalPosition().toPoint());
      event->accept();
      return;
    }
    QWidget::mouseReleaseEvent(event);
  }
#endif

  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    const int h = height();
    const int total_w = width();
    const bool rtl = isRightToLeft();
    // Geometry is laid out for LTR (name | … | eye | trash at the right edge),
    // then mirrored horizontally for RTL so the buttons land on the leading
    // edge as a mirrored UI expects.
    const auto place = [&](QWidget* w, int x_ltr, int width_px) {
      w->setGeometry(rtl ? total_w - x_ltr - width_px : x_ltr, 0, width_px, h);
    };

    const int trash_x = total_w - h;
    place(trash_, trash_x, h);
    const int eye_x = trash_x - kRowSpacing - h;
    place(eye_, eye_x, h);

    const int name_x = kRowSpacing;
    const int name_right = eye_x - kRowSpacing;
    const int name_width = name_right - name_x;
    if (name_width < 1) {
      if (name_->isVisible()) {
        name_->setVisible(false);
      }
      return;
    }
    if (!name_->isVisible()) {
      name_->setVisible(true);
    }
    place(name_, name_x, name_width);
  }

 private:
  [[nodiscard]] theme::Theme frameworkTheme() const {
    return theme::themeFor(current_theme_.contains("light"));
  }

  void applyWarningStyle() {
    if (is_warning_) {
      // The semantic warning STATUS hue, not an interaction fill: the Highlight
      // family's pale nominal fill is a background tone with near-zero contrast
      // as text on the light data backdrop.
      const QColor warning = theme::status(theme::Status::Warning, frameworkTheme());
      name_->setStyleSheet(QStringLiteral("color: %1;").arg(warning.name()));
    } else {
      name_->setStyleSheet(QString{});
    }
  }

  void refreshIcons() {
    const QSize sz(row_height_, row_height_);
    eye_->setIconSize(sz);
    eye_->setIcon(loadSvg(eye_->isChecked() ? kVisibilityOnPath : kVisibilityOffPath, current_theme_));
    trash_->setIconSize(sz);
    trash_->setIcon(loadSvg(kTrashIconPath, current_theme_));
  }

  qint64 id_;
  QString current_theme_;
  int row_height_;
  QString full_name_;
  QString warning_reason_;
  bool is_warning_ = false;
  ElidingLabel* name_ = nullptr;
  QToolButton* eye_ = nullptr;
  QToolButton* trash_ = nullptr;
};

class LayerListWidget : public QListWidget {
  Q_OBJECT
 public:
  explicit LayerListWidget(QWidget* parent) : QListWidget(parent) {
    setSelectionMode(QAbstractItemView::SingleSelection);
#ifdef PJ_TARGET_WASM
    // QListWidget's platform drag object does not complete an InternalMove in
    // Qt/WASM. Row widgets feed the same insertion semantics through the
    // pointer-only fallback below; native keeps Qt's standard DnD path.
    setDragDropMode(QAbstractItemView::NoDragDrop);
#else
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
#endif
  }

#ifdef PJ_TARGET_WASM
  void beginPointerReorder(QListWidgetItem* item, const QPoint& global_pos) {
    pointer_drag_from_ = row(item);
    pointer_drag_start_ = global_pos;
    pointer_drag_active_ = false;
    setCurrentItem(item);
  }

  void updatePointerReorder(const QPoint& global_pos) {
    if (pointer_drag_from_ < 0 || pointer_drag_active_) {
      return;
    }
    pointer_drag_active_ = (global_pos - pointer_drag_start_).manhattanLength() >= QApplication::startDragDistance();
  }

  void finishPointerReorder(const QPoint& global_pos) {
    const int from = pointer_drag_from_;
    const bool active = pointer_drag_active_;
    pointer_drag_from_ = -1;
    pointer_drag_active_ = false;
    if (!active || from < 0) {
      return;
    }

    const QPoint local_pos = viewport()->mapFromGlobal(global_pos);
    int to = local_pos.y() < 0 ? 0 : count();
    const QPoint row_axis_pos(viewport()->rect().center().x(), local_pos.y());
    if (const QModelIndex index = indexAt(row_axis_pos); index.isValid()) {
      const QRect item_rect = visualRect(index);
      to = index.row() + (local_pos.y() > item_rect.center().y() ? 1 : 0);
    }
    emit rowMoved(from, to);
  }
#endif

 signals:
  void rowMoved(int from, int to);

 protected:
  void dropEvent(QDropEvent* event) override {
    if (event->source() != this) {
      event->ignore();
      return;
    }
    const int from = currentRow();
    int to = count();
    if (const QModelIndex idx = indexAt(event->position().toPoint()); idx.isValid()) {
      to = idx.row();
      if (dropIndicatorPosition() == QAbstractItemView::BelowItem) {
        ++to;
      }
    }
    event->setDropAction(Qt::IgnoreAction);
    event->accept();
    if (from >= 0) {
      emit rowMoved(from, to);
    }
  }

#ifdef PJ_TARGET_WASM
 private:
  QPoint pointer_drag_start_;
  int pointer_drag_from_ = -1;
  bool pointer_drag_active_ = false;
#endif
};

std::optional<LayerRow> storedRowForItem(const QListWidget* list, QListWidgetItem* item) {
  if (list == nullptr || item == nullptr) {
    return std::nullopt;
  }
  auto* row = qobject_cast<LayerRowWidget*>(list->itemWidget(item));
  if (row == nullptr) {
    return std::nullopt;
  }
  return row->row();
}

void detachItemWidgets(QListWidget* list) {
  if (list == nullptr) {
    return;
  }
  for (int i = 0; i < list->count(); ++i) {
    QListWidgetItem* item = list->item(i);
    QWidget* widget = list->itemWidget(item);
    if (widget == nullptr) {
      continue;
    }
    list->removeItemWidget(item);
    widget->hide();
    widget->setParent(nullptr);
    widget->deleteLater();
  }
}

}  // namespace

LayerListView::LayerListView(QWidget* parent) : QWidget(parent), current_theme_(currentTheme()) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(
      theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
      theme::space(theme::Space::None));
  root->setSpacing(theme::space(theme::Space::None));

  auto* list = new LayerListWidget(this);
  list_ = list;
  list_->setUniformItemSizes(true);
  list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list_->setResizeMode(QListView::Adjust);
  list_->setSpacing(theme::space(theme::Space::None));
  list_->setMinimumWidth(0);
  list_->setStyleSheet(QStringLiteral("QListWidget::item { padding: %1px; }").arg(theme::space(theme::Space::None)));
  list_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  list_->setFixedHeight(kDefaultRowHeight * 4 + 4);
  root->addWidget(list_);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

  connect(list_, &QListWidget::itemSelectionChanged, this, &LayerListView::selectionChanged);
  connect(list, &LayerListWidget::rowMoved, this, &LayerListView::onRowMoved, Qt::QueuedConnection);

  // The layer list scrolls with the canonical overlay pill, not a native bar.
  attachPillScrollbars(this);
}

void LayerListView::setRows(const std::vector<LayerRow>& rows) {
  const std::optional<qint64> selected = currentId();
  QListWidgetItem* to_select = nullptr;
  {
    const QSignalBlocker block(list_);
    detachItemWidgets(list_);
    list_->clear();
    items_.clear();
    for (const LayerRow& row : rows) {
      if (items_.find(row.id) != items_.end()) {
        continue;  // Ids must be unique; ignore duplicates (matches addRow).
      }
      auto* item = new QListWidgetItem(list_);
      installRowWidget(item, row);
      if (selected.has_value() && row.id == *selected) {
        to_select = item;
      }
    }
  }
  if (to_select != nullptr) {
    list_->setCurrentItem(to_select);
  } else if (list_->count() > 0) {
    list_->setCurrentRow(0);
  } else if (selected.has_value()) {
    emit selectionChanged();
  }
  updateMinimumHeight();
}

void LayerListView::addRow(const LayerRow& row) {
  if (items_.find(row.id) != items_.end()) {
    // Ids must be unique. addRow does not replace an existing row (that would
    // orphan the old item widget); use setRowName/setRowVisible to mutate, or
    // setRows to rebuild.
    return;
  }
  auto* item = new QListWidgetItem(list_);
  installRowWidget(item, row);
  if (list_->currentItem() == nullptr) {
    list_->setCurrentItem(item);
  }
  updateMinimumHeight();
}

void LayerListView::removeRow(qint64 id) {
  auto it = items_.find(id);
  if (it == items_.end()) {
    return;
  }
  QListWidgetItem* item = it->second;
  const int row = list_->row(item);
  if (QWidget* widget = list_->itemWidget(item)) {
    list_->removeItemWidget(item);
    widget->hide();
    widget->setParent(nullptr);
    widget->deleteLater();
  }
  delete list_->takeItem(row);
  items_.erase(it);
  updateMinimumHeight();
}

void LayerListView::clearRows() {
  detachItemWidgets(list_);
  list_->clear();
  items_.clear();
  updateMinimumHeight();
}

void LayerListView::updateMinimumHeight() {
  // Fixed (min == max) so the layout can neither stretch the list to its
  // natural QListWidget sizeHint nor collapse it below the 4-row floor.
  const int rows = std::clamp(list_->count(), 4, 6);
  list_->setFixedHeight(kDefaultRowHeight * rows + 4);
}

void LayerListView::setRowVisible(qint64 id, bool visible) {
  const auto it = items_.find(id);
  if (it == items_.end()) {
    return;
  }
  if (auto* row = qobject_cast<LayerRowWidget*>(list_->itemWidget(it->second))) {
    row->setVisibleState(visible);
  }
}

void LayerListView::setRowName(qint64 id, const QString& name) {
  const auto it = items_.find(id);
  if (it == items_.end()) {
    return;
  }
  if (auto* row = qobject_cast<LayerRowWidget*>(list_->itemWidget(it->second))) {
    row->setDisplayName(name);
    it->second->setToolTip(row->effectiveToolTip());
  }
}

void LayerListView::setRowWarning(qint64 id, bool warn, const QString& reason) {
  const auto it = items_.find(id);
  if (it == items_.end()) {
    return;
  }
  if (auto* row = qobject_cast<LayerRowWidget*>(list_->itemWidget(it->second))) {
    row->setWarningState(warn, reason);
    it->second->setToolTip(row->effectiveToolTip());
  }
}

void LayerListView::setCurrentId(qint64 id) {
  const auto it = items_.find(id);
  if (it == items_.end()) {
    list_->setCurrentRow(-1);
    return;
  }
  list_->setCurrentItem(it->second);
}

std::optional<qint64> LayerListView::currentId() const {
  const QListWidgetItem* item = list_->currentItem();
  if (item == nullptr) {
    return std::nullopt;
  }
  return item->data(kLayerIdRole).toLongLong();
}

std::vector<qint64> LayerListView::order() const {
  std::vector<qint64> ids;
  ids.reserve(static_cast<std::size_t>(list_->count()));
  for (int i = 0; i < list_->count(); ++i) {
    ids.push_back(list_->item(i)->data(kLayerIdRole).toLongLong());
  }
  return ids;
}

void LayerListView::setTheme(const QString& theme) {
  current_theme_ = theme;
  for (int i = 0; i < list_->count(); ++i) {
    QWidget* widget = list_->itemWidget(list_->item(i));
    if (auto* row = qobject_cast<LayerRowWidget*>(widget)) {
      row->setTheme(current_theme_);
    }
  }
}

void LayerListView::installRowWidget(QListWidgetItem* item, const LayerRow& row) {
  auto* row_widget = new LayerRowWidget(row.id, row.name, row.visible, current_theme_, kDefaultRowHeight, list_);
  if (row.warn) {
    row_widget->setWarningState(true, row.warning_reason);
  }
  item->setSizeHint(QSize(0, kDefaultRowHeight));
  item->setData(kLayerIdRole, QVariant::fromValue(static_cast<qlonglong>(row.id)));
  item->setToolTip(row_widget->effectiveToolTip());
  list_->setItemWidget(item, row_widget);
  items_[row.id] = item;

  connect(row_widget, &LayerRowWidget::visibilityToggled, this, &LayerListView::visibilityToggled);
  connect(row_widget, &LayerRowWidget::removeClicked, this, &LayerListView::removeRequested);
#ifdef PJ_TARGET_WASM
  auto* list = static_cast<LayerListWidget*>(list_);
  connect(row_widget, &LayerRowWidget::reorderPressed, list, [list, item](const QPoint& global_pos) {
    list->beginPointerReorder(item, global_pos);
  });
  connect(row_widget, &LayerRowWidget::reorderMoved, list, &LayerListWidget::updatePointerReorder);
  connect(row_widget, &LayerRowWidget::reorderReleased, list, &LayerListWidget::finishPointerReorder);
#endif
}

void LayerListView::setOrder(const std::vector<qint64>& ordered_ids) {
  if (ordered_ids != order()) {
    rebuildFromOrder(ordered_ids, currentId().value_or(-1));
  }
}

void LayerListView::rebuildFromOrder(const std::vector<qint64>& ordered_ids, qint64 select_id) {
  const bool had_selection = currentId().has_value();
  std::unordered_map<qint64, LayerRow> stored_rows;
  stored_rows.reserve(static_cast<std::size_t>(list_->count()));
  for (int i = 0; i < list_->count(); ++i) {
    QListWidgetItem* item = list_->item(i);
    if (auto stored = storedRowForItem(list_, item)) {
      stored_rows.emplace(stored->id, std::move(*stored));
    }
  }

  QListWidgetItem* to_select = nullptr;
  {
    const QSignalBlocker block(list_);
    detachItemWidgets(list_);
    list_->clear();
    items_.clear();
    for (const qint64 id : ordered_ids) {
      const auto it = stored_rows.find(id);
      if (it == stored_rows.end()) {
        continue;
      }
      auto* item = new QListWidgetItem(list_);
      installRowWidget(item, it->second);  // warning state travels in the LayerRow
      if (id == select_id) {
        to_select = item;
      }
    }
  }
  if (to_select != nullptr) {
    list_->setCurrentItem(to_select);
  } else if (had_selection && list_->count() > 0) {
    list_->setCurrentRow(0);
  } else if (had_selection) {
    emit selectionChanged();
  }
}

void LayerListView::onRowMoved(int from, int to) {
  std::vector<qint64> ids = order();
  if (from < 0 || from >= static_cast<int>(ids.size())) {
    return;
  }
  const qint64 moved = ids[static_cast<std::size_t>(from)];
  ids = detail::reorderIds(std::move(ids), from, to);
  rebuildFromOrder(ids, moved);
  emit reordered(ids);
}

namespace detail {

std::vector<qint64> reorderIds(std::vector<qint64> ids, int from, int to) {
  if (from < 0 || from >= static_cast<int>(ids.size())) {
    return ids;
  }
  int dst = std::clamp(to, 0, static_cast<int>(ids.size()));
  const qint64 moved = ids[static_cast<std::size_t>(from)];
  ids.erase(ids.begin() + from);
  if (dst > from) {
    --dst;
  }
  dst = std::clamp(dst, 0, static_cast<int>(ids.size()));
  ids.insert(ids.begin() + dst, moved);
  return ids;
}

}  // namespace detail

}  // namespace PJ

#include "LayerListView.moc"
