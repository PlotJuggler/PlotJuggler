// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/CurveEditor.h"

#include <qwt_plot_curve.h>
#include <qwt_text.h>

#include <QAbstractItemView>
#include <QDropEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QPushButton>
#include <QString>
#include <QToolButton>
#include <QWidgetAction>
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/StateTransitionsController.h"
#include "pj_widgets/ColorPickerPopup.h"
#include "pj_widgets/ElidingLabel.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/Search.h"
#include "pj_widgets/SvgButton.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_CurveEditor.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// Inline-row geometry. Every inner widget sits flush with the row's
// outer border — no internal padding. The swatch is a square anchored
// to the left, eye + trash are squares anchored to the right, all
// scaling to the row's height so they maintain 1:1 aspect. The name
// takes whatever horizontal space is left.
//
// The row height tracks the global icon-size scrubber so larger icons
// don't clip; the default 20 matches MainWindow's first-launch icon
// size and the original compact-list design.
constexpr int kDefaultRowHeight = 20;
constexpr int kRowSpacing = 2;
// Horizontal breathing room between a row's content and the panel edges —
// without it the name text (and a swatch-less strip row especially) sits flush
// against the panel border.
constexpr int kRowEdgeInset = 6;

constexpr auto kCurveNameRole = Qt::UserRole;
constexpr auto kCurveDisplayNameRole = Qt::UserRole + 1;

constexpr auto kVisibilityOnPath = ":/resources/svg/visibility.svg";
constexpr auto kVisibilityOffPath = ":/resources/svg/visibility_off.svg";
constexpr auto kMarkersOnPath = ":/resources/svg/markers.svg";
constexpr auto kMarkersOffPath = ":/resources/svg/markers_off.svg";
constexpr auto kTrashIconPath = ":/resources/svg/trash.svg";
// Marker-scope row glyphs: Dataset = a single database (one dataset's own
// markers); Global = a grid of tiles (every loaded dataset), so the two rows
// read as different scopes at a glance, not just different labels.
constexpr auto kDatasetScopeIconPath = ":/resources/svg/database.svg";
constexpr auto kGlobalScopeIconPath = ":/resources/svg/grid_view.svg";
// objectName of each footer row, so tests can find them.
constexpr auto kMarkerScopeRowName = "markerScopeRow";
// A footer row's (dataset, scope) key, so a visibility signal finds its row.
constexpr auto kMarkerScopeDatasetProperty = "markerScopeDataset";
constexpr auto kMarkerScopeScopeProperty = "markerScopeScope";
// objectName of a footer row's scope glyph: the QSS rule keyed on it keeps the
// glyph flat like the row toggles.
constexpr auto kMarkerScopeGlyphName = "markerScopeGlyph";
// The glyph is drawn this much smaller than the row so it reads as a marker,
// not a button.
constexpr int kMarkerScopeGlyphInset = 6;

// Property keys tagged onto per-row QToolButtons so onStylesheetChanged
// can find them via findChildren and re-tint without rebuilding rows.
constexpr auto kVisibilityButtonProperty = "pj.curveEditor.visibilityButton";
constexpr auto kMarkersButtonProperty = "pj.curveEditor.markersButton";
constexpr auto kTrashButtonProperty = "pj.curveEditor.trashButton";
constexpr auto kColorButtonProperty = "pj.curveEditor.colorButton";

class CurveColorButton : public QPushButton {
 public:
  explicit CurveColorButton(QColor color, QWidget* parent = nullptr) : QPushButton(parent), color_(color) {
    setCursor(Qt::PointingHandCursor);
    setFlat(true);
    setFocusPolicy(Qt::NoFocus);
  }

  void setColor(QColor color) {
    if (color_ == color) {
      return;
    }
    color_ = color;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color_);

    const int extent = std::max(6, std::min(width(), height()) - 6);
    const QRectF swatch_rect((width() - extent) / 2.0, (height() - extent) / 2.0, extent, extent);
    const qreal radius = theme::radius(theme::Radius::Input);
    painter.drawRoundedRect(swatch_rect, radius, radius);
  }

 private:
  QColor color_;
};

// Non-interactive glyph in the swatch slot of a marker-scope footer row: says
// "this row is a scope's, not one curve's" where a curve row shows its color.
// An SvgButton (self-retinting) made mouse-transparent, sized to the row.
SvgButton* makeMarkerScopeGlyph(const char* icon_path, int row_height) {
  auto* glyph = new SvgButton(QString::fromLatin1(icon_path));
  glyph->setObjectName(QString::fromLatin1(kMarkerScopeGlyphName));
  glyph->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  glyph->setExtent(row_height, std::max(kMarkerScopeGlyphInset, row_height - kMarkerScopeGlyphInset));
  return glyph;
}

// Custom row widget. Uses explicit geometry instead of a QHBoxLayout so
// the markers + eye + trash icons pin to the right edge regardless of
// available width — they never get pushed out, no jitter while resizing,
// and the name takes the leftover middle (or hides). The swatch is
// left-anchored, the buttons are right-anchored, the name fills (or
// vanishes from) the gap between them.
//
// swatch/markers/eye/trash are each optional (null = absent from this row):
// a curve row carries all four, a State Transitions strip row omits swatch
// and markers, and a marker-scope footer row puts its scope glyph in the
// swatch slot and omits eye and trash (glyph + name + markers). resizeEvent
// anchors whichever of {trash, eye, markers} are present right-to-left,
// skipping the absent ones.
class CurveRowWidget : public QWidget {
 public:
  CurveRowWidget(
      QWidget* swatch, ElidingLabel* name, QToolButton* markers, QToolButton* eye, QToolButton* trash, int row_height,
      QWidget* parent)
      : QWidget(parent),
        swatch_(swatch),
        name_(name),
        markers_(markers),
        eye_(eye),
        trash_(trash),
        row_height_(row_height) {
    if (swatch_ != nullptr) {
      swatch_->setParent(this);
    }
    name_->setParent(this);
    if (markers_ != nullptr) {
      markers_->setParent(this);
    }
    if (eye_ != nullptr) {
      eye_->setParent(this);
    }
    if (trash_ != nullptr) {
      trash_->setParent(this);
    }
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
  }

  void setRowHeight(int row_height) {
    if (row_height == row_height_) {
      return;
    }
    row_height_ = row_height;
    updateGeometry();
  }

  [[nodiscard]] QSize sizeHint() const override {
    // Width is whatever the listWidget viewport gives us; the row's
    // height tracks the icon-size scrubber so icons grow with it.
    return {0, row_height_};
  }

 protected:
  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    const int h = height();
    const int total_w = width();

    // Every inner widget is a square of side `h` (the row height), so
    // each one is 1:1 and flush with the top + bottom borders. The
    // swatch anchors left (when the row has one); both ends keep
    // kRowEdgeInset off the panel border.
    if (swatch_ != nullptr) {
      swatch_->setGeometry(kRowEdgeInset, 0, h, h);
    }

    // Right-anchor whichever of {trash, eye, markers} this row carries,
    // left-to-right, skipping the absent ones (see the class comment for
    // which rows omit which). name_right tracks the left edge of the
    // leftmost placed button, or the panel edge if none are present.
    int name_right = total_w - kRowEdgeInset;
    for (QToolButton* button : {trash_, eye_, markers_}) {
      if (button == nullptr) {
        continue;
      }
      const int x = name_right - h;
      button->setGeometry(x, 0, h, h);
      name_right = x - kRowSpacing;
    }

    // Name lives between the swatch (when present) and the leftmost right-
    // anchored button. If the gap is too small to be readable, hide it; the
    // buttons stay put.
    const int name_x = kRowEdgeInset + (swatch_ != nullptr ? h + kRowSpacing : 0);
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
    name_->setGeometry(name_x, 0, name_width, h);
  }

 private:
  QWidget* swatch_;
  ElidingLabel* name_;
  QToolButton* markers_;
  QToolButton* eye_;
  QToolButton* trash_;
  int row_height_;
};

}  // namespace

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent), ui_(new Ui::CurveEditor) {
  ui_->setupUi(this);

  // Curves header: filter line edit + kebab menu, mirroring the Datasets
  // header. The kebab currently hosts a single "Clear all curves" action;
  // future panel-level options (sort, hide-invisible, etc.) hang here.
  auto* curves_menu = new QMenu(this);
  curves_menu->setObjectName(u"PJMenu"_s);
  auto* clear_all_button = new QPushButton(tr("Clear all curves"), curves_menu);
  clear_all_button->setFlat(true);
  clear_all_button->setProperty("destructive", true);
  clear_all_button->setIcon(loadSvg(":/resources/svg/trash.svg", current_theme_));
  connect(clear_all_button, &QPushButton::clicked, this, [this, curves_menu]() {
    curves_menu->hide();
    if (state_controller_ != nullptr) {
      state_controller_->removeAllSeries();
      return;
    }
    if (plot_ == nullptr) {
      return;
    }
    // Snapshot curve names first — removeCurve fires curveListChanged which
    // mutates the underlying list, so we can't iterate it live.
    QStringList names;
    for (const auto& info : plot_->curveList()) {
      if (info.curve != nullptr) {
        names << info.source_name;
      }
    }
    for (const QString& name : names) {
      plot_->removeCurve(name);
    }
    plot_->replot();
    emit plot_->undoableChange();
  });
  auto* clear_all_action = new QWidgetAction(curves_menu);
  clear_all_action->setDefaultWidget(clear_all_button);
  curves_menu->addAction(clear_all_action);

  connect(ui_->buttonCurvesMenu, &QToolButton::clicked, this, [this, curves_menu]() {
    // Right-align the popup with the kebab: open at the button's bottom-
    // left, then shift left by (menu_width - button_width) so the menu's
    // right edge sits flush with the button's right edge. menu->width()
    // is only meaningful after popup() lays out, so we reposition after
    // the initial show — popup() is non-blocking.
    auto* btn = ui_->buttonCurvesMenu;
    const QPoint bottom_left = btn->mapToGlobal(QPoint(0, btn->height()));
    curves_menu->popup(bottom_left);
    const int shift = curves_menu->width() - btn->width();
    if (shift > 0) {
      curves_menu->move(bottom_left.x() - shift, bottom_left.y());
    }
  });
  connect(ui_->filterCurves, &Search::textChanged, this, &CurveEditor::onFilterChanged);
  // Enter while typing drops focus — restores any sibling header chrome
  // via the focus-out branch of eventFilter without a stray click.
  connect(ui_->filterCurves, &Search::returnPressed, ui_->filterCurves->lineEdit(), &QLineEdit::clearFocus);
  ui_->filterCurves->lineEdit()->installEventFilter(this);
  // Lock the header row to its natural height so hiding label/kebab on
  // filter focus doesn't shift the line edit vertically.
  ui_->widgetLabelCurves->layout()->activate();
  ui_->widgetLabelCurves->setFixedHeight(ui_->widgetLabelCurves->layout()->sizeHint().height());

  // Progressive collapse runs off CurveEditor::resizeEvent (our own
  // width), not off the header band's QWidget — the band can't shrink
  // below the sum of its content's natural sizes, so it would never
  // trigger a hide threshold on its own.
  ui_->listWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  // Adjust mode tells the view to re-issue per-item geometry on resize
  // (otherwise items keep their first-laid-out width and the rows never
  // shrink with the viewport).
  ui_->listWidget->setResizeMode(QListView::Adjust);
  // Rows sit flush against each other — no inter-item gap.
  ui_->listWidget->setSpacing(PJ::theme::space(theme::Space::None));
  // Strip-mode drag-reorder (see eventFilter): InternalMove supplies the drop
  // indicator, but the drop itself is intercepted and re-routed through the
  // controller — the exact LayerListView pattern, which keeps the item widgets
  // alive (a real InternalMove would strip them).
  ui_->listWidget->setDefaultDropAction(Qt::MoveAction);
  ui_->listWidget->viewport()->installEventFilter(this);

  // QHBoxLayout's minimumSize is the sum of every child's natural
  // minimum. The QLabel ("Curves") and the QLineEdit have non-zero
  // implicit minimums, which would keep the header band — and the
  // CurveEditor as a whole — from ever shrinking past ~96 px. Zero
  // them out so the layout can actually collapse and our resizeEvent
  // threshold has something to act on.
  setMinimumWidth(0);
  ui_->widgetLabelCurves->setMinimumWidth(0);
  ui_->labelCurves->setMinimumWidth(0);
  ui_->filterCurves->setMinimumWidth(0);
  ui_->listWidget->setMinimumWidth(0);
}

CurveEditor::~CurveEditor() {
  delete ui_;
}

void CurveEditor::setPlot(PlotWidget* plot) {
  if (plot_ == plot) {
    return;
  }
  if (curve_list_connection_) {
    QObject::disconnect(curve_list_connection_);
  }
  if (curve_color_connection_) {
    QObject::disconnect(curve_color_connection_);
  }
  if (marker_scopes_connection_) {
    QObject::disconnect(marker_scopes_connection_);
  }
  if (marker_visibility_connection_) {
    QObject::disconnect(marker_visibility_connection_);
  }
  if (plot_destroyed_connection_) {
    QObject::disconnect(plot_destroyed_connection_);
  }
  plot_ = plot;
  clearActivePicker();
  if (plot_ != nullptr && state_controller_ != nullptr) {
    // The two bindings are mutually exclusive; a real plot bind wins.
    QObject::disconnect(state_series_connection_);
    QObject::disconnect(state_destroyed_connection_);
    state_controller_ = nullptr;
  }
  // Plot curves keep a fixed list order; only strip rows drag-reorder.
  ui_->listWidget->setDragDropMode(QAbstractItemView::NoDragDrop);
  if (plot_ != nullptr) {
    curve_list_connection_ = connect(plot_, &PlotWidgetBase::curveListChanged, this, &CurveEditor::refresh);
    curve_color_connection_ = connect(plot_, &PlotWidget::curveColorChanged, this, &CurveEditor::onCurveColorChanged);
    marker_scopes_connection_ = connect(plot_, &PlotWidget::datasetMarkerScopesChanged, this, &CurveEditor::refresh);
    marker_visibility_connection_ =
        connect(plot_, &PlotWidget::markerScopeVisibilityChanged, this, &CurveEditor::onMarkerScopeVisibilityChanged);
    plot_destroyed_connection_ = connect(plot_, &QObject::destroyed, this, [this]() {
      plot_ = nullptr;
      clearActivePicker();
      ui_->listWidget->clear();
      clearMarkerScopeRows();
    });
  }
  refresh();
}

void CurveEditor::setStateTransitions(StateTransitionsController* controller) {
  if (state_controller_ == controller) {
    return;
  }
  QObject::disconnect(state_series_connection_);
  QObject::disconnect(state_destroyed_connection_);
  state_controller_ = controller;
  if (state_controller_ != nullptr) {
    // Strip bind wins: release any plot binding (same exclusivity as setPlot).
    if (plot_ != nullptr) {
      QObject::disconnect(curve_list_connection_);
      QObject::disconnect(curve_color_connection_);
      QObject::disconnect(marker_scopes_connection_);
      QObject::disconnect(marker_visibility_connection_);
      QObject::disconnect(plot_destroyed_connection_);
      plot_ = nullptr;
      clearActivePicker();
    }
    state_series_connection_ =
        connect(state_controller_, &StateTransitionsController::seriesListChanged, this, &CurveEditor::refresh);
    state_destroyed_connection_ = connect(state_controller_, &QObject::destroyed, this, [this]() {
      state_controller_ = nullptr;
      ui_->listWidget->setDragDropMode(QAbstractItemView::NoDragDrop);
      ui_->listWidget->clear();
    });
    // Rows reorder by drag, mirroring the 3D panel's layer list; the actual
    // move happens in the controller (see eventFilter's Drop branch).
    ui_->listWidget->setDragDropMode(QAbstractItemView::InternalMove);
    refresh();
  } else {
    ui_->listWidget->setDragDropMode(QAbstractItemView::NoDragDrop);
    if (plot_ == nullptr) {
      ui_->listWidget->clear();
      clearMarkerScopeRows();
    }
  }
}

void CurveEditor::refresh() {
  // Clear without firing selection-change handlers; we re-evaluate at the end.
  QSignalBlocker block_list(ui_->listWidget);
  ui_->listWidget->clear();
  clearMarkerScopeRows();
  // Row swatches are about to be destroyed — invalidate any cached pointer.
  clearActivePicker();

  if (state_controller_ != nullptr) {
    // Strip rows: keyed by the numeric row id; an invalid color = no swatch
    // (state colors are hash-derived, deliberately not editable).
    for (const auto& entry : state_controller_->seriesEntries()) {
      appendRow(QString::number(entry.row_id), entry.name, QColor(), entry.visible, /*markers_visible=*/false);
    }
    applyFilter();
    return;
  }
  if (plot_ == nullptr) {
    return;
  }

  for (const auto& info : plot_->curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    appendRow(
        info.source_name, info.curve->title().text(), info.curve->pen().color(), info.curve->isVisible(),
        info.show_markers);
  }

  // Up to two marker-scope rows per dataset (Dataset then Global), each only
  // when its object topic exists on this plot, in the pinned footer under the
  // list so they stay in reach however long the curve list scrolls. The label
  // names the dataset only when this plot draws more than one.
  const std::vector<MarkerScopeKey>& scope_rows = plot_->markerScopeRows();
  const bool single_dataset = plot_->curveDatasets().size() == 1;
  for (const auto& [dataset_id, scope] : scope_rows) {
    const QString scope_name = scope == MarkerScope::kDataset ? tr("Dataset markers") : tr("Global markers");
    const QString label =
        single_dataset ? scope_name : tr("%1 — %2").arg(scope_name, plot_->datasetDisplayName(dataset_id));
    appendMarkerScopeRow(dataset_id, scope, label, plot_->datasetMarkerScopeVisible(dataset_id, scope));
  }
  ui_->markerScopesFooter->setVisible(!scope_rows.empty());

  // Preserve the active filter across refreshes.
  applyFilter();
}

void CurveEditor::appendRow(
    const QString& curve_key, const QString& display_name, QColor color, bool visible, bool markers_visible) {
  auto* item = new QListWidgetItem();
  item->setData(kCurveNameRole, curve_key);
  item->setData(kCurveDisplayNameRole, display_name);

  // Children are constructed without a parent — CurveRowWidget's ctor
  // reparents them in one place so resizeEvent can pin geometry directly
  // (sizes scale to the row height, so no setFixedSize here).
  // An invalid color means "this row has no editable color" (strip series):
  // no swatch is created and the name starts at the row's left edge.
  CurveColorButton* swatch = nullptr;
  if (color.isValid()) {
    swatch = new CurveColorButton(color);
    swatch->setProperty(kColorButtonProperty, curve_key);
    connect(swatch, &QPushButton::clicked, this, [this, curve_key, swatch]() { onSwatchClicked(curve_key, swatch); });
  }

  // Curve-visibility eye gates the curve itself; the markers toggle gates this
  // curve's contribution to the plot-markers overlay (CurveInfo::show_markers).
  auto* visibility = makeToggleButton(
      u"curveVisibilityToggle"_s, kVisibilityButtonProperty, kVisibilityOnPath, kVisibilityOffPath, visible,
      tr("Toggle curve visibility"), [this, curve_key](bool checked) { onVisibilityToggled(curve_key, checked); });
  // The markers toggle is meaningful only for real curves (plot_ bound); State
  // Transitions strip rows have no plot-markers overlay, so they omit it. The
  // eye still applies to strip rows via onVisibilityToggled.
  QToolButton* markers = nullptr;
  if (plot_ != nullptr) {
    markers = makeToggleButton(
        u"curveMarkersToggle"_s, kMarkersButtonProperty, kMarkersOnPath, kMarkersOffPath, markers_visible,
        tr("Toggle marker visibility"), [this, curve_key](bool checked) { onMarkersToggled(curve_key, checked); });
  }

  auto* name_label = new ElidingLabel();
  name_label->setObjectName(u"curveNameLabel"_s);
  // Curve names are typically topic paths (`/foo/bar/leaf`); elide from
  // the left so the meaningful leaf stays visible as the row narrows.
  name_label->setElideMode(Qt::ElideLeft);
  name_label->setFullText(display_name);
  name_label->setToolTip(curve_key);

  auto* trash = new QToolButton();
  trash->setObjectName(u"curveTrashToggle"_s);
  trash->setProperty(kTrashButtonProperty, curve_key);
  trash->setAutoRaise(true);
  trash->setFocusPolicy(Qt::NoFocus);
  trash->setIconSize(QSize(row_height_, row_height_));
  trash->setIcon(loadSvg(kTrashIconPath, current_theme_));
  trash->setToolTip(tr("Remove this curve from its plot"));
  connect(trash, &QToolButton::clicked, this, [this, curve_key]() {
    if (state_controller_ != nullptr) {
      state_controller_->removeSeries(curve_key.toULongLong());
      return;
    }
    if (plot_ == nullptr) {
      return;
    }
    plot_->removeCurve(curve_key);
    plot_->replot();
    emit plot_->undoableChange();
  });

  auto* row_widget =
      new CurveRowWidget(swatch, name_label, markers, visibility, trash, row_height_, /*parent=*/nullptr);
  ui_->listWidget->addItem(item);
  item->setSizeHint(QSize(0, row_height_));
  ui_->listWidget->setItemWidget(item, row_widget);
}

QToolButton* CurveEditor::makeToggleButton(
    const QString& object_name, const char* property_key, const char* on_path, const char* off_path, bool checked,
    const QString& tooltip, std::function<void(bool)> on_toggled) {
  auto* button = new QToolButton();
  button->setObjectName(object_name);
  button->setProperty(property_key, true);
  button->setCheckable(true);
  button->setAutoRaise(true);
  button->setFocusPolicy(Qt::NoFocus);
  button->setIconSize(QSize(row_height_, row_height_));
  button->setChecked(checked);
  button->setIcon(loadSvg(checked ? on_path : off_path, current_theme_));
  button->setToolTip(tooltip);
  connect(
      button, &QToolButton::toggled, this,
      [this, button, on_path, off_path, on_toggled = std::move(on_toggled)](bool is_checked) {
        button->setIcon(loadSvg(is_checked ? on_path : off_path, current_theme_));
        on_toggled(is_checked);
      });
  return button;
}

void CurveEditor::appendMarkerScopeRow(DatasetId dataset_id, MarkerScope scope, const QString& label, bool visible) {
  const bool is_dataset_scope = scope == MarkerScope::kDataset;

  SvgButton* glyph = makeMarkerScopeGlyph(is_dataset_scope ? kDatasetScopeIconPath : kGlobalScopeIconPath, row_height_);
  auto* name_label = new ElidingLabel();
  name_label->setObjectName(u"curveNameLabel"_s);
  name_label->setElideMode(Qt::ElideLeft);
  name_label->setFullText(label);
  name_label->setToolTip(label);
  QFont name_font = name_label->font();
  name_font.setBold(true);
  name_label->setFont(name_font);

  auto* markers = makeToggleButton(
      u"curveMarkersToggle"_s, kMarkersButtonProperty, kMarkersOnPath, kMarkersOffPath, visible,
      is_dataset_scope ? tr("Show or hide this dataset's markers on this plot")
                       : tr("Show or hide this dataset's global markers on this plot"),
      [this, dataset_id, scope](bool checked) {
        if (plot_ != nullptr) {
          plot_->setDatasetMarkerScopeVisible(dataset_id, scope, checked);
        }
      });

  auto* row_widget = new CurveRowWidget(glyph, name_label, markers, nullptr, nullptr, row_height_, /*parent=*/nullptr);
  row_widget->setObjectName(QString::fromLatin1(kMarkerScopeRowName));
  row_widget->setProperty(kMarkerScopeDatasetProperty, dataset_id);
  row_widget->setProperty(kMarkerScopeScopeProperty, static_cast<int>(scope));
  row_widget->setFixedHeight(row_height_);
  ui_->markerScopesLayout->addWidget(row_widget);
}

void CurveEditor::clearMarkerScopeRows() {
  qDeleteAll(markerScopeRows());  // a deleted child leaves its layout by itself
  ui_->markerScopesFooter->hide();
}

QList<QWidget*> CurveEditor::markerScopeRows() const {
  QList<QWidget*> rows;
  for (int i = 0; i < ui_->markerScopesLayout->count(); ++i) {
    if (QWidget* row = ui_->markerScopesLayout->itemAt(i)->widget()) {
      rows.push_back(row);
    }
  }
  return rows;
}

void CurveEditor::onMarkerScopeVisibilityChanged(DatasetId dataset_id, MarkerScope scope, bool visible) {
  if (plot_ == nullptr) {
    return;
  }
  for (QWidget* row : markerScopeRows()) {
    if (row->property(kMarkerScopeDatasetProperty).toUInt() != dataset_id ||
        row->property(kMarkerScopeScopeProperty).toInt() != static_cast<int>(scope)) {
      continue;
    }
    // setChecked re-enters setDatasetMarkerScopeVisible with the same value: a no-op.
    if (auto* toggle = row->findChild<QToolButton*>(u"curveMarkersToggle"_s)) {
      toggle->setChecked(visible);
    }
    return;
  }
  // No row for it yet — refresh() builds it with the right state.
}

void CurveEditor::forEachRow(const std::function<void(QWidget*)>& visit) const {
  for (int i = 0; i < ui_->listWidget->count(); ++i) {
    if (QWidget* row = ui_->listWidget->itemWidget(ui_->listWidget->item(i))) {
      visit(row);
    }
  }
  for (QWidget* row : markerScopeRows()) {
    visit(row);
  }
}

void CurveEditor::onSwatchClicked(const QString& curve_name, QPushButton* swatch) {
  if (plot_ == nullptr) {
    return;
  }
  const auto* info = plot_->curveFromTitle(curve_name);
  const QColor current = (info != nullptr && info->curve != nullptr) ? info->curve->pen().color() : QColor(Qt::white);

  active_color_curve_ = curve_name;

  if (color_picker_ == nullptr) {
    color_picker_ = new ColorPickerPopup(this);
    connect(color_picker_, &ColorPickerPopup::colorChanged, this, &CurveEditor::onPickerColorChanged);
  }
  color_picker_->setColor(current);
  color_picker_->move(swatch->mapToGlobal(QPoint(0, swatch->height())));
  color_picker_->show();
}

void CurveEditor::onPickerColorChanged(QColor color) {
  if (plot_ == nullptr || active_color_curve_.isEmpty() || !color.isValid()) {
    return;
  }
  // onChangeCurveColor calls replot() internally; no second replot here.
  plot_->onChangeCurveColor(active_color_curve_, color);
  emit plot_->undoableChange();
}

void CurveEditor::onCurveColorChanged(const QString& curve_name, QColor color) {
  for (auto* button : ui_->listWidget->findChildren<QPushButton*>()) {
    if (button->property(kColorButtonProperty).toString() != curve_name) {
      continue;
    }
    if (auto* color_button = dynamic_cast<CurveColorButton*>(button)) {
      color_button->setColor(color);
    }
  }
}

void CurveEditor::clearActivePicker() {
  active_color_curve_.clear();
  if (color_picker_ != nullptr && color_picker_->isVisible()) {
    color_picker_->hide();
  }
}

void CurveEditor::onVisibilityToggled(const QString& curve_name, bool visible) {
  if (state_controller_ != nullptr) {
    state_controller_->setSeriesVisible(curve_name.toULongLong(), visible);
    return;
  }
  if (plot_ == nullptr) {
    return;
  }
  plot_->setCurveVisible(curve_name, visible);
}

void CurveEditor::onMarkersToggled(const QString& curve_name, bool show) {
  if (plot_ == nullptr) {
    return;
  }
  plot_->setCurveShowMarkers(curve_name, show);
}

void CurveEditor::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  const int button_extent = metrics.icon_size + metrics.icon_padding;
  const int band_extent = button_extent + (2 * metrics.layout_padding);
  const QSize icon_sz(metrics.icon_size, metrics.icon_size);
  ui_->filterCurves->setChromeMetrics(metrics);
  ui_->buttonCurvesMenu->setMinimumSize(button_extent, button_extent);
  ui_->buttonCurvesMenu->setMaximumSize(button_extent, button_extent);
  ui_->buttonCurvesMenu->setIconSize(icon_sz);
  ui_->widgetLabelCurves->setMinimumHeight(band_extent);
  ui_->widgetLabelCurves->setMaximumHeight(band_extent);
  if (auto* layout = ui_->headerLayout) {
    layout->setContentsMargins(
        metrics.layout_padding, metrics.layout_padding, metrics.layout_padding, metrics.layout_padding);
    layout->setSpacing(metrics.layout_spacing);
  }
  // QListWidget::setSpacing is the gap between adjacent rows.
  ui_->listWidget->setSpacing(metrics.layout_spacing);
  // Row height tracks icon_size so the per-row eye / trash glyphs and
  // the colour swatch grow with the rest of the chrome. Push the new
  // value into each existing row (item sizeHint + the row widget's own
  // stored row height + the per-row button iconSize so the eye and
  // trash glyphs re-rasterise at the new extent).
  row_height_ = metrics.icon_size;
  for (int i = 0; i < ui_->listWidget->count(); ++i) {
    ui_->listWidget->item(i)->setSizeHint(QSize(0, row_height_));
  }
  // The +1 keeps the footer layout clear of its 1-px top border (QSS paints
  // it, the layout doesn't inset for it).
  ui_->markerScopesLayout->setSpacing(metrics.layout_spacing);
  ui_->markerScopesLayout->setContentsMargins(0, 1 + metrics.layout_spacing, 0, 0);
  for (QWidget* row : markerScopeRows()) {
    row->setFixedHeight(row_height_);
  }
  const QSize row_icon_size(row_height_, row_height_);
  forEachRow([&](QWidget* row) {
    // Every row is a CurveRowWidget (file-local, no Q_OBJECT), so static_cast.
    static_cast<CurveRowWidget*>(row)->setRowHeight(row_height_);
    for (auto* button : row->findChildren<QToolButton*>()) {
      if (auto* glyph = qobject_cast<SvgButton*>(button)) {
        glyph->setExtent(row_height_, std::max(kMarkerScopeGlyphInset, row_height_ - kMarkerScopeGlyphInset));
      } else {
        button->setIconSize(row_icon_size);
      }
    }
  });
}

void CurveEditor::onStylesheetChanged(QString theme) {
  current_theme_ = std::move(theme);
  // Header kebab icon (the Search glyph self-retints).
  ui_->buttonCurvesMenu->setIcon(loadSvg(":/resources/svg/more_vert.svg", current_theme_));
  // Re-tint every row's toggles to the new theme ink (a footer row's scope
  // glyph is an SvgButton and retints itself).
  forEachRow([this](QWidget* row) {
    for (auto* button : row->findChildren<QToolButton*>()) {
      if (button->property(kVisibilityButtonProperty).isValid()) {
        button->setIcon(loadSvg(button->isChecked() ? kVisibilityOnPath : kVisibilityOffPath, current_theme_));
      } else if (button->property(kMarkersButtonProperty).isValid()) {
        button->setIcon(loadSvg(button->isChecked() ? kMarkersOnPath : kMarkersOffPath, current_theme_));
      } else if (button->property(kTrashButtonProperty).isValid()) {
        button->setIcon(loadSvg(kTrashIconPath, current_theme_));
      }
    }
  });
}

void CurveEditor::onFilterChanged(const QString& /*text*/) {
  applyFilter();
}

void CurveEditor::applyFilter() {
  const QString needle = ui_->filterCurves->text().trimmed();
  for (int i = 0; i < ui_->listWidget->count(); ++i) {
    QListWidgetItem* item = ui_->listWidget->item(i);
    const QString curve_name = item->data(kCurveNameRole).toString();
    const QString display_name = item->data(kCurveDisplayNameRole).toString();
    const bool match = needle.isEmpty() || curve_name.contains(needle, Qt::CaseInsensitive) ||
                       display_name.contains(needle, Qt::CaseInsensitive);
    item->setHidden(!match);
  }
}

bool CurveEditor::eventFilter(QObject* watched, QEvent* event) {
  const QEvent::Type type = event->type();
  if ((type == QEvent::FocusIn || type == QEvent::FocusOut) && watched == ui_->filterCurves->lineEdit()) {
    // Focus expands the filter into the label's space. Kebab stays
    // visible — it never gets pushed out.
    const bool focused = (type == QEvent::FocusIn);
    ui_->labelCurves->setVisible(!focused);
  }
  if (watched == ui_->listWidget->viewport() && type == QEvent::Drop && state_controller_ != nullptr) {
    // Strip-row drag-reorder: swallow the InternalMove drop (which would strip
    // the item widgets) and route the move through the controller instead —
    // seriesListChanged then rebuilds the list in the new order. Same drop
    // arithmetic as the 3D layer list: below a row's midline inserts after it.
    auto* drop = static_cast<QDropEvent*>(event);
    if (drop->source() != ui_->listWidget) {
      return QWidget::eventFilter(watched, event);  // foreign drag, not a reorder
    }
    const int from = ui_->listWidget->currentRow();
    int to = ui_->listWidget->count();
    const QPoint pos = drop->position().toPoint();
    if (QListWidgetItem* target = ui_->listWidget->itemAt(pos); target != nullptr) {
      to = ui_->listWidget->row(target);
      if (pos.y() > ui_->listWidget->visualItemRect(target).center().y()) {
        ++to;
      }
    }
    drop->setDropAction(Qt::IgnoreAction);
    drop->accept();
    if (from >= 0) {
      state_controller_->reorderSeries(from, to);
    }
    return true;
  }
  return QWidget::eventFilter(watched, event);
}

void CurveEditor::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  updateHeaderForWidth();
}

void CurveEditor::updateHeaderForWidth() {
  // When the panel narrows past the point where a usable filter would
  // fit, hide the filter + search icon. The "Curves" label stays as
  // the section title; the kebab is always visible — it never gets
  // pushed out. Filter-focus still hides the label to widen the input
  // (handled in eventFilter).
  constexpr int kFilterHideBelow = 140;
  const bool wide_enough = width() >= kFilterHideBelow;
  ui_->filterCurves->setVisible(wide_enough);
}

}  // namespace PJ
