// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plugins/host_qt/widget_adapters.hpp"

#include <pj_widgets/ComboBox.h>
#include <pj_widgets/CredentialsEditor.h>
#include <pj_widgets/DateRangePicker.h>
#include <pj_widgets/DualOptionsWidget.h>
#include <pj_widgets/Scrollbar.h>
#include <pj_widgets/ToggleSwitch.h>

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractTextDocumentLayout>
#include <QBoxLayout>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QLayout>
#include <QPainter>
#include <QPen>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTextBrowser>
#include <QTextObjectInterface>
#include <QVariant>
#include <algorithm>

#include "pj_plugins/host_qt/widget_binding.hpp"
#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

namespace {

// Marker properties linking an adapted original to its styled replacement.
constexpr const char* kDualOptionsWidgetProperty = "_pj_dual_options_widget";
constexpr const char* kDualOptionsRadiosProperty = "_pj_dual_options_radios";
constexpr const char* kDualOptionsDesiredVisibleProperty = "_pj_dual_options_desired_visible";

// Markdown supplied by a plugin is self-contained display text. Prevent the
// rich view from resolving image/file resources referenced by that text; links
// are handled separately by widget_binding as explicit user actions.
class MarkdownDocument final : public QTextDocument {
 public:
  using QTextDocument::QTextDocument;

  QVariant loadResource(int /*type*/, const QUrl& /*name*/) override {
    return QByteArray{};
  }
};

// The stock image handler bypasses loadResource: when the document returns no
// data it reads the file by name itself and caches the result in the document.
// Replacing it lays out and paints nothing, so an image reference never opens a file.
class BlockedImageHandler final : public QObject, public QTextObjectInterface {
  Q_OBJECT
  Q_INTERFACES(QTextObjectInterface)
 public:
  using QObject::QObject;

  QSizeF intrinsicSize(QTextDocument* /*doc*/, int /*pos*/, const QTextFormat& /*format*/) override {
    return {};
  }
  void drawObject(
      QPainter* /*painter*/, const QRectF& /*rect*/, QTextDocument* /*doc*/, int /*pos*/,
      const QTextFormat& /*format*/) override {}
};

class MarkdownTextBrowser final : public QTextBrowser {
 public:
  explicit MarkdownTextBrowser(QWidget* parent) : QTextBrowser(parent) {
    auto* doc = new MarkdownDocument(this);
    setDocument(doc);
    doc->documentLayout()->registerHandler(QTextFormat::ImageObject, new BlockedImageHandler(doc));
  }
};

static PJ::DualOptionsWidget* pairedDualOptionsWidget(const QWidget* widget) {
  QObject* obj = widget->property(kDualOptionsWidgetProperty).value<QObject*>();
  return qobject_cast<PJ::DualOptionsWidget*>(obj);
}

// The radios an adapted DualOptionsWidget replaces, in segment order. Empty if
// any of them has been destroyed.
static QList<QRadioButton*> adaptedRadios(const PJ::DualOptionsWidget* dual) {
  QList<QRadioButton*> radios;
  const QObjectList objects = dual->property(kDualOptionsRadiosProperty).value<QObjectList>();
  for (QObject* obj : objects) {
    auto* radio = qobject_cast<QRadioButton*>(obj);
    if (radio == nullptr) {
      return {};
    }
    radios.push_back(radio);
  }
  return radios;
}

// Mirror each source radio's enabled state onto its segment, so a disabled option
// greys out and cannot be switched to rather than disabling the whole control —
// which stays interactive while any segment remains enabled.
static void applyRadioEnabledStates(DualOptionsWidget* dual, const QList<QRadioButton*>& radios) {
  bool any_enabled = false;
  for (qsizetype i = 0; i < radios.size(); ++i) {
    const bool segment_enabled = radios[i]->isEnabled();
    dual->setSegmentEnabled(static_cast<int>(i), segment_enabled);
    any_enabled = any_enabled || segment_enabled;
  }
  dual->setEnabled(any_enabled);
}

static void syncDualOptionsFromRadios(QRadioButton* radio) {
  auto* dual = pairedDualOptionsWidget(radio);
  if (dual == nullptr) {
    return;
  }
  const QList<QRadioButton*> radios = adaptedRadios(dual);
  if (radios.isEmpty()) {
    return;
  }

  applyRadioEnabledStates(dual, radios);

  bool visible = false;
  int selected = 0;
  for (qsizetype i = 0; i < radios.size(); ++i) {
    radios[i]->hide();
    visible = visible || radios[i]->property(kDualOptionsDesiredVisibleProperty).toBool();
    if (radios[i]->isChecked()) {
      selected = static_cast<int>(i);
    }
  }
  dual->setVisible(visible);

  const QSignalBlocker blocker(dual);
  dual->setSelectedIndex(selected);
}

static bool boxSegmentContainsOnlyGroupOrSpacer(
    QBoxLayout* layout, const QList<QRadioButton*>& radios, int& begin, int& end) {
  begin = -1;
  end = -1;
  for (QRadioButton* radio : radios) {
    const int index = layout->indexOf(radio);
    if (index < 0) {
      return false;
    }
    begin = (begin < 0) ? index : std::min(begin, index);
    end = std::max(end, index);
  }
  for (int i = begin; i <= end; ++i) {
    QLayoutItem* item = layout->itemAt(i);
    if (item == nullptr || item->spacerItem() != nullptr) {
      continue;
    }
    if (item->layout() != nullptr) {
      return false;
    }
    QWidget* widget = item->widget();
    if (widget == nullptr || radios.contains(qobject_cast<QRadioButton*>(widget))) {
      continue;
    }
    return false;
  }
  return true;
}

static bool gridItemPosition(
    QGridLayout* layout, QWidget* widget, int& row, int& column, int& row_span, int& column_span) {
  const int index = layout->indexOf(widget);
  if (index < 0) {
    return false;
  }
  layout->getItemPosition(index, &row, &column, &row_span, &column_span);
  return true;
}

static bool gridSegmentContainsOnlyGroupOrSpacer(
    QGridLayout* layout, const QList<QRadioButton*>& radios, int& row, int& column, int& column_span) {
  row = 0;
  int begin_col = -1;
  int end_col = -1;
  for (qsizetype i = 0; i < radios.size(); ++i) {
    int radio_row = 0;
    int radio_col = 0;
    int radio_row_span = 0;
    int radio_col_span = 0;
    if (!gridItemPosition(layout, radios[i], radio_row, radio_col, radio_row_span, radio_col_span)) {
      return false;
    }
    if (radio_row_span != 1 || (i > 0 && radio_row != row)) {
      return false;
    }
    row = radio_row;
    begin_col = (begin_col < 0) ? radio_col : std::min(begin_col, radio_col);
    end_col = std::max(end_col, radio_col + radio_col_span);
  }

  column = begin_col;
  column_span = end_col - column;

  for (int i = 0; i < layout->count(); ++i) {
    int item_row = 0;
    int item_col = 0;
    int item_row_span = 0;
    int item_col_span = 0;
    layout->getItemPosition(i, &item_row, &item_col, &item_row_span, &item_col_span);
    if (item_row != row || item_col >= end_col || item_col + item_col_span <= column) {
      continue;
    }
    QLayoutItem* item = layout->itemAt(i);
    if (item == nullptr || item->spacerItem() != nullptr) {
      continue;
    }
    if (item->layout() != nullptr) {
      return false;
    }
    QWidget* widget = item->widget();
    if (widget == nullptr || radios.contains(qobject_cast<QRadioButton*>(widget))) {
      continue;
    }
    return false;
  }
  return true;
}

struct RadioGroupPlacement {
  QBoxLayout* box_layout = nullptr;
  QGridLayout* grid_layout = nullptr;
  // Box: index range [begin_index, end_index] the group occupies in the layout.
  int begin_index = -1;
  int end_index = -1;
  // Grid: the cell span covering the whole group.
  int row = 0;
  int column = 0;
  int column_span = 0;
};

static bool findRadioGroupPlacement(
    QLayout* layout, const QList<QRadioButton*>& radios, RadioGroupPlacement& placement) {
  if (layout == nullptr) {
    return false;
  }

  if (auto* box_layout = qobject_cast<QBoxLayout*>(layout)) {
    int begin = -1;
    int end = -1;
    if (boxSegmentContainsOnlyGroupOrSpacer(box_layout, radios, begin, end)) {
      placement = {};
      placement.box_layout = box_layout;
      placement.begin_index = begin;
      placement.end_index = end;
      return true;
    }
  }

  if (auto* grid_layout = qobject_cast<QGridLayout*>(layout)) {
    int row = 0;
    int column = 0;
    int column_span = 0;
    if (gridSegmentContainsOnlyGroupOrSpacer(grid_layout, radios, row, column, column_span)) {
      placement = {};
      placement.grid_layout = grid_layout;
      placement.row = row;
      placement.column = column;
      placement.column_span = column_span;
      return true;
    }
  }

  for (int i = 0; i < layout->count(); ++i) {
    QLayoutItem* item = layout->itemAt(i);
    if (item != nullptr && item->layout() != nullptr && findRadioGroupPlacement(item->layout(), radios, placement)) {
      return true;
    }
  }
  return false;
}

static bool radiosHaveExplicitExclusiveGroup(const QList<QRadioButton*>& radios) {
  QButtonGroup* group = radios.front()->group();
  if (group == nullptr || !group->exclusive() || group->buttons().size() != radios.size()) {
    return false;
  }
  return std::all_of(radios.begin(), radios.end(), [group](QRadioButton* radio) { return radio->group() == group; });
}

// Orders `radios` left-to-right by their slot in the shared layout (box index
// or grid column). False when no layout places the whole group together.
static bool sortRadiosByLayout(QWidget* parent, QList<QRadioButton*>& radios) {
  RadioGroupPlacement placement;
  if (!findRadioGroupPlacement(parent->layout(), radios, placement)) {
    return false;
  }
  QList<QPair<int, QRadioButton*>> ordered;
  for (QRadioButton* radio : radios) {
    int slot = 0;
    if (placement.box_layout != nullptr) {
      slot = placement.box_layout->indexOf(radio);
    } else if (placement.grid_layout != nullptr) {
      int row = 0;
      int row_span = 0;
      int column_span = 0;
      (void)gridItemPosition(placement.grid_layout, radio, row, slot, row_span, column_span);
    }
    ordered.push_back({slot, radio});
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (qsizetype i = 0; i < ordered.size(); ++i) {
    radios[i] = ordered[i].second;
  }
  return true;
}

static void insertDualOptionsWidget(QWidget* parent, const QList<QRadioButton*>& radios, DualOptionsWidget* dual) {
  RadioGroupPlacement placement;
  if (!findRadioGroupPlacement(parent->layout(), radios, placement)) {
    return;
  }

  if (placement.box_layout != nullptr) {
    QBoxLayout* box = placement.box_layout;
    const bool horizontal = box->direction() == QBoxLayout::LeftToRight || box->direction() == QBoxLayout::RightToLeft;
    // The segmented control adopts the source radios' layout axis: a vertical
    // radio column becomes a vertical strip whose chip slides up/down.
    dual->setOrientation(horizontal ? Qt::Horizontal : Qt::Vertical);
    const int insert_at = placement.begin_index;
    // The push-to-right-edge rearrangement below is only valid for the classic
    // `[... group (expanding spacer)]` tail rows. If any real widget follows the
    // group (e.g. `[label, group, stretch, checkbox]`), rearranging would scramble
    // the row — those rows keep the group's original slot instead.
    bool trailing_widget = false;
    for (int i = placement.end_index + 1; i < box->count(); ++i) {
      if (box->itemAt(i)->widget() != nullptr) {
        trailing_widget = true;
        break;
      }
    }
    // A row can opt out of the push-to-right-edge treatment — e.g. an inline
    // "label → input → then: → group" row that wants the group to sit one snug
    // gap after its preceding label — by tagging its first radio with the
    // pjInlineGroup dynamic property. The group then keeps its authored slot.
    const bool keep_inline = !radios.isEmpty() && radios.first()->property("pjInlineGroup").toBool();
    for (QRadioButton* radio : radios) {
      box->removeWidget(radio);
    }
    if (horizontal && !trailing_widget && !keep_inline) {
      // Drop the trailing horizontal spacer that used to hold the group on the left,
      // then re-append as [stretch][group][inset] so the group is pushed to the
      // right edge of its row with the canonical comfortable inset — lining up
      // with the row-filling ToggleSwitches (which use the same inset).
      for (int i = box->count() - 1; i >= 0; --i) {
        QSpacerItem* sp = box->itemAt(i)->spacerItem();
        if (sp != nullptr && (sp->expandingDirections() & Qt::Horizontal)) {
          delete box->takeAt(i);
          break;
        }
      }
      box->addStretch(1);
      box->addWidget(dual);
      box->addSpacing(theme::space(theme::Space::Comfortable));
    } else {
      box->insertWidget(insert_at, dual);
    }
  } else if (placement.grid_layout != nullptr) {
    for (QRadioButton* radio : radios) {
      placement.grid_layout->removeWidget(radio);
    }
    placement.grid_layout->addWidget(dual, placement.row, placement.column, 1, placement.column_span);
  }
}

static bool tryAdaptRadios(QList<QRadioButton*> radios) {
  if (radios.size() < 2) {
    return false;
  }
  QWidget* parent = radios.front()->parentWidget();
  if (parent == nullptr) {
    return false;
  }
  int checked_count = 0;
  for (QRadioButton* radio : radios) {
    if (pairedDualOptionsWidget(radio) != nullptr || radio->parentWidget() != parent || radio->text().isEmpty()) {
      return false;
    }
    checked_count += radio->isChecked() ? 1 : 0;
  }
  if (!radiosHaveExplicitExclusiveGroup(radios)) {
    return false;
  }
  // A group loaded without an initial selection has no segment to highlight —
  // defer until plugin data checks one (the reactive tryAdaptStyledWidget path).
  if (checked_count != 1) {
    return false;
  }
  if (!sortRadiosByLayout(parent, radios)) {
    return false;
  }

  QStringList labels;
  int selected = 0;
  for (qsizetype i = 0; i < radios.size(); ++i) {
    labels.push_back(radios[i]->text());
    if (radios[i]->isChecked()) {
      selected = static_cast<int>(i);
    }
  }

  auto* dual = new DualOptionsWidget(labels, parent);
  dual->setToolTip(parent->toolTip());
  applyRadioEnabledStates(dual, radios);
  dual->setSelectedIndex(selected);
  QObjectList radio_objects;
  for (QRadioButton* radio : radios) {
    radio_objects.push_back(radio);
    radio->setProperty(kDualOptionsWidgetProperty, QVariant::fromValue<QObject*>(dual));
    radio->setProperty(kDualOptionsDesiredVisibleProperty, !radio->isHidden());
  }
  dual->setProperty(kDualOptionsRadiosProperty, QVariant::fromValue(radio_objects));

  insertDualOptionsWidget(parent, radios, dual);
  for (QRadioButton* radio : radios) {
    radio->hide();
  }

  QObject::connect(dual, &DualOptionsWidget::selectionChanged, dual, [dual](int index) {
    const QList<QRadioButton*> group_radios = adaptedRadios(dual);
    if (index >= 0 && index < group_radios.size() && group_radios[index]->isEnabled()) {
      group_radios[index]->setChecked(true);
    }
  });
  for (qsizetype i = 0; i < radios.size(); ++i) {
    QRadioButton* radio = radios[i];
    const int index = static_cast<int>(i);
    QObject::connect(radio, &QRadioButton::toggled, dual, [radio, dual, index](bool checked) {
      if (checked) {
        dual->setSelectedIndex(index);
      }
      radio->hide();
    });
  }
  return true;
}

// Try to adapt the exclusive button group that `radio` belongs to. No-op when
// the radio is already adapted, has no group, the group is non-exclusive, or
// the group has fewer than two radios sharing `radio`'s parent. Cheap and
// idempotent — safe to call per radio whenever its data is applied, which is
// how a group that only becomes adaptable AFTER plugin data selects an option
// gets converted without re-walking the whole widget tree on every data tick.
static void tryAdaptRadioGroup(QRadioButton* radio) {
  if (radio == nullptr || pairedDualOptionsWidget(radio) != nullptr) {
    return;
  }
  QButtonGroup* group = radio->group();
  QWidget* parent = radio->parentWidget();
  if (group == nullptr || !group->exclusive() || parent == nullptr) {
    return;
  }
  QList<QRadioButton*> group_radios;
  for (auto* button : group->buttons()) {
    if (auto* rb = qobject_cast<QRadioButton*>(button); rb != nullptr && rb->parentWidget() == parent) {
      group_radios.push_back(rb);
    }
  }
  if (group_radios.size() >= 2) {
    (void)tryAdaptRadios(group_radios);
  }
}

// --- QCheckBox -> ToggleSwitch ------------------------------------------------

constexpr const char* kToggleSwitchProperty = "_pj_toggle_switch";                   // on the hidden QCheckBox
constexpr const char* kToggleCheckBoxProperty = "_pj_toggle_checkbox";               // on the ToggleSwitch
constexpr const char* kToggleDesiredVisibleProperty = "_pj_toggle_desired_visible";  // on the hidden QCheckBox

static PJ::ToggleSwitch* pairedToggleSwitch(const QWidget* widget) {
  QObject* obj = widget->property(kToggleSwitchProperty).value<QObject*>();
  return qobject_cast<PJ::ToggleSwitch*>(obj);
}

// True when `widget` lives inside one of the host composite widgets that manage
// their own internal child controls (CredentialsEditor's allow-insecure box,
// DateRangePicker's sub-widgets). Those internals are opaque — adapting a
// checkbox buried in them would corrupt the composite, so they are left alone.
static bool isInsideHostComposite(const QWidget* widget) {
  for (QWidget* p = widget->parentWidget(); p != nullptr; p = p->parentWidget()) {
    if (qobject_cast<CredentialsEditor*>(p) != nullptr || qobject_cast<DateRangePicker*>(p) != nullptr) {
      return true;
    }
  }
  return false;
}

// Push the checkbox's current state onto its ToggleSwitch (keeping it hidden).
static void syncToggleFromCheckBox(QCheckBox* checkbox) {
  auto* toggle = pairedToggleSwitch(checkbox);
  if (toggle == nullptr) {
    return;
  }
  checkbox->hide();
  toggle->setEnabled(checkbox->isEnabled());
  toggle->setVisible(checkbox->property(kToggleDesiredVisibleProperty).toBool());
  const QSignalBlocker blocker(toggle);
  toggle->setChecked(checkbox->isChecked(), /*animate=*/false);
}

// Swap `old_w` for `new_w` in `parent`'s layout, preserving position/span.
// QLayout::replaceWidget searches nested layouts recursively and returns the
// item that wrapped `old_w` (which we delete; `old_w` itself stays alive).
static bool replaceWidgetInLayout(QWidget* parent, QWidget* old_w, QWidget* new_w) {
  QLayout* layout = parent->layout();
  if (layout == nullptr) {
    return false;
  }
  QLayoutItem* old_item = layout->replaceWidget(old_w, new_w);
  if (old_item == nullptr) {
    return false;
  }
  delete old_item;
  return true;
}

// Replace a plain QCheckBox with a labelled ToggleSwitch (label on the LEFT,
// switch on the right). The checkbox is kept hidden+alive so plugin data and
// the connectWidgetSignals event callback keep flowing through it; the toggle
// just drives it. No-op for tristate / textless / composite-internal / already
// adapted checkboxes.
static bool tryAdaptCheckBox(QCheckBox* checkbox) {
  if (checkbox == nullptr || pairedToggleSwitch(checkbox) != nullptr) {
    return false;
  }
  if (checkbox->isTristate() || checkbox->text().isEmpty()) {
    return false;
  }
  QWidget* parent = checkbox->parentWidget();
  if (parent == nullptr || parent->layout() == nullptr || isInsideHostComposite(checkbox)) {
    return false;
  }

  auto* toggle = new ToggleSwitch(parent);
  toggle->setText(checkbox->text());
  toggle->setLabelSide(ToggleSwitch::LabelSide::Left);
  // Fill the row so the switch (drawn at the widget's right edge for a Left-side
  // label) is pushed to the far right of its settings area, settings-list style,
  // instead of hugging the label text. The pill itself stays a fixed width.
  toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  toggle->setToolTip(checkbox->toolTip());
  toggle->setEnabled(checkbox->isEnabled());
  toggle->setChecked(checkbox->isChecked(), /*animate=*/false);

  if (!replaceWidgetInLayout(parent, checkbox, toggle)) {
    delete toggle;
    return false;
  }
  checkbox->setProperty(kToggleSwitchProperty, QVariant::fromValue<QObject*>(toggle));
  checkbox->setProperty(kToggleDesiredVisibleProperty, !checkbox->isHidden());
  toggle->setProperty(kToggleCheckBoxProperty, QVariant::fromValue<QObject*>(checkbox));
  checkbox->hide();

  const QPointer<QCheckBox> cb_ptr(checkbox);
  QObject::connect(toggle, &ToggleSwitch::toggled, toggle, [cb_ptr](bool checked) {
    if (cb_ptr != nullptr) {
      cb_ptr->setChecked(checked);
    }
  });
  QObject::connect(checkbox, &QCheckBox::toggled, toggle, [checkbox, toggle](bool checked) {
    const QSignalBlocker blocker(toggle);
    toggle->setChecked(checked, /*animate=*/false);
    checkbox->hide();
  });
  return true;
}

// --- QTableView interior-only cell grid --------------------------------------

constexpr const char* kInteriorGridProperty = "pjInteriorGrid";                // opt-in, set in the .ui
constexpr const char* kInteriorGridAppliedProperty = "pjInteriorGridApplied";  // idempotency marker

// Item delegate for an interior-grid table. Draws:
//  - vertical dividers: the right edge of every cell EXCEPT the last visible
//    column, so the table has no outer-right line that would double the column
//    splitter / scrollbar chrome on its right;
//  - horizontal dividers: the bottom edge of EVERY row INCLUDING the last, so the
//    bottom of the data is closed and the final row reads like the rest (a short
//    table has empty space, not chrome, below it, so it needs its own bottom line).
// Paired with setShowGrid(false): yields the internal grid plus a clean bottom rule,
// without the native grid's outer-right edge line.
//
// Why this exists: QTableView::paintEvent always draws the trailing
// column/row edge (no last-line special-case), so with a stretched first column
// the native grid's right edge lands flush on the surrounding splitter/scrollbar
// chrome and reads as a doubled border. The grid is painted AFTER all items, so
// a delegate cannot erase it — the only robust route is to turn the native grid
// off and paint the interior lines ourselves. QSS can't do this either (Qt drops
// per-::item left/right borders and tints every column edge in the selection
// color — the spurious blue line this replaces).
//
// The divider color is fetched the exact way QTableView picks the native grid
// color (styleHint SH_Table_GridLineColor), so it tracks the QSS `gridline-color`
// token and the active theme with no hardcoded color and no theme-switch wiring.
class InteriorGridDelegate : public QStyledItemDelegate {
 public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    QStyledItemDelegate::paint(painter, option, index);  // content + selection first

    const auto* view = qobject_cast<const QTableView*>(option.widget);
    if (view == nullptr) {
      return;
    }
    const int rgb = view->style()->styleHint(QStyle::SH_Table_GridLineColor, &option, view);
    QPen pen(QColor::fromRgb(static_cast<QRgb>(rgb)));
    pen.setCosmetic(true);  // exactly one device pixel, DPI-independent
    pen.setWidth(0);

    painter->save();
    painter->setPen(pen);
    const QRect r = option.rect;
    // Vertical divider on the cell's right edge — only between columns, never on the
    // last visible column (that outer-right line would double the column splitter /
    // scrollbar chrome on the table's right).
    if (hasVisibleColumnAfter(view, index)) {
      painter->drawLine(r.topRight(), r.bottomRight());
    }
    // Horizontal divider on the cell's bottom edge — under EVERY row, including the
    // last, so the bottom of the data is closed and the final row reads like the rest.
    painter->drawLine(r.bottomLeft(), r.bottomRight());
    painter->restore();
  }

 private:
  // True when a visible column exists after `index`'s column, so a divider on that
  // cell's right edge is interior (not the table's outer-right edge). Skips hidden
  // columns so a hidden trailing column can't leave a stray outer-right line.
  static bool hasVisibleColumnAfter(const QTableView* view, const QModelIndex& index) {
    const QAbstractItemModel* model = view->model();
    const int count = model->columnCount(index.parent());
    for (int c = index.column() + 1; c < count; ++c) {
      if (!view->isColumnHidden(c)) {
        return true;
      }
    }
    return false;
  }
};

}  // namespace

void adaptRadioGroups(QWidget* root) {
  if (root == nullptr) {
    return;
  }
  // One pass over every radio; tryAdaptRadioGroup is idempotent, so visiting
  // both members of a pair just no-ops the second time.
  const QList<QRadioButton*> radios = root->findChildren<QRadioButton*>();
  for (QRadioButton* radio : radios) {
    tryAdaptRadioGroup(radio);
  }
}

void adaptCheckBoxes(QWidget* root) {
  if (root == nullptr) {
    return;
  }
  const QList<QCheckBox*> checkboxes = root->findChildren<QCheckBox*>();
  for (QCheckBox* checkbox : checkboxes) {
    (void)tryAdaptCheckBox(checkbox);
  }
}

void adaptComboBoxes(QWidget* root) {
  if (root == nullptr) {
    return;
  }
  // Unlike radios/checkboxes, PJ::ComboBox IS a QComboBox, so there is no swap:
  // we upgrade each plain QComboBox in place (gradient delegate + popup frame
  // fixes), which preserves its model, current index, and signal connections.
  // Skip ones already promoted to PJ::ComboBox, ones inside opaque host
  // composites, and ones already upgraded (marker keeps it idempotent).
  static constexpr const char* kComboStyledProperty = "_pj_combo_styled";
  const QList<QComboBox*> combos = root->findChildren<QComboBox*>();
  for (QComboBox* combo : combos) {
    if (qobject_cast<ComboBox*>(combo) != nullptr || combo->property(kComboStyledProperty).toBool() ||
        isInsideHostComposite(combo)) {
      continue;
    }
    applyComboBoxStyling(combo);
    combo->setProperty(kComboStyledProperty, true);
  }
}

void adaptScrollAreas(QWidget* root) {
  // The canonical walker lives in pj_widgets alongside PJ::Scrollbar so app
  // windows can reuse it; the host only adds its composite-widget veto (a plugin
  // custom widget's internal scroll areas must not get overlaid).
  PJ::attachPillScrollbars(root, [](QAbstractScrollArea* area) { return isInsideHostComposite(area); });
}

void adaptGridTables(QWidget* root) {
  if (root == nullptr) {
    return;
  }
  // Opt-in via a dynamic bool property set in the plugin .ui — the host hardcodes
  // no table name, so this stays domain-neutral. QTableWidget is a QTableView, so
  // findChildren<QTableView*> covers both. Idempotent via the marker property.
  const QList<QTableView*> tables = root->findChildren<QTableView*>();
  for (QTableView* table : tables) {
    if (!table->property(kInteriorGridProperty).toBool() || table->property(kInteriorGridAppliedProperty).toBool()) {
      continue;
    }
    table->setShowGrid(false);                                // native grid off (kills its outer edges)
    table->setFrameShape(QFrame::NoFrame);                    // no outer frame either
    table->setItemDelegate(new InteriorGridDelegate(table));  // draw interior dividers only
    table->setProperty(kInteriorGridAppliedProperty, true);
  }
}

void adaptMarkdownEdits(QWidget* root) {
  if (root == nullptr) {
    return;
  }
  // Dynamic .ui properties are only available after QUiLoader has built the
  // tree, which is why this promotion lives in the engines' post-load adapter
  // pass rather than PjUiLoader::createWidget(). Only child edits in a layout
  // can be replaced without changing the loaded root pointer.
  const QList<QPlainTextEdit*> edits = root->findChildren<QPlainTextEdit*>();
  for (QPlainTextEdit* edit : edits) {
    if (!edit->property(kMarkdownProperty).toBool()) {
      continue;
    }
    QWidget* parent = edit->parentWidget();
    if (parent == nullptr || parent->layout() == nullptr) {
      continue;
    }

    auto* browser = new MarkdownTextBrowser(parent);
    browser->setObjectName(edit->objectName());
    browser->setSizePolicy(edit->sizePolicy());
    browser->setMinimumSize(edit->minimumSize());
    browser->setMaximumSize(edit->maximumSize());
    browser->setBaseSize(edit->baseSize());
    browser->setFont(edit->font());
    browser->setPalette(edit->palette());
    browser->setStyleSheet(edit->styleSheet());
    browser->setEnabled(edit->isEnabled());
    browser->setVisible(!edit->isHidden());
    browser->setFocusPolicy(edit->focusPolicy());
    browser->setContextMenuPolicy(edit->contextMenuPolicy());
    browser->setLayoutDirection(edit->layoutDirection());
    browser->setToolTip(edit->toolTip());
    browser->setStatusTip(edit->statusTip());
    browser->setWhatsThis(edit->whatsThis());
    browser->setAccessibleName(edit->accessibleName());
    browser->setAccessibleDescription(edit->accessibleDescription());
    browser->setHorizontalScrollBarPolicy(edit->horizontalScrollBarPolicy());
    browser->setVerticalScrollBarPolicy(edit->verticalScrollBarPolicy());
    browser->setFrameShape(edit->frameShape());
    browser->setFrameShadow(edit->frameShadow());
    browser->setLineWidth(edit->lineWidth());
    browser->setMidLineWidth(edit->midLineWidth());
    browser->setPlaceholderText(edit->placeholderText());
    browser->setTabChangesFocus(edit->tabChangesFocus());
    browser->setWordWrapMode(edit->wordWrapMode());
    browser->setLineWrapMode(
        edit->lineWrapMode() == QPlainTextEdit::NoWrap ? QTextEdit::NoWrap : QTextEdit::WidgetWidth);
    browser->setTabStopDistance(edit->tabStopDistance());
    browser->setPlainText(edit->toPlainText());

    for (const QByteArray& property_name : edit->dynamicPropertyNames()) {
      browser->setProperty(property_name.constData(), edit->property(property_name.constData()));
    }

    if (!replaceWidgetInLayout(parent, edit, browser)) {
      delete browser;
      continue;
    }
    edit->setObjectName({});
    delete edit;
  }
}

void adaptStyledWidgets(QWidget* root) {
  adaptRadioGroups(root);
  adaptCheckBoxes(root);
  adaptComboBoxes(root);
  adaptMarkdownEdits(root);
  adaptScrollAreas(root);
  adaptGridTables(root);
}

void tryAdaptStyledWidget(QWidget* w) {
  if (auto* rb = qobject_cast<QRadioButton*>(w)) {
    tryAdaptRadioGroup(rb);
    return;
  }
  if (auto* cb = qobject_cast<QCheckBox*>(w)) {
    (void)tryAdaptCheckBox(cb);
    return;
  }
}

void syncStyledWidget(QWidget* w) {
  if (auto* rb = qobject_cast<QRadioButton*>(w)) {
    syncDualOptionsFromRadios(rb);
    return;
  }
  if (auto* cb = qobject_cast<QCheckBox*>(w)) {
    syncToggleFromCheckBox(cb);
    return;
  }
}

bool redirectAdaptedVisibility(QWidget* w, bool visible) {
  if (pairedDualOptionsWidget(w) != nullptr) {
    w->setProperty(kDualOptionsDesiredVisibleProperty, visible);
    return true;
  }
  if (pairedToggleSwitch(w) != nullptr) {
    w->setProperty(kToggleDesiredVisibleProperty, visible);
    return true;
  }
  return false;
}

void forwardEmbeddedDialogClose(QWidget* content, QDialog* outer) {
  auto* inner = qobject_cast<QDialog*>(content);
  if (inner == nullptr || outer == nullptr) {
    return;
  }
  // Esc (or any programmatic accept/reject) on the embedded root closes the
  // hosting chrome with the same result instead of hiding just the content.
  QObject::connect(inner, &QDialog::finished, outer, [outer](int result) { outer->done(result); });
}

}  // namespace PJ

#include "widget_adapters.moc"
