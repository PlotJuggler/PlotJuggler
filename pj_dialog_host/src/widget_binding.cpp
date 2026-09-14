// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <pj_plotting/PlotWidget.h>
#include <pj_runtime/AppSession.h>
#include <pj_runtime/CatalogModel.h>
#include <pj_widgets/DateRangePicker.h>
#include <pj_widgets/FileDialog.h>
#include <pj_widgets/HeaderDividerHighlight.h>
#include <pj_widgets/HeaderResizePolicy.h>
#include <pj_widgets/MarkerTimeline.h>
#include <pj_widgets/RangeSlider.h>
#include <pj_widgets/SvgUtil.h>
#include <pj_widgets/ToggleSwitch.h>
#include <qwt_plot_curve.h>
#include <qwt_point_data.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QBoxLayout>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHelpEvent>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QSvgRenderer>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTimeZone>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidgetAction>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <pj_base/types.hpp>
#include <pj_plugins/host/widget_event_builder.hpp>
#include <pj_plugins/host_qt/chart_preview_widget.hpp>
#include <pj_plugins/host_qt/widget_adapters.hpp>
#include <pj_plugins/host_qt/widget_binding.hpp>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "chart_placeholder_overlay.hpp"
#include "lua_syntax_highlighter.hpp"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/TailFollowOverlay.h"
#include "python_syntax_highlighter.hpp"
using namespace Qt::StringLiterals;

namespace PJ {

// Widgets the host moved OUT of the panel tree into its own chrome (a side
// drawer, say — see kHoistedWidgetsProperty). Every by-name lookup on a panel
// root goes through these, so a hoisted widget keeps receiving its data and
// keeps its rules exactly as if it still sat under the root.
template <class TPtr>
TPtr findNamedIn(QWidget* root, const QString& name) {
  if (TPtr found = root->findChild<TPtr>(name); found != nullptr) {
    return found;
  }
  for (const QVariant& entry : root->property(kHoistedWidgetsProperty).toList()) {
    auto* hoisted = qobject_cast<QWidget*>(entry.value<QObject*>());
    if (hoisted == nullptr) {
      continue;
    }
    if (hoisted->objectName() == name) {
      if (TPtr typed = qobject_cast<TPtr>(hoisted); typed != nullptr) {
        return typed;
      }
    }
    if (TPtr found = hoisted->findChild<TPtr>(name); found != nullptr) {
      return found;
    }
  }
  return nullptr;
}

template <class TPtr>
QList<TPtr> findAllIn(QWidget* root) {
  QList<TPtr> all = root->findChildren<TPtr>();
  for (const QVariant& entry : root->property(kHoistedWidgetsProperty).toList()) {
    auto* hoisted = qobject_cast<QWidget*>(entry.value<QObject*>());
    if (hoisted == nullptr) {
      continue;
    }
    if (TPtr typed = qobject_cast<TPtr>(hoisted); typed != nullptr) {
      all.push_back(typed);
    }
    all.append(hoisted->findChildren<TPtr>());
  }
  return all;
}

QString resolveNamedIconPath(std::string_view icon_name) {
  if (icon_name == "link") {
    return u":/resources/svg/link.svg"_s;
  }
  if (icon_name == "contract") {
    return u":/resources/svg/contract.svg"_s;
  }
  if (icon_name == "file") {
    // The same glyph the app's file data-source tab uses.
    return QStringLiteral(":/resources/svg/draft.svg");
  }
  if (icon_name == "plug_connect") {
    return u":/resources/svg/plug_connect.svg"_s;
  }
  if (icon_name == "refresh") {
    return u":/resources/svg/refresh.svg"_s;
  }
  if (icon_name == "trash") {
    return QStringLiteral(":/resources/svg/trash.svg");
  }
  if (icon_name == "search") {
    return u":/resources/svg/search_light.svg"_s;
  }
  if (icon_name == "menu") {
    // Hamburger: a panel's own drawer/side-list toggle.
    return u":/resources/svg/menu.svg"_s;
  }
  if (icon_name == "add") {
    return u":/resources/svg/add.svg"_s;
  }
  return {};
}

namespace {

// Human-readable nanosecond duration: "42s", "12m 30s", "3h 45m", "2d 5h 30m".
std::string formatDuration(std::int64_t duration_ns) {
  const std::int64_t total_secs = duration_ns / 1'000'000'000LL;
  if (total_secs < 60) {
    return std::to_string(total_secs) + "s";
  }
  const std::int64_t days = total_secs / 86400;
  const std::int64_t hours = (total_secs % 86400) / 3600;
  const std::int64_t minutes = (total_secs % 3600) / 60;
  const std::int64_t secs = total_secs % 60;
  if (days > 0) {
    return std::to_string(days) + "d " + std::to_string(hours) + "h " + std::to_string(minutes) + "m";
  }
  if (hours > 0) {
    return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
  }
  return std::to_string(minutes) + "m " + std::to_string(secs) + "s";
}

// Map a slider position in [0, slider_max] onto absolute nanoseconds within
// [min_ns, max_ns].
std::int64_t sliderToNs(int pos, int slider_max, std::int64_t min_ns, std::int64_t max_ns) {
  if (slider_max <= 0) {
    return min_ns;
  }
  const double fraction = static_cast<double>(pos) / static_cast<double>(slider_max);
  return min_ns + static_cast<std::int64_t>(fraction * static_cast<double>(max_ns - min_ns));
}

}  // namespace

// ---------------------------------------------------------------------------
// apply_widget_data — push WidgetDataView values into Qt widgets
// ---------------------------------------------------------------------------

// Item-data role tagging a QTableWidgetItem/QListWidgetItem with the plugin
// row/list index it was written for. Anchored to the item object itself — Qt
// relocates item pointers (not their data) when it re-sorts — so the
// row-translation functions below recover the true originating index
// directly, with no key-text matching and no ambiguity when two rows/items
// share identical text.
constexpr int kPluginRowRole = Qt::UserRole + 1;

namespace {

// True for the variant's float/double alternatives.
bool isFloatingValue(const NumericValue& v) {
  return std::holds_alternative<float>(v) || std::holds_alternative<double>(v);
}

bool isNanValue(const NumericValue& v) {
  if (const auto* f = std::get_if<float>(&v)) {
    return std::isnan(*f);
  }
  if (const auto* d = std::get_if<double>(&v)) {
    return std::isnan(*d);
  }
  return false;
}

// The column that absorbs leftover width — by convention column 0 carries the
// topic/channel/curve name and is the one worth making wide.
constexpr int kNameColumn = 0;
// Starting width for the short data columns (type, encoding, count, ...). Wide
// enough for a typical value, and draggable from there.
constexpr int kDataColumnWidth = 96;
// A radio-button column only ever holds the indicator, so it is pinned narrow.
constexpr int kRadioColumnWidth = 36;

// Order two values of the SAME comparison class (both integral or both floating);
// columnValuesComparable() is what guarantees a column never mixes the two.
// Integers compare exactly across signedness — coercing them to double would tie
// distinct values above 2^53 (int64 ns timestamps live there).
bool numericLess(const NumericValue& a, const NumericValue& b) {
  return std::visit(
      [](auto lhs, auto rhs) -> bool {
        if constexpr (std::is_floating_point_v<decltype(lhs)> || std::is_floating_point_v<decltype(rhs)>) {
          return static_cast<double>(lhs) < static_cast<double>(rhs);
        } else {
          return std::cmp_less(lhs, rhs);
        }
      },
      a, b);
}

}  // namespace

// A table cell that carries the plugin's original numeric value beside its display
// text, so a column orders on the value rather than on its rendering ("720" must
// not land before "65").
//
// The ordering is a strict weak ordering, which is a correctness requirement and
// not a preference: QTableModel::sort feeds this to std::stable_sort, where an
// inconsistent comparator is undefined behaviour (a crash), not a wrong order. It
// is therefore derived from a per-item RANK — a property of one item, never of the
// pair — so no comparison triangle can cycle:
//
//   0. a real number  — ordered by value
//   1. NaN            — compares false against every number in both directions, so
//                       it gets its own rank at one end instead of being mutually
//                       incomparable with everything (the classic SWO violation)
//   2. no value       — ordered by text, after every valued cell
//
// Rank 2 never text-compares against rank 0/1. Deciding that per pair is what
// cycles: with values 5 and 100 and a key-less cell showing "20", text says
// 100 < "20" < 5 while numbers say 5 < 100. Grouping the key-less cells at one end
// is also what the ulog Value column wants, where "N/A" cells carry no key.
class TypedTableItem : public QTableWidgetItem {
 public:
  // Reported by type(); lets applyTableRows spot a plain cell without a dynamic_cast.
  static constexpr int kType = QTableWidgetItem::UserType + 1;

  explicit TypedTableItem(const QString& text) : QTableWidgetItem(text, kType) {}

  [[nodiscard]] const std::optional<NumericValue>& sortValue() const {
    return value_;
  }
  void setSortValue(std::optional<NumericValue> value) {
    value_ = std::move(value);
  }

  [[nodiscard]] QTableWidgetItem* clone() const override {
    auto* copy = new TypedTableItem(QString{});
    // QTableWidgetItem's copy ctor resets the item type; its operator= copies every
    // role + the flags while leaving the (already correct) type alone.
    *static_cast<QTableWidgetItem*>(copy) = *this;
    copy->value_ = value_;
    return copy;
  }

  bool operator<(const QTableWidgetItem& other) const override {
    // Qt picks the comparator from the LEFT operand's dynamic type, so a column that
    // mixed plain and typed cells could answer one way as `plain < typed` (text) and
    // the other as `typed < plain` (rank) — asymmetric, hence UB. applyTableRows
    // keeps every column it writes homogeneous; text-comparing here is the matching
    // answer if some other path ever leaves a plain cell alongside a typed one.
    if (other.type() != kType) {
      return QTableWidgetItem::operator<(other);
    }
    const auto& rhs = static_cast<const TypedTableItem&>(other).value_;
    const int lhs_rank = rank(value_);
    const int rhs_rank = rank(rhs);
    if (lhs_rank != rhs_rank) {
      return lhs_rank < rhs_rank;
    }
    switch (lhs_rank) {
      case kRankNumber:
        return numericLess(*value_, *rhs);
      case kRankNan:
        return false;  // every NaN is equivalent to every other
      default:
        return QTableWidgetItem::operator<(other);
    }
  }

 private:
  static constexpr int kRankNumber = 0;
  static constexpr int kRankNan = 1;
  static constexpr int kRankText = 2;

  static int rank(const std::optional<NumericValue>& v) {
    if (!v.has_value()) {
      return kRankText;
    }
    return isNanValue(*v) ? kRankNan : kRankNumber;
  }

  std::optional<NumericValue> value_;
};

namespace {

// Whether a column's values can all be ordered against each other exactly.
//
// A column that mixes integers and floats is rejected wholesale (→ text ordering):
// there is no exact order across uint64 and double — uint64 exceeds int64's range
// and uint64→double loses precision above 2^53 — and ordering only SOME pairs of a
// column numerically breaks the strict weak ordering std::stable_sort demands.
// Rejecting per column rather than per pair is what keeps that decision consistent.
// JSON keeps integers and floats distinct, so no real column trips this; a column
// of mixed-sign integers (int64 + uint64 off the wire) is NOT mixed for this
// purpose — numericLess compares those exactly.
bool columnValuesComparable(const std::vector<std::optional<NumericValue>>& values) {
  std::optional<bool> floating;
  for (const auto& v : values) {
    if (!v.has_value()) {
      continue;
    }
    const bool is_float = isFloatingValue(*v);
    if (!floating.has_value()) {
      floating = is_float;
    } else if (*floating != is_float) {
      return false;
    }
  }
  return true;
}

}  // namespace

// Lazily suspends QTableWidget sorting for a batch of row/cell writes. Rows
// arrive in the plugin's own order and are written by model-row index; with
// sorting enabled QTableWidget physically re-sorts the model on every setItem/
// setText, so a mid-loop re-sort remaps the indices and the remaining writes
// land on the wrong rows (blank cells, name↔value pairs scrambled, duplicated
// rows). Call beforeWrite() ahead of every mutating call: sorting is suspended
// on the first one and restored once on destruction, so Qt applies a single
// clean sort — and a call that ends up writing nothing never toggles sorting
// and never pays a re-sort.
class ScopedSortSuspender {
 public:
  explicit ScopedSortSuspender(QTableWidget* tw) : tw_(tw), was_sorting_(tw->isSortingEnabled()) {}
  ~ScopedSortSuspender() {
    if (suspended_) {
      tw_->setSortingEnabled(true);
    }
  }
  ScopedSortSuspender(const ScopedSortSuspender&) = delete;
  ScopedSortSuspender& operator=(const ScopedSortSuspender&) = delete;

  void beforeWrite() {
    if (was_sorting_ && !suspended_) {
      tw_->setSortingEnabled(false);
      suspended_ = true;
    }
  }

 private:
  QTableWidget* tw_;
  bool was_sorting_;
  bool suspended_ = false;
};

// Push `rows` into the table with minimal churn. All table aspects
// (rows/selection/visibility) share one widget-data key, so every selection
// change and every streamed per-row detail update re-delivers the whole rows
// array. When the shape (row + column count) is unchanged — the common case —
// only the cells whose text actually differs are updated in place: this keeps
// the existing QTableWidgetItems (so selection + scroll survive), avoids the
// ResizeToContents re-measure a full rebuild triggers, and lets streamed detail
// fill in cell-by-cell instead of snapping in all at once. Only a row/column
// count change forces a full rebuild. Text-keyed selection restore
// (selected_items) runs later, once the sort has settled, and still matches rows.
// `column_values` holds the sparse per-column sort keys (column → one entry per
// row); a column absent from it, or a nullopt entry, orders by cell text.
static void applyTableRows(
    QTableWidget* tw, const std::vector<std::vector<std::string>>& rows,
    const std::map<int, std::vector<std::optional<NumericValue>>>& column_values) {
  ScopedSortSuspender sort_guard(tw);

  // Resolve the usable sort-key columns once, up front: a column is keyed only if
  // it indexes a real column, states a value for every row, and is exactly ordered
  // (see columnValuesComparable). Everything else falls through to text ordering.
  // Width comes from the WIDEST row, not the first: the SDK keys columns up to the
  // max row width, and a ragged delivery whose first row is short (a spanning
  // "Totals" row) must not silently drop the keys of every later column.
  std::size_t col_count = 0;
  for (const auto& row : rows) {
    col_count = std::max(col_count, row.size());
  }
  std::vector<const std::vector<std::optional<NumericValue>>*> col_keys(col_count, nullptr);
  for (const auto& [col, values] : column_values) {
    if (col >= 0 && static_cast<std::size_t>(col) < col_count && values.size() == rows.size() &&
        columnValuesComparable(values)) {
      col_keys[static_cast<std::size_t>(col)] = &values;
    }
  }
  auto key_at = [&col_keys](std::size_t r, std::size_t c) -> std::optional<NumericValue> {
    if (c >= col_keys.size() || col_keys[c] == nullptr) {
      return std::nullopt;
    }
    return (*col_keys[c])[r];
  };

  const bool same_shape = static_cast<std::size_t>(tw->rowCount()) == rows.size() &&
                          (rows.empty() || static_cast<std::size_t>(tw->columnCount()) == rows.front().size());
  if (same_shape) {
    for (std::size_t r = 0; r < rows.size(); ++r) {
      const auto& row = rows[r];
      // Iterate the TABLE's columns, not just the delivered row's: a ragged row
      // (shorter than the first row) must still blank/upgrade the cells it
      // omits, or a .ui-declared plain item survives next to typed neighbours —
      // and a column mixing plain and typed items orders some pairs by text and
      // others by value, which is not a strict weak ordering (UB in the sort).
      // A coordinate that has no item AND no delivered cell stays itemless.
      for (std::size_t c = 0; c < static_cast<std::size_t>(tw->columnCount()); ++c) {
        const bool delivered = c < row.size();
        const QString text = delivered ? QString::fromStdString(row[c]) : QString();
        std::optional<NumericValue> value = delivered ? key_at(r, c) : std::nullopt;
        QTableWidgetItem* item = tw->item(static_cast<int>(r), static_cast<int>(c));
        if (item == nullptr && !delivered) {
          continue;  // no item to sanitize and nothing to show — don't materialize one
        }
        auto* typed =
            (item != nullptr && item->type() == TypedTableItem::kType) ? static_cast<TypedTableItem*>(item) : nullptr;
        if (item == nullptr) {
          sort_guard.beforeWrite();
          typed = new TypedTableItem(text);
          typed->setSortValue(std::move(value));
          tw->setItem(static_cast<int>(r), static_cast<int>(c), typed);
        } else if (typed == nullptr) {
          // A cell the .ui declared. Upgrade it — unconditionally, even with no key
          // to stamp: TypedTableItem::operator< is only sound on a column whose
          // cells are all typed (see its comment), and a key-less typed cell orders
          // by text exactly as the plain one did.
          sort_guard.beforeWrite();
          typed = new TypedTableItem(text);
          *static_cast<QTableWidgetItem*>(typed) = *item;  // keep the roles/flags it already carried
          typed->setText(text);
          typed->setSortValue(std::move(value));
          tw->setItem(static_cast<int>(r), static_cast<int>(c), typed);  // deletes the plain item
        } else if (typed->text() != text || typed->sortValue() != value) {
          // A key that moved with unchanged text still re-orders the column, so it
          // has to go through the same suspend/restore as a text edit.
          sort_guard.beforeWrite();
          typed->setText(text);
          typed->setSortValue(std::move(value));
        }
        typed->setData(kPluginRowRole, static_cast<int>(r));
      }
    }
  } else {
    sort_guard.beforeWrite();
    const bool updates = tw->updatesEnabled();
    tw->setUpdatesEnabled(false);
    tw->setRowCount(static_cast<int>(rows.size()));
    const std::size_t table_cols = static_cast<std::size_t>(tw->columnCount());
    for (std::size_t r = 0; r < rows.size(); ++r) {
      const auto& row = rows[r];
      // Clamp to the table's width: QTableWidget::setItem does NOT range-check
      // the column — an out-of-range write lands in the NEXT row via the
      // flattened index (overwriting its first cell and its plugin-row tag) and
      // a past-the-end write leaks the item. A row wider than the headers is a
      // plugin bug; dropping its overflow cells is the safe rendering of it.
      for (std::size_t c = 0; c < row.size() && c < table_cols; ++c) {
        auto* item = new TypedTableItem(QString::fromStdString(row[c]));
        item->setSortValue(key_at(r, c));
        item->setData(kPluginRowRole, static_cast<int>(r));
        tw->setItem(static_cast<int>(r), static_cast<int>(c), item);
      }
      // setRowCount is a no-op when only the width changed, so cells this
      // delivery does not cover can still hold items from the previous shape —
      // stale text, stale sort keys, and above all a stale kPluginRowRole that
      // can duplicate another row's tag and corrupt the view<->plugin row
      // mapping (selection, visibility, radio, double-click). Drop them; cell
      // WIDGETS (e.g. lazily wired radios) are left for their own aspect.
      for (int c = static_cast<int>(row.size()); c < tw->columnCount(); ++c) {
        delete tw->takeItem(static_cast<int>(r), c);
      }
    }
    tw->setUpdatesEnabled(updates);
  }
}

namespace {
// Opt-in marker set in the plugin .ui, naming the sibling QListWidget that drives
// a table's column selection. The host hardcodes no widget name, so the
// list-drives-table-columns pairing stays domain-neutral.
constexpr const char* kColumnSelectorListProperty = "pjColumnSelectorList";
// A QPlainTextEdit that is a transcript/log: opens on its newest lines,
// follows them while the reader is at the end, and carries a "jump to the
// latest" overlay while they are not (pj_widgets/TailFollowOverlay.h).
constexpr const char* kFollowTailProperty = "pjFollowTail";
// A QListWidget's row context menu, value "id=Label[;id2=Label 2...]" (the
// same ';' clause separator the combo rules use). Not part of the dialog
// protocol — the host only reports which id fired
// (WidgetEventBuilder::itemContextAction); the action set itself is
// host-rendered UI the plugin declares declaratively, like pj_enable_when.
constexpr const char* kContextActionsProperty = "pj_context_actions";

// One row context-menu entry: its id (reported back on choice) and label
// (what the menu shows).
struct ContextMenuAction {
  QString id;
  QString label;
};

// Parses kContextActionsProperty's value. Unlike the combo rules, a malformed
// clause (no '=', empty id or label) is skipped rather than invalidating the
// whole property — one broken entry just means one fewer menu item, not a
// governed widget silently losing its enable/visible rule.
std::vector<ContextMenuAction> parseContextActions(const QString& value) {
  std::vector<ContextMenuAction> actions;
  for (const QString& raw_clause : value.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
    const QString clause = raw_clause.trimmed();
    const qsizetype eq = clause.indexOf(QLatin1Char('='));
    if (eq <= 0 || eq == clause.size() - 1) {
      continue;
    }
    QString id = clause.left(eq).trimmed();
    QString label = clause.mid(eq + 1).trimmed();
    if (id.isEmpty() || label.isEmpty()) {
      continue;
    }
    actions.push_back({std::move(id), std::move(label)});
  }
  return actions;
}

// Grey out an item while leaving it visible: a disabled item is neither enabled
// nor selectable. Shared by the list and table disable paths.
template <typename ItemT>
void setItemEnabled(ItemT* item, bool enabled) {
  Qt::ItemFlags flags = item->flags();
  flags.setFlag(Qt::ItemIsEnabled, enabled);
  flags.setFlag(Qt::ItemIsSelectable, enabled);
  item->setFlags(flags);
}

// Swallows user selection gestures (mouse press/release/double-click) on the view
// it filters, so the view's selection can only be driven programmatically. Used by
// the column-selector wiring below; keyboard selection is separately blocked by
// NoFocus on the host widget. eventFilter is a QObject virtual, so no moc is needed.
class SelectionGestureBlocker : public QObject {
 public:
  using QObject::QObject;

 protected:
  bool eventFilter(QObject* /*obj*/, QEvent* event) override {
    switch (event->type()) {
      case QEvent::MouseButtonPress:
      case QEvent::MouseButtonRelease:
      case QEvent::MouseButtonDblClick:
        return true;
      default:
        return false;
    }
  }
};

// Mirror a list's current selection onto its column-selector table: select every
// column whose header text matches a selected list row, clearing the rest. Purely
// programmatic (no signal dependency), so it stays correct whether the list
// selection changed via the user or via applyToWidget (which sets it under a
// signal blocker, so itemSelectionChanged never fires for programmatic changes).
static void syncColumnSelectorTable(QTableWidget* tw, QListWidget* list) {
  const QList<QListWidgetItem*> chosen = list->selectedItems();
  if (chosen.isEmpty() || tw->columnCount() == 0 || tw->rowCount() == 0) {
    tw->clearSelection();
    return;
  }
  std::set<QString> wanted;
  for (const QListWidgetItem* item : chosen) {
    wanted.insert(item->text());
  }
  QItemSelection selection;
  for (int c = 0; c < tw->columnCount(); ++c) {
    const QTableWidgetItem* header = tw->horizontalHeaderItem(c);
    if (header != nullptr && wanted.count(header->text()) > 0) {
      selection.select(tw->model()->index(0, c), tw->model()->index(tw->rowCount() - 1, c));
    }
  }
  tw->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Columns);
}

// Re-sync every column-selector table whose list — or the table itself — carries
// data in this payload. applyToWidget sets list selection under a per-widget
// signal blocker, so itemSelectionChanged never fires for a programmatic change;
// without this pass a plugin-driven change (e.g. clearing the selection on a mode
// switch) would leave a stale column highlight. applyWidgetData runs on every tick
// carrying any change at all, so anything else would rebuild the selection at the
// dialog's full tick rate.
void syncColumnSelectorTables(QWidget* root, const std::vector<std::string>& payload_names) {
  const auto in_payload = [&payload_names](const QString& widget_name) {
    return std::find(payload_names.begin(), payload_names.end(), widget_name.toStdString()) != payload_names.end();
  };
  for (auto* tw : findAllIn<QTableWidget*>(root)) {
    const QString list_name = tw->property(kColumnSelectorListProperty).toString();
    if (list_name.isEmpty() || (!in_payload(list_name) && !in_payload(tw->objectName()))) {
      continue;
    }
    if (auto* list = findNamedIn<QListWidget*>(root, list_name)) {
      syncColumnSelectorTable(tw, list);
    }
  }
}

// Bridges radio-cell clicks back to the dialog event stream. connectWidgetSignals
// owns the event callback but the radio cells don't exist yet then (rows arrive
// later via applyWidgetData), so it stashes this holder on the table and
// applyTableRadioColumn wires each radio to it as rows materialise. Found by a
// fixed objectName + static_cast (no Q_OBJECT / moc needed in this TU).
class RadioEmitHolder : public QObject {
 public:
  RadioEmitHolder(QObject* parent, std::function<void(int)> fn) : QObject(parent), emit_row(std::move(fn)) {
    setObjectName(u"pj_radio_emit_holder"_s);
  }
  std::function<void(int)> emit_row;
};

// The canonical Material trash icon (:/resources/svg/trash.svg, the same glyph
// LayerListView / Scene3DConfigPanel use), tinted to `ink` and rasterised from
// the vector at the target DEVICE resolution (extent * dpr) so it stays crisp on
// HiDPI — rendering at logical size and letting the view upscale is what made it
// fuzzy. Cached by (ink, extent, dpr).
QPixmap rowTrashPixmap(const QColor& ink, int extent, qreal dpr) {
  static std::map<std::tuple<QRgb, int, qint64>, QPixmap> cache;
  const auto key = std::make_tuple(ink.rgba(), extent, qRound64(dpr * 100));
  auto it = cache.find(key);
  if (it != cache.end()) {
    return it->second;
  }
  QString svg;
  QFile file(QStringLiteral(":/resources/svg/trash.svg"));
  if (file.open(QIODevice::ReadOnly)) {
    svg = QString::fromUtf8(file.readAll());
  }
  // trash.svg paints a single fill="#3D3D3D"; recolour it to the row ink.
  svg.replace(QStringLiteral("#3D3D3D"), ink.name(QColor::HexRgb), Qt::CaseInsensitive);
  const int px = qMax(1, qRound(extent * dpr));
  QPixmap pix(px, px);
  pix.fill(Qt::transparent);
  QSvgRenderer renderer(svg.toUtf8());
  if (renderer.isValid()) {
    QPainter painter(&pix);
    renderer.render(&painter);
  }
  pix.setDevicePixelRatio(dpr);
  cache.emplace(key, pix);
  return pix;
}

// Paints a trailing trash icon on the last column of each delegated row and
// turns a click on that icon into an itemDeleteRequested(row) event. Only active
// when the item view carries a true "pj_deletable" dynamic property (set from
// WidgetData), so the same delegate is harmless on non-deletable views. Clicks
// off the icon fall through untouched, so selection and double-click still work.
class ListRowDeleteDelegate : public QStyledItemDelegate {
 public:
  static constexpr int kIconExtent = 16;
  static constexpr int kIconMargin = 6;
  static constexpr int kRowVPadding = 6;  // matches QPushButton's QSS padding

  ListRowDeleteDelegate(QObject* parent, std::function<void(int)> on_delete)
      : QStyledItemDelegate(parent), on_delete_(std::move(on_delete)) {}

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    if (!deletable() || !isLastColumn(index)) {
      QStyledItemDelegate::paint(painter, option, index);
      return;
    }
    // A row wider than the viewport (long text in a narrow list) would put the
    // icon past the visible edge, where it cannot be seen or clicked. Anchor it
    // to the visible edge instead, and elide the text so it stops short of it.
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.rect = rowRectInView(option);
    opt.text = opt.fontMetrics.elidedText(opt.text, Qt::ElideRight, textBudget(option, index));
    const QWidget* widget = option.widget;
    QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
    const qreal dpr = painter->device() != nullptr ? painter->device()->devicePixelRatioF() : 1.0;
    painter->drawPixmap(iconRect(opt.rect), rowTrashPixmap(option.palette.color(QPalette::Text), kIconExtent, dpr));
  }

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    if (deletable() && isLastColumn(index)) {
      s.setWidth(s.width() + kIconExtent + (2 * kIconMargin));
      // Match the Save button's height: text + the same 6px vertical padding a
      // QPushButton uses, so a row reads as the same size as the button below.
      s.setHeight(qMax(s.height(), option.fontMetrics.height() + (2 * kRowVPadding)));
    }
    return s;
  }

  bool editorEvent(
      QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index) override {
    const QEvent::Type type = event->type();
    const bool mouse_button =
        type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick;
    if (deletable() && isLastColumn(index) && mouse_button) {
      const auto* me = static_cast<QMouseEvent*>(event);
      if (me->button() == Qt::LeftButton && iconRect(rowRectInView(option)).contains(me->pos())) {
        // The press is consumed too, not just the release: the view selects a
        // row on press, and a plugin that reacts to selection (switching what
        // the panel shows, hiding the list) can take the widget away before
        // the release that was meant to delete ever arrives.
        if (type == QEvent::MouseButtonRelease) {
          // Report the delivered-order (plugin) index, not the view row: on a
          // sorted list index.row() names a DIFFERENT underlying item, so the
          // trash button would delete the wrong thing (double-click already
          // translates the same way — see listItemPluginIndex).
          const QVariant tag = index.data(kPluginRowRole);
          on_delete_(tag.isValid() ? tag.toInt() : index.row());
        }
        return true;
      }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
  }

  // Elided rows lose their tail to paint()'s budget above with no other way to
  // read it back, so surface the full text as a tooltip whenever it doesn't
  // fit that same budget; a row that already fits shows none (falling through
  // to the base class, which has nothing to add — no Qt::ToolTipRole is ever
  // set on these items).
  bool helpEvent(
      QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option,
      const QModelIndex& index) override {
    if (event == nullptr || event->type() != QEvent::ToolTip) {
      return QStyledItemDelegate::helpEvent(event, view, option, index);
    }
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    if (opt.fontMetrics.horizontalAdvance(opt.text) > textBudget(option, index)) {
      QToolTip::showText(event->globalPos(), opt.text, view);
      return true;
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
  }

 private:
  // The width paint() elides text into: the visible row minus the icon
  // reservation when the trash icon is actually drawn on this (deletable,
  // last) column, or the full visible row otherwise — a plain list elides
  // through the style, with no icon to make room for. Shared with helpEvent
  // so "does it fit" can never disagree between the paint and the tooltip.
  [[nodiscard]] int textBudget(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const int row_width = rowRectInView(option).width();
    if (!deletable() || !isLastColumn(index)) {
      return row_width;
    }
    return qMax(0, row_width - kIconExtent - (3 * kIconMargin));
  }

  [[nodiscard]] bool deletable() const {
    const auto* view = qobject_cast<const QWidget*>(parent());
    return view != nullptr && view->property("pj_deletable").toBool();
  }
  [[nodiscard]] static bool isLastColumn(const QModelIndex& index) {
    return index.model() != nullptr && index.column() == index.model()->columnCount() - 1;
  }
  // The row's rect clipped to what the viewport actually shows. Both are in
  // viewport coordinates, so this holds under horizontal scrolling too.
  [[nodiscard]] static QRect rowRectInView(const QStyleOptionViewItem& option) {
    QRect row = option.rect;
    if (const auto* view = qobject_cast<const QAbstractItemView*>(option.widget);
        view != nullptr && view->viewport() != nullptr) {
      row.setRight(qMin(row.right(), view->viewport()->rect().right()));
    }
    return row;
  }
  [[nodiscard]] static QRect iconRect(const QRect& row) {
    return {
        row.right() - kIconExtent - kIconMargin, row.top() + ((row.height() - kIconExtent) / 2), kIconExtent,
        kIconExtent};
  }
  std::function<void(int)> on_delete_;
};
}  // namespace

// Render `col` of `tw` as an exclusive radio group (one QRadioButton per row),
// check `checked_row`, and wire each radio's click to emit_row(its current row).
// Idempotent: reuses existing radios and only syncs the checked state, so it can
// run on every data apply without flicker. Rows that shrink away have their cell
// widgets destroyed by QTableWidget::setRowCount, which auto-removes them from the
// QButtonGroup. `clicked` fires on user interaction only (NOT on setChecked), so
// the checked-state sync below never feeds back as a spurious event.
static void applyTableRadioColumn(
    QTableWidget* tw, int col, int checked_row, const std::function<void(int)>& emit_row) {
  if (col < 0 || col >= tw->columnCount()) {
    return;
  }
  auto* group = tw->findChild<QButtonGroup*>(u"pj_radio_group"_s, Qt::FindDirectChildrenOnly);
  if (group == nullptr) {
    group = new QButtonGroup(tw);
    group->setObjectName(u"pj_radio_group"_s);
    group->setExclusive(true);
  }
  for (int r = 0; r < tw->rowCount(); ++r) {
    auto* radio = qobject_cast<QRadioButton*>(tw->cellWidget(r, col));
    if (radio == nullptr) {
      radio = new QRadioButton(tw);
      radio->setStyleSheet(u"QRadioButton { margin-left: %1px; }"_s.arg(theme::space(theme::Space::Comfortable)));
      tw->setCellWidget(r, col, radio);
      group->addButton(radio);
      // Resolve the row at click time: rows renumber as the user adds/removes
      // series, so a row index captured at creation would go stale.
      QObject::connect(radio, &QRadioButton::clicked, radio, [emit_row, tw, col, radio]() {
        for (int rr = 0; rr < tw->rowCount(); ++rr) {
          if (tw->cellWidget(rr, col) == radio) {
            emit_row(rr);
            return;
          }
        }
      });
    }
    radio->setChecked(r == checked_row);
  }

  // Keep the radio column just wide enough for the button, and let the first
  // non-radio column be the one that absorbs leftover width — installTreeLikeHeader
  // nominates column 0, which would over-widen the radio when it comes first.
  //
  // The radio column stays Interactive rather than Fixed: a Fixed section refuses
  // to be dragged, which used to leave these tables with no resizable divider at
  // all. Its width is simply seeded narrow, and the shared policy keeps the row
  // spanning the viewport.
  auto* header = tw->horizontalHeader();
  int fill_column = kNameColumn;
  for (int c = 0; c < tw->columnCount(); ++c) {
    if (c != col) {
      fill_column = c;
      break;
    }
  }
  auto* policy = PJ::HeaderResizePolicy::install(header, fill_column, 20);
  policy->setFillSection(fill_column);
  policy->setSectionWidth(col, kRadioColumnWidth);
}

// Key text identifying a table row for text-keyed selection (selected_items
// apply + selection-changed emit). Plugin-fed tables read the key column
// recorded at delivery time (_pj_plugin_key_col, see recordPluginKeyColumn);
// it is authoritative even before radio cell widgets materialise. Tables
// never fed by a delivery (predefined in a .ui) fall back to scanning for the
// first column that hosts no cell widget — columns with one (e.g. an
// exclusive radio column) carry no selectable item text. The scan stops at
// that column even if it has no item (nullopt), rather than falling through
// to a later column.
static std::optional<std::string> tableRowKeyText(const QTableWidget* tw, int row) {
  const QVariant recorded_col = tw->property("_pj_plugin_key_col");
  if (recorded_col.isValid()) {
    if (auto* item = tw->item(row, recorded_col.toInt())) {
      return item->text().toStdString();
    }
    return std::nullopt;
  }
  for (int c = 0; c < tw->columnCount(); ++c) {
    if (tw->cellWidget(row, c) == nullptr) {
      if (auto* item = tw->item(row, c)) {
        return item->text().toStdString();
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Plugin-order ↔ view-order row translation.
//
// Plugins deliver `rows` in their own order and key every index-based aspect
// (selected_rows, visible_rows, disabled_rows, radio_checked_row) — and
// interpret every emitted index (table_radio_row, item_double_clicked_index) —
// against that order. With sortingEnabled the user can re-order the view, so
// the host translates row indices in both directions. Rows are identified by
// the kPluginRowRole tag applyTableRows() stamps on every item, not by text,
// so two rows sharing identical key-column text still resolve to their own
// distinct plugin index (a text-keyed lookup could not tell them apart).
// Tables whose rows never came from a plugin delivery (predefined in a .ui)
// have no tagged items and keep raw-index semantics via the identity fallback.
// ---------------------------------------------------------------------------

// Record the key column for this delivery. `radio_col` is the column rendered
// as radio widgets this delivery (or -1): it carries no item text, so the key
// column is the first column other than it. Consumed by tableRowKeyText,
// which needs the key column even before radio cell widgets materialise; row
// index translation does not need this (see viewToPluginRowMap).
static void recordPluginKeyColumn(QTableWidget* tw, int radio_col) {
  tw->setProperty("_pj_plugin_key_col", radio_col == 0 ? 1 : 0);
}

// view row -> plugin row for every current row, read directly off each row's
// kPluginRowRole item tag. Falls back to identity (raw row index) for a row
// whose items were never tagged — normal for a .ui-predefined table never fed
// by a plugin delivery.
static std::vector<int> viewToPluginRowMap(const QTableWidget* tw) {
  std::vector<int> map(static_cast<std::size_t>(tw->rowCount()));
  for (int r = 0; r < tw->rowCount(); ++r) {
    int plugin_row = r;
    for (int c = 0; c < tw->columnCount(); ++c) {
      if (const QTableWidgetItem* item = tw->item(r, c)) {
        const QVariant tag = item->data(kPluginRowRole);
        if (tag.isValid()) {
          plugin_row = tag.toInt();
        }
        break;
      }
    }
    map[static_cast<std::size_t>(r)] = plugin_row;
  }
  return map;
}

// Inverse of a viewToPluginRowMap result: plugin row -> current view row.
static std::vector<int> invertRowMap(const std::vector<int>& view_to_plugin) {
  std::vector<int> inverse(view_to_plugin.size());
  for (std::size_t r = 0; r < view_to_plugin.size(); ++r) {
    inverse[static_cast<std::size_t>(view_to_plugin[r])] = static_cast<int>(r);
  }
  return inverse;
}

// Emit-direction translation for a single row (radio click, double-click).
static int viewRowToPluginRow(const QTableWidget* tw, int view_row) {
  const std::vector<int> map = viewToPluginRowMap(tw);
  if (view_row < 0 || static_cast<std::size_t>(view_row) >= map.size()) {
    return view_row;
  }
  return map[static_cast<std::size_t>(view_row)];
}

// Plugin index of a list item: the kPluginRowRole tag stamped on it when
// listItems was applied — anchored to the item itself, so it survives list
// sorting and needs no text matching (safe even with duplicate item text).
// Falls back to the raw view row for lists never fed by a delivery.
static int listItemPluginIndex(const QListWidget* lw, const QListWidgetItem* item) {
  const QVariant tag = item->data(kPluginRowRole);
  return tag.isValid() ? tag.toInt() : lw->row(item);
}

// True when `tw`'s header labels already equal `headers`.
static bool tableMatchesHeaders(const QTableWidget* tw, const QStringList& headers) {
  if (tw->columnCount() != headers.size()) {
    return false;
  }
  for (int i = 0; i < headers.size(); ++i) {
    const QTableWidgetItem* h = tw->horizontalHeaderItem(i);
    if (h == nullptr || h->text() != headers[i]) {
      return false;
    }
  }
  return true;
}

// Size a topic/curve table the way it reads best: the first (name) column is the
// widest and soaks up the leftover width, while the data columns start at a
// readable default and can be dragged. Header weight is left to the app
// stylesheet (the global `QHeaderView::section { font-weight: normal }` rule),
// which reads consistently with CurveTreeView — a widget-side setFont would be
// ignored while a stylesheet is active anyway.
//
// Sizing is handled by the shared HeaderResizePolicy so these tables drag exactly
// like CurveTreeView: every divider is live and follows the cursor, and the next
// column gives or takes the width. The name column is the policy's fill section,
// which is what keeps dead grey space off the right edge.
//
// The initial data-column widths are applied once and kept, so this is guarded by
// a dynamic property and a column-count check: it's safe to call on every
// widget_data delivery — it configures the first time the table actually has
// columns and no-ops after. This is deliberately NOT gated on the header *labels*
// changing: dialogs whose .ui predefines column headers (e.g. MCAP's tableWidget)
// match the plugin's setTableHeaders() verbatim, so a label-change gate would skip
// them entirely and leave the .ui's default sizing.
#ifdef PJ_TARGET_WASM
// Re-clamps a horizontal QHeaderView to its sizeHint height whenever Qt-wasm's
// QTableView resizes it (see installTreeLikeHeader). Owned by the header it
// watches. No Q_OBJECT: it overrides only the virtual eventFilter.
class WasmHeaderHeightClamp : public QObject {
 public:
  explicit WasmHeaderHeightClamp(QHeaderView* header) : QObject(header), header_(header) {}

  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() == QEvent::Resize && watched == header_) {
      const int want = header_->sizeHint().height();
      // Only act when the height is wrong, so the setFixedHeight-triggered
      // resize back to `want` is a no-op the next time through (no loop).
      if (want > 0 && header_->height() != want) {
        header_->setFixedHeight(want);
      }
    }
    return QObject::eventFilter(watched, event);
  }

 private:
  QHeaderView* header_;
};
#endif

static void installTreeLikeHeader(QTableWidget* tw, int stretch_col = -1) {
  auto* header = tw->horizontalHeader();
  if (header->count() == 0) {
    return;
  }

  // A nominated content column (typically the series name, just after a leading
  // radio marker) is sized EVERY call — setSectionResizeMode is idempotent, so
  // the stretch still lands if the first delivery hadn't yet carried the radio
  // column key (and so it is NOT gated by the one-time guard below). The VIEW
  // drives these built-in modes on its own resize, so the content column fills
  // whether or not the header is shown; the narrow marker columns hug content.
  if (stretch_col >= 0 && stretch_col < header->count()) {
    for (int i = 0; i < header->count(); ++i) {
      header->setSectionResizeMode(i, i == stretch_col ? QHeaderView::Stretch : QHeaderView::ResizeToContents);
    }
    tw->setProperty("pjContentStretch", true);
    return;
  }

  // Once a content-column stretch is established, a later delivery that lacks the
  // radio-column key (a stable value drops out of the diff) must NOT fall through
  // and overwrite it with the uniform/list-like sizing below.
  if (tw->property("pjContentStretch").toBool()) {
    return;
  }
  // The one-time policy install below must run once, not per delivery.
  if (tw->property("pjTreeLikeHeader").toBool()) {
    return;
  }
  tw->setProperty("pjTreeLikeHeader", true);

  // A .ui can hide the horizontal header (a list-like table has no column labels
  // worth showing). Nothing can be dragged or highlighted then, and the policy
  // would in fact freeze the columns at Qt's default section width: it rebalances
  // off the header's own resize event, which a hidden header never receives — so
  // rows end up a fraction of the table wide, with the row-delete trash parked
  // mid-row on top of the text. Qt's Stretch mode is driven by the view instead
  // and keeps every section spanning the viewport, at any column count.
  if (header->isHidden()) {
    header->setSectionResizeMode(QHeaderView::Stretch);
    return;
  }

  auto* policy = PJ::HeaderResizePolicy::install(header, kNameColumn, 20);
  PJ::HeaderDividerHighlight::install(header);
  for (int i = kNameColumn + 1; i < header->count(); ++i) {
    policy->setSectionWidth(i, kDataColumnWidth);
  }

#ifdef PJ_TARGET_WASM
  // Qt-for-WebAssembly's QTableView lays the horizontal header out at the full
  // table height instead of its sizeHint (a platform layout bug; desktop clamps
  // correctly), collapsing the row viewport to zero — the rows exist but can't
  // paint. QTableView positions the header with a direct setGeometry that
  // bypasses maximumHeight, and it re-runs on every relayout, so a one-shot
  // setFixedHeight at build time gets overwritten. Re-clamp the header to its
  // sizeHint every time it is resized; it converges (setFixedHeight sticks once
  // the layout has settled) and self-heals on later relayouts.
  header->installEventFilter(new WasmHeaderHeightClamp(header));
#endif
}

// Write a delta-provided cell's text/value into (row, col): update in place if
// it's already typed, upgrade a plain .ui-declared item (keeping its roles/
// flags), or create a new typed cell if none exists yet. Always leaves a
// homogeneous typed cell behind — never a plain QTableWidgetItem — so a
// delta-only workflow can't leave a column mixing plain and typed items (see
// TypedTableItem's own comment on why that is unsound to sort).
static void upsertDeltaCell(
    QTableWidget* tw, int row, int col, const QString& text, std::optional<NumericValue> value, int plugin_row) {
  QTableWidgetItem* item = tw->item(row, col);
  if (item == nullptr) {
    auto* created = new TypedTableItem(text);
    created->setSortValue(std::move(value));
    created->setData(kPluginRowRole, plugin_row);
    tw->setItem(row, col, created);
  } else if (item->type() == TypedTableItem::kType) {
    auto* typed = static_cast<TypedTableItem*>(item);
    typed->setText(text);
    typed->setSortValue(std::move(value));
  } else {
    auto* typed = new TypedTableItem(text);
    *static_cast<QTableWidgetItem*>(typed) = *item;  // keep the roles/flags it already carried
    typed->setText(text);
    typed->setSortValue(std::move(value));
    tw->setItem(row, col, typed);  // deletes the plain item
  }
}

// Apply one batch table delta (already seq-gated by the caller). Protocol
// order: update_cells, then remove_rows, then append — all indexes in the
// pre-delta plugin row space. Works under user sorting by translating plugin
// rows through kPluginRowRole; sorting is suspended so mid-loop re-sorts
// cannot remap indexes, and the roles are renumbered afterwards (removals
// shift the plugin space down; appends take the next indexes).
// Returns false — applying nothing — when any update/remove op fails to
// resolve against the current table: the protocol's whole-delta rejection, so
// the caller leaves the seq unconsumed and a corrected retransmission of the
// same seq still applies.
static bool applyTableDelta(QTableWidget* tw, const PJ::WidgetDataView::TableDeltaView& delta) {
  if (delta.update_cells.empty() && delta.remove_rows.empty() && delta.append.empty()) {
    return true;
  }
  ScopedSortSuspender sort_guard(tw);

  // A table seeded by a predefined .ui (never via applyTableRows) carries no
  // plugin-row tags; stamp the identity mapping first so the machinery below
  // can rely on tags existing (renumbering reads them directly). Every item
  // created after this pass is born tagged, so one pass suffices — the
  // property keeps the O(rows×cols) rescan off the per-tick delta path.
  if (!tw->property("_pj_row_tags_seeded").toBool()) {
    for (int r = 0; r < tw->rowCount(); ++r) {
      for (int c = 0; c < tw->columnCount(); ++c) {
        if (QTableWidgetItem* item = tw->item(r, c); item != nullptr && !item->data(kPluginRowRole).isValid()) {
          sort_guard.beforeWrite();
          item->setData(kPluginRowRole, r);
        }
      }
    }
    tw->setProperty("_pj_row_tags_seeded", true);
  }

  // Row-index translation is read only by update_cells / remove_rows;
  // append-only deltas (the common streaming shape) skip the O(rows) map build.
  std::vector<int> plugin_to_view;
  if (!delta.update_cells.empty() || !delta.remove_rows.empty()) {
    plugin_to_view = invertRowMap(viewToPluginRowMap(tw));
  }
  const auto model_row_of = [&plugin_to_view](int plugin_row) -> int {
    return plugin_row >= 0 && static_cast<std::size_t>(plugin_row) < plugin_to_view.size()
               ? plugin_to_view[static_cast<std::size_t>(plugin_row)]
               : -1;
  };

  // Validate every targeted op BEFORE the first content write: one
  // unresolvable target rejects the delta whole (never partially applied),
  // mirroring the decoder's strictness — a partially-applied delta would leave
  // the table diverged from the plugin's model with no way to repair it.
  for (const auto& cell : delta.update_cells) {
    if (model_row_of(cell.row) < 0 || cell.col >= tw->columnCount()) {
      return false;
    }
  }
  for (int plugin_row : delta.remove_rows) {
    if (model_row_of(plugin_row) < 0) {
      return false;
    }
  }

  // An update replaces the WHOLE cell (see TableDeltaView::CellUpdate::value's
  // doc-comment) — text and sort key must move together, or a typed column
  // desyncs its displayed order from what's on screen.
  for (const auto& cell : delta.update_cells) {
    const int row = model_row_of(cell.row);
    sort_guard.beforeWrite();
    upsertDeltaCell(tw, row, cell.col, QString::fromStdString(cell.text), cell.value, cell.row);
  }

  // Translate every plugin index up front — removing re-indexes the model —
  // then delete in descending model order.
  std::vector<int> doomed_model_rows;
  doomed_model_rows.reserve(delta.remove_rows.size());
  for (int plugin_row : delta.remove_rows) {
    doomed_model_rows.push_back(model_row_of(plugin_row));
  }
  std::sort(doomed_model_rows.begin(), doomed_model_rows.end(), std::greater<>());
  for (int row : doomed_model_rows) {
    sort_guard.beforeWrite();
    tw->removeRow(row);
  }

  // Close the gaps in the plugin space: each surviving index drops by the
  // number of removed indexes below it. remove_rows arrives descending, so an
  // ascending copy turns that count into one binary search per row (all cells
  // of a row share the same plugin index).
  if (!delta.remove_rows.empty()) {
    sort_guard.beforeWrite();
    const std::vector<int> removed_asc(delta.remove_rows.rbegin(), delta.remove_rows.rend());
    for (int r = 0; r < tw->rowCount(); ++r) {
      int new_plugin = -1;
      for (int c = 0; c < tw->columnCount(); ++c) {
        if (QTableWidgetItem* item = tw->item(r, c)) {
          if (new_plugin < 0) {
            const int old_plugin = item->data(kPluginRowRole).toInt();
            const auto shift =
                std::lower_bound(removed_asc.begin(), removed_asc.end(), old_plugin) - removed_asc.begin();
            new_plugin = old_plugin - static_cast<int>(shift);
          }
          item->setData(kPluginRowRole, new_plugin);
        }
      }
    }
  }

  // Resolve append_values by column once, up front — same sparse-map-to-flat
  // idea as applyTableRows' col_keys/key_at — instead of a map lookup per cell.
  std::vector<const std::vector<std::optional<NumericValue>>*> append_col_keys(
      static_cast<std::size_t>(tw->columnCount()), nullptr);
  for (const auto& [col, values] : delta.append_values) {
    if (col >= 0 && static_cast<std::size_t>(col) < append_col_keys.size()) {
      append_col_keys[static_cast<std::size_t>(col)] = &values;
    }
  }

  // Appends take the next plugin indexes; roles are contiguous 0..rowCount-1
  // after the renumbering above, so rowCount is the next free index.
  int next_plugin_row = tw->rowCount();
  std::size_t append_row_idx = 0;
  for (const auto& row_cells : delta.append) {
    sort_guard.beforeWrite();
    const int row = tw->rowCount();
    tw->insertRow(row);
    // Create an item for EVERY column (empty text for missing cells): a row
    // without items has no plugin-row tag, and a later re-sort would desync
    // the row-identity mapping.
    for (std::size_t c = 0; c < append_col_keys.size(); ++c) {
      const bool has_cell = c < row_cells.size();
      const QString text = has_cell ? QString::fromStdString(row_cells[c]) : QString();
      std::optional<NumericValue> value = append_col_keys[c] != nullptr && append_row_idx < append_col_keys[c]->size()
                                              ? (*append_col_keys[c])[append_row_idx]
                                              : std::nullopt;
      upsertDeltaCell(tw, row, static_cast<int>(c), text, std::move(value), next_plugin_row);
    }
    ++next_plugin_row;
    ++append_row_idx;
  }
  return true;
}

// The text last pushed into a pjFollowTail edit, kept so the next push can be
// told apart as growth (a prefix match, O(n) memcmp) or a replacement without
// re-serializing the document.
constexpr const char* kFollowTailLastTextProperty = "_pj_follow_tail_text";
constexpr const char* kMarkdownLastTextProperty = "_pj_markdown_text";
constexpr const char* kMarkdownConfiguredProperty = "_pj_markdown_configured";

// Attach a `TailFollowOverlay` to `view`'s viewport, unless one is already
// there (idempotent — both text-transport paths call this on every apply).
static void ensureTailFollowOverlay(QAbstractScrollArea* view) {
  if (view->viewport()->findChild<QWidget*>(QStringLiteral("tailFollowOverlay"), Qt::FindDirectChildrenOnly) ==
      nullptr) {
    new TailFollowOverlay(view);
  }
}

// pjFollowTail semantics for a QPlainTextEdit. A transcript grows at its end,
// so growth is INSERTED there instead of re-set wholesale: the blocks already
// laid out keep their layout and the viewport's top line does not move, which
// is what keeps a scrolled-up reader in place. (setPlainText would re-lay the
// whole document lazily — its scroll range reads low until the wrapped blocks
// are measured, and a "was at the bottom?" check against that range lies.) A
// reader at the end follows the new lines; a first population or a wholesale
// replacement (another conversation swapped in) opens on the newest lines.
static void applyFollowTailText(QPlainTextEdit* pte, const QString& new_text) {
  const QString last = pte->property(kFollowTailLastTextProperty).toString();
  QScrollBar* sb = pte->verticalScrollBar();
  const bool grew = !last.isEmpty() && new_text.size() >= last.size() && new_text.startsWith(last);
  if (grew) {
    const bool at_end = sb->value() >= sb->maximum();
    if (new_text.size() > last.size()) {
      QTextCursor tail(pte->document());
      tail.movePosition(QTextCursor::End);
      tail.insertText(new_text.mid(last.size()));
    }
    if (at_end) {
      // The cursor round trip only forces the true end to be laid out (so `maximum` is real);
      // the cursor and any selection belong to the reader, and the scrollbar follows the tail.
      const QTextCursor reader_cursor = pte->textCursor();
      pte->moveCursor(QTextCursor::End);
      pte->setTextCursor(reader_cursor);
      sb->setValue(sb->maximum());
    }
  } else {
    pte->setPlainText(new_text);
    pte->moveCursor(QTextCursor::End);
    sb->setValue(sb->maximum());
  }
  pte->setProperty(kFollowTailLastTextProperty, new_text);
  ensureTailFollowOverlay(pte);
}

static bool isFenceLine(QStringView line) {
  const QStringView trimmed = line.trimmed();
  if (trimmed.size() < 3 || (trimmed.front() != u'`' && trimmed.front() != u'~')) {
    return false;
  }
  for (const QChar ch : trimmed) {
    if (ch != trimmed.front()) {
      return false;
    }
  }
  return true;
}

// True when a changed source is a rewrite of the live tail rather than another
// conversation. Plugin renderers commonly keep terminal paragraph separators
// and temporarily close an open code fence, so raw startsWith(last) is too
// strict. Reaching into the final content block is enough evidence to preserve
// the reader. With text-only transport, a replacement sharing that same prefix
// is inherently indistinguishable and receives the conservative preserve-view
// behavior too.
static bool isMarkdownTailRewrite(const QString& last, const QString& next) {
  if (last.isEmpty() || next.isEmpty()) {
    return false;
  }
  qsizetype common = 0;
  const qsizetype common_limit = std::min(last.size(), next.size());
  while (common < common_limit && last[common] == next[common]) {
    ++common;
  }

  qsizetype content_end = last.size();
  while (content_end > 0 && last[content_end - 1].isSpace()) {
    --content_end;
  }
  if (content_end > 0) {
    const qsizetype final_line_start = last.lastIndexOf(u'\n', content_end - 1) + 1;
    if (isFenceLine(QStringView(last).mid(final_line_start, content_end - final_line_start))) {
      content_end = final_line_start;
      while (content_end > 0 && last[content_end - 1].isSpace()) {
        --content_end;
      }
    }
  }

  const qsizetype separator = content_end > 0 ? last.lastIndexOf(u"\n\n", content_end - 1) : -1;
  const qsizetype final_block_start = separator < 0 ? 0 : separator + 2;
  return common > final_block_start;
}

static void configureMarkdownBrowser(QTextBrowser* browser) {
  if (browser->property(kMarkdownConfiguredProperty).toBool()) {
    return;
  }
  browser->setProperty(kMarkdownConfiguredProperty, true);
  browser->setReadOnly(true);
  browser->setTextInteractionFlags(Qt::TextBrowserInteraction);
  browser->setOpenLinks(false);
  browser->setOpenExternalLinks(false);
  browser->setAcceptDrops(false);
  QObject::connect(browser, &QTextBrowser::anchorClicked, browser, [](const QUrl& url) {
    const QString scheme = url.scheme();
    if (url.isValid() &&
        (scheme.compare(u"http", Qt::CaseInsensitive) == 0 || scheme.compare(u"https", Qt::CaseInsensitive) == 0)) {
      QDesktopServices::openUrl(url);
    }
  });
}

static QTextDocument::MarkdownFeatures markdownFeatures() {
  return QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML;
}

// Reparses `new_text` unless it is what the browser already shows. With
// `follow_tail`, selection and scroll position survive the reparse and a
// reader at the end stays at the end.
static void applyMarkdownText(QTextBrowser* browser, const QString& new_text, bool follow_tail) {
  configureMarkdownBrowser(browser);
  const QVariant last_value = browser->property(kMarkdownLastTextProperty);
  const QString last = last_value.toString();
  if (last_value.isValid() && last == new_text) {
    return;
  }
  if (!follow_tail) {
    browser->document()->setMarkdown(new_text, markdownFeatures());
    browser->setProperty(kMarkdownLastTextProperty, new_text);
    return;
  }

  QScrollBar* sb = browser->verticalScrollBar();
  const bool tail_rewrite = last_value.isValid() && isMarkdownTailRewrite(last, new_text);
  const bool at_end = sb->value() >= sb->maximum();
  const QTextCursor saved_cursor = browser->textCursor();
  const int saved_anchor = saved_cursor.anchor();
  const int saved_position = saved_cursor.position();
  const QTextCursor top_cursor = browser->cursorForPosition(QPoint(0, 0));
  const int top_position = top_cursor.position();
  const int top_offset = browser->cursorRect(top_cursor).top();
  const int old_scroll = sb->value();

  browser->document()->setMarkdown(new_text, markdownFeatures());
  browser->setProperty(kMarkdownLastTextProperty, new_text);

  if (tail_rewrite) {
    const int document_end = std::max(0, browser->document()->characterCount() - 1);
    QTextCursor restored(browser->document());
    restored.setPosition(std::clamp(saved_anchor, 0, document_end));
    restored.setPosition(std::clamp(saved_position, 0, document_end), QTextCursor::KeepAnchor);
    browser->setTextCursor(restored);
    if (at_end) {
      sb->setValue(sb->maximum());
    } else {
      QTextCursor restored_top(browser->document());
      restored_top.setPosition(std::clamp(top_position, 0, document_end));
      sb->setValue(std::clamp(old_scroll, 0, sb->maximum()));
      sb->setValue(std::clamp(sb->value() + browser->cursorRect(restored_top).top() - top_offset, 0, sb->maximum()));
    }
  } else {
    browser->moveCursor(QTextCursor::End);
    sb->setValue(sb->maximum());
  }

  ensureTailFollowOverlay(browser);
}

static void applyToWidget(
    QWidget* w, std::string_view name, const PJ::WidgetDataView& view, PJ::AppSession* session = nullptr,
    PJ::CatalogModel* catalog = nullptr) {
  const QSignalBlocker blocker(w);

  // --- Generic properties (any widget) ---
  // For a widget whose plain control was swapped for a styled replacement (see
  // widget_adapters), `enabled` is written straight to the hidden original and
  // reaches the replacement via syncStyledWidget below. `visible` is redirected
  // onto the original as a desired-visible the replacement derives from (e.g. a
  // DualOptionsWidget's visibility is the OR of its hidden radios), so it
  // goes through redirectAdaptedVisibility; an un-adapted widget just sets its
  // own visibility.
  if (auto v = view.enabled(name)) {
    w->setEnabled(*v);
  }
  if (auto v = view.visible(name)) {
    if (!redirectAdaptedVisibility(w, *v)) {
      w->setVisible(*v);
    }
  }

  // --- Generic field-validity indicator (any widget) ---
  // The plugin owns the validation rule and pushes {valid, tooltip}; the host
  // renders a soft cue (the tooltip plus an error background on the field
  // itself when invalid) without needing a per-field indicator widget. The cue
  // is scoped by objectName so child widgets are unaffected; cleared when valid.
  // The invalid cue is a Highlight (magenta) outline, not a fill: the field keeps
  // its nominal background and only its border recolours, so an empty/invalid
  // input reads as "needs attention" without the jarring red wash.
  if (auto ok = view.fieldValid(name)) {
    if (auto tip = view.fieldValidTooltip(name)) {
      w->setToolTip(QString::fromStdString(*tip));
    }
    const QString sel = w->objectName().isEmpty() ? QString() : QStringLiteral("#%1").arg(w->objectName());
    if (*ok || sel.isEmpty()) {
      w->setStyleSheet(QString());
    } else {
      const auto fw_theme = theme::appTheme();
      w->setStyleSheet(
          sel + QStringLiteral(" { border: 1px solid %1; }")
                    .arg(
                        theme::interaction(theme::Variant::Highlight, theme::State::Nominal, fw_theme)
                            .name(QColor::HexArgb)));
    }
  }

  // --- QLineEdit ---
  if (auto* le = qobject_cast<QLineEdit*>(w)) {
    if (auto v = view.text(name)) {
      // Guard against re-setting identical text: an editable field that refreshes
      // on its own onTextChanged would otherwise move the caret to the end mid-typing.
      const QString t = QString::fromStdString(*v);
      if (le->text() != t) {
        le->setText(t);
      }
    }
    if (auto v = view.placeholder(name)) {
      le->setPlaceholderText(QString::fromStdString(*v));
    }
    if (auto v = view.readOnly(name)) {
      le->setReadOnly(*v);
    }
    return;
  }

  // --- QTextBrowser promoted from a pjMarkdown QPlainTextEdit ---
  if (auto* browser = qobject_cast<QTextBrowser*>(w);
      browser != nullptr && browser->property(kMarkdownProperty).toBool()) {
    if (auto text = view.plainText(name)) {
      applyMarkdownText(browser, QString::fromStdString(*text), browser->property(kFollowTailProperty).toBool());
    }
    return;
  }

  // --- QPlainTextEdit ---
  if (auto* pte = qobject_cast<QPlainTextEdit*>(w)) {
    if (auto code = view.codeContent(name)) {
      // Code editors use a light-theme syntax highlighter (dark-on-white token
      // colors), so the text area must stay white regardless of the app's
      // light/dark theme. Tag the widget so the global stylesheet paints it
      // white; set once + re-polish so the attribute selector re-evaluates.
      if (!pte->property("_pj_code_editor").toBool()) {
        pte->setProperty("_pj_code_editor", true);
        pte->style()->unpolish(pte);
        pte->style()->polish(pte);
      }
      // Also tag the editor's immediate container pane so the area around the
      // editor (assist dropdowns, labels) shares the white code surface in the
      // light theme. WA_StyledBackground lets the QSS background paint on a
      // plain QWidget; the dark theme leaves the pane at its normal background.
      if (auto* pane = pte->parentWidget(); pane != nullptr && !pane->property("_pj_code_editor_pane").toBool()) {
        pane->setProperty("_pj_code_editor_pane", true);
        pane->setAttribute(Qt::WA_StyledBackground, true);
        pane->style()->unpolish(pane);
        pane->style()->polish(pane);
      }
      // Code editor mode: only update if content actually differs (preserve cursor).
      QString new_text = QString::fromStdString(*code);
      if (pte->toPlainText() != new_text) {
        pte->setPlainText(new_text);
      }
      // Install or swap syntax highlighter when the language changes.
      if (auto lang = view.codeLanguage(name)) {
        QString current = pte->property("_pj_code_lang").toString();
        QString requested = QString::fromStdString(*lang);
        if (current != requested) {
          pte->setProperty("_pj_code_lang", requested);
          if (auto* old = pte->document()->findChild<QSyntaxHighlighter*>()) {
            delete old;
          }
          if (*lang == "lua") {
            new PJ::LuaSyntaxHighlighter(pte->document());
          } else if (*lang == "python") {
            new PJ::PythonSyntaxHighlighter(pte->document());
          }
        }
      }
      // Place the caret where the plugin asked (e.g. just past an inserted
      // completion), now that the new text is in place.
      if (auto cur = view.codeCursor(name)) {
        QTextCursor tc = pte->textCursor();
        tc.setPosition(qBound(0, *cur, static_cast<int>(pte->toPlainText().size())));
        pte->setTextCursor(tc);
      }
    } else if (auto pt = view.plainText(name)) {
      const QString new_text = QString::fromStdString(*pt);
      if (pte->property(kFollowTailProperty).toBool()) {
        applyFollowTailText(pte, new_text);
      } else {
        // Transcript/log semantics: setPlainText resets the viewport, which
        // yanks a reader who was following the newest lines. On an UPDATE of
        // an already-populated widget, stick to the bottom when the reader was
        // at the end, else keep their place (clamped). First population keeps
        // Qt's default (top), so document-style plain text opens at the start.
        const bool had_content = !pte->document()->isEmpty();
        QScrollBar* sb = pte->verticalScrollBar();
        const bool at_bottom = sb->value() >= sb->maximum();
        const int old_value = sb->value();
        pte->setPlainText(new_text);
        if (had_content) {
          sb->setValue(at_bottom ? sb->maximum() : std::min(old_value, sb->maximum()));
        }
      }
    }
    if (auto v = view.readOnly(name)) {
      pte->setReadOnly(*v);
    }
    // Opt-in caret tracking: when set, connectWidgetSignals also wires
    // cursorPositionChanged so the plugin sees caret moves, not just edits.
    if (auto track = view.codeCaretTracking(name)) {
      pte->setProperty("_pj_caret_tracking", *track);
    }
    return;
  }

  // --- QComboBox ---
  if (auto* cb = qobject_cast<QComboBox*>(w)) {
    if (auto v = view.items(name)) {
      // Rebuild only when the item set actually changed. For an editable combo
      // (where text + items share one diff key) an unconditional clear()+add
      // would wipe the line edit and bounce the caret to the end on every
      // keystroke tick. Comparing first keeps mid-text editing stable.
      QStringList incoming;
      incoming.reserve(static_cast<qsizetype>(v->size()));
      for (const auto& item : *v) {
        incoming << QString::fromStdString(item);
      }
      QStringList current;
      current.reserve(cb->count());
      for (int i = 0; i < cb->count(); ++i) {
        current << cb->itemText(i);
      }
      if (current != incoming) {
        const QString saved = cb->isEditable() ? cb->currentText() : QString();
        cb->clear();
        cb->addItems(incoming);
        if (cb->isEditable()) {
          cb->setCurrentText(saved);
        }
      }
    }
    if (auto v = view.currentIndex(name)) {
      cb->setCurrentIndex(*v);
    }
    // Editable combos carry free text (e.g. a server URI). Reflect the
    // plugin's text() back into the line edit, guarded so a value identical
    // to what the user just typed doesn't reset the cursor mid-edit.
    if (cb->isEditable()) {
      if (auto v = view.text(name)) {
        const QString t = QString::fromStdString(*v);
        if (cb->currentText() != t) {
          cb->setCurrentText(t);
        }
      }
    }
    return;
  }

  // --- QCheckBox ---
  if (auto* ck = qobject_cast<QCheckBox*>(w)) {
    if (auto v = view.checked(name)) {
      ck->setChecked(*v);
    }
    if (auto v = view.text(name)) {
      ck->setText(QString::fromStdString(*v));
    }
    // Keep any styled replacement in sync, and adapt now if this checkbox just
    // became adaptable (e.g. its text arrived via data). Both no-op otherwise.
    syncStyledWidget(ck);
    tryAdaptStyledWidget(ck);
    return;
  }

  // --- ToggleSwitch (iOS-style toggle; QWidget, not QCheckBox) ---
  if (auto* ts = qobject_cast<ToggleSwitch*>(w)) {
    if (auto v = view.checked(name)) {
      ts->setChecked(*v, /*animate=*/false);  // no animation on programmatic sync
    }
    return;
  }

  // --- QRadioButton ---
  if (auto* rb = qobject_cast<QRadioButton*>(w)) {
    if (auto v = view.checked(name)) {
      rb->setChecked(*v);
    }
    // Keep any styled replacement in sync, and adapt the group now if it has
    // just become adaptable (e.g. data selected one option of a previously
    // unselected group). Both no-op for un-adapted/non-adaptable widgets.
    syncStyledWidget(rb);
    tryAdaptStyledWidget(rb);
    return;
  }

  // --- QSpinBox ---
  if (auto* sb = qobject_cast<QSpinBox*>(w)) {
    if (auto v = view.rangeMin(name)) {
      sb->setMinimum(*v);
    }
    if (auto v = view.rangeMax(name)) {
      sb->setMaximum(*v);
    }
    if (auto v = view.valueInt(name)) {
      sb->setValue(*v);
    }
    return;
  }

  // --- QDoubleSpinBox ---
  if (auto* dsb = qobject_cast<QDoubleSpinBox*>(w)) {
    if (auto v = view.valueDouble(name)) {
      dsb->setValue(*v);
    }
    return;
  }

  // --- QListWidget ---
  if (auto* lw = qobject_cast<QListWidget*>(w)) {
    // Per-row delete affordance (setListItemsDeletable). The property drives the
    // ListRowDeleteDelegate installed in connectWidgetSignals; toggling it after
    // rows exist needs a relayout so sizeHint (which reserves the icon width) is
    // re-queried.
    const bool deletable = view.listDeletable(name).value_or(false);
    if (lw->property("pj_deletable").toBool() != deletable) {
      lw->setProperty("pj_deletable", deletable);
      lw->doItemsLayout();
    }
    if (auto v = view.listItems(name)) {
      lw->clear();
      for (std::size_t i = 0; i < v->size(); ++i) {
        auto* item = new QListWidgetItem(QString::fromStdString((*v)[i]));
        // Delivered index, so itemDoubleClicked can report the plugin's index
        // even when list sorting re-orders the view (see listItemPluginIndex).
        item->setData(kPluginRowRole, static_cast<int>(i));
        lw->addItem(item);
      }
    }
    if (auto v = view.listMultiSelect(name)) {
      // MultiSelection: each click toggles that row, so several rows can be
      // picked with plain clicks (no modifier) — the intuitive multi-select.
      lw->setSelectionMode(*v ? QAbstractItemView::MultiSelection : QAbstractItemView::SingleSelection);
    }
    if (auto v = view.selectedItems(name)) {
      std::set<std::string> selected(v->begin(), v->end());
      for (int i = 0; i < lw->count(); ++i) {
        auto* item = lw->item(i);
        item->setSelected(selected.count(item->text().toStdString()) > 0);
      }
    }
    // Grey out (disable + un-selectable) items whose text is in the disabled set,
    // keeping them visible; re-enable the rest. Runs after items/selection so it
    // tracks the latest list contents.
    if (auto v = view.listDisabledItems(name)) {
      std::set<QString> disabled;
      for (const std::string& entry : *v) {
        disabled.insert(QString::fromStdString(entry));
      }
      for (int i = 0; i < lw->count(); ++i) {
        auto* item = lw->item(i);
        setItemEnabled(item, disabled.count(item->text()) == 0);
      }
    }
    // Empty-state overlay: a centered hint floating over the list viewport while
    // it has no rows, hidden the moment items appear (mirrors the chart
    // placeholder). Parented to the viewport so it tracks the list's content area.
    if (auto ph = view.listPlaceholder(name)) {
      auto* overlay = lw->viewport()->findChild<ChartPlaceholderOverlay*>(QString(), Qt::FindDirectChildrenOnly);
      if (overlay == nullptr) {
        overlay = new ChartPlaceholderOverlay(lw->viewport());
      }
      overlay->setText(QString::fromStdString(*ph));
      overlay->setVisible(lw->count() == 0);
      overlay->recenter();
      // Re-center after the current layout settles: on initial injection the
      // viewport may still grow to its final height afterwards, and no later
      // resize event fires if the surrounding dialog was already sized — which
      // left the hint stuck low instead of centered.
      QTimer::singleShot(0, overlay, [overlay]() { overlay->recenter(); });
    } else if (
        auto* overlay = lw->viewport()->findChild<ChartPlaceholderOverlay*>(QString(), Qt::FindDirectChildrenOnly)) {
      // A payload that updates items WITHOUT re-sending list_placeholder must
      // still recompute the overlay — the SDK contract is auto-hide the moment
      // data appears, not "hide only when the plugin repeats the key".
      overlay->setVisible(lw->count() == 0);
      overlay->recenter();
    }
    return;
  }

  // --- QTableWidget ---
  if (auto* tw = qobject_cast<QTableWidget*>(w)) {
    // Per-row delete affordance (setListItemsDeletable), shared with QListWidget.
    // The property drives the ListRowDeleteDelegate installed in
    // connectWidgetSignals; relayout when it changes so the delegate's sizeHint
    // is re-queried for existing rows.
    const bool deletable = view.listDeletable(name).value_or(false);
    if (tw->property("pj_deletable").toBool() != deletable) {
      tw->setProperty("pj_deletable", deletable);
      tw->doItemsLayout();
    }
    // The dialog protocol carries no cell-edit event (only selection, double-click
    // and radio), so an edited cell can never be read back by the plugin — an
    // editable cell silently discards the edit on accept. Force read-only on every
    // protocol table so no picker looks editable when it isn't. Persistent and
    // idempotent, so setting it on each delivery is free. A future editable table
    // would need a new protocol event anyway, and would opt out here then.
    tw->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // The radio column is read up front: recordPluginKeyColumn needs to know
    // which column carries radio widgets (no item text) to pick the key column,
    // and the header sizing stretches the content column just after it.
    const std::optional<int> radio_col = view.tableRadioColumn(name);
    if (auto v = view.tableHeaders(name)) {
      QStringList hdr;
      for (const auto& h : *v) {
        hdr << QString::fromStdString(h);
      }
      // Re-setting labels reconfigures the header (not free), so only do it when
      // they actually changed.
      if (!tableMatchesHeaders(tw, hdr)) {
        tw->setColumnCount(static_cast<int>(hdr.size()));
        tw->setHorizontalHeaderLabels(hdr);
      }
    }
    // Header sizing runs on EVERY delivery, not only when the header SET changes:
    // a leading radio marker means the next column is the series name that should
    // absorb the width, and that stretch must (re)apply once the radio column is
    // known even if the headers themselves were stable. Idempotent; the one-time
    // HeaderResizePolicy install inside stays guarded.
    installTreeLikeHeader(tw, radio_col.has_value() ? *radio_col + 1 : -1);
    bool rows_replaced = false;
    if (auto v = view.tableRows(name)) {
      applyTableRows(tw, *v, view.tableColumnValues(name));
      recordPluginKeyColumn(tw, radio_col.value_or(-1));
      // A full-rows resync resets the delta gate: a producer whose seq counter
      // restarted must not have its first post-resync delta swallowed by a
      // stale recorded seq.
      tw->setProperty("_pj_table_delta_seq", QVariant());
      rows_replaced = true;
    }
    // Grow the data columns to fit their content the first time real rows land.
    // installTreeLikeHeader runs before applyTableRows, so its seed widths only
    // saw the header labels — MCAP's "Schema" column opened clipped to the word
    // "Schema" even though its cells read "std_msgs/Float64". Grow-only (never
    // below the seeded default) and capped, applied once via the policy's
    // non-redistributing setter so it never fights a later user drag.
    if (rows_replaced && tw->rowCount() > 0 && tw->property("pjTreeLikeHeader").toBool() &&
        !tw->horizontalHeader()->isHidden() && !tw->property("pjDataColsContentFit").toBool()) {
      tw->setProperty("pjDataColsContentFit", true);
      auto* header = tw->horizontalHeader();
      auto* policy = PJ::HeaderResizePolicy::install(header, kNameColumn, 20);
      constexpr int kDataColumnMaxWidth = 320;
      // Cell text width + left/right item padding; setSectionWidth still raises to
      // the header label's own minimum, so a short-content column never shrinks
      // below its title (QTableView::sizeHintForColumn is protected — measure
      // directly, which also avoids QHeaderView::resizeSection's drag rebalance).
      constexpr int kCellPadding = 16;
      const QFontMetrics metrics(tw->font());
      for (int i = kNameColumn + 1; i < header->count(); ++i) {
        int content = 0;
        for (int r = 0; r < tw->rowCount(); ++r) {
          if (const QTableWidgetItem* item = tw->item(r, i)) {
            content = std::max(content, metrics.horizontalAdvance(item->text()));
          }
        }
        const int want = std::clamp(content + kCellPadding, header->sectionSize(i), kDataColumnMaxWidth);
        policy->setSectionWidth(i, want);
      }
    }
    // Batch deltas, seq-gated per widget: apply only when the seq differs from
    // the last one applied here; a delivery that also carried a full `rows`
    // replace consumes the delta without applying it (rows wins). A delta that
    // fails to decode or to resolve against the table consumes nothing, so a
    // corrected retransmission of the same seq still applies.
    if (auto delta_seq = view.tableDeltaSeq(name)) {
      const QVariant last_seq = tw->property("_pj_table_delta_seq");
      if (!last_seq.isValid() || last_seq.toULongLong() != *delta_seq) {
        if (rows_replaced) {
          tw->setProperty("_pj_table_delta_seq", QVariant::fromValue<qulonglong>(*delta_seq));
        } else if (auto delta = view.tableDelta(name)) {
          if (applyTableDelta(tw, *delta)) {
            tw->setProperty("_pj_table_delta_seq", QVariant::fromValue<qulonglong>(*delta_seq));
          }
        }
      }
    }
    // Sort arrow for a table the PLUGIN sorts (it re-emits rows already ordered and
    // leaves Qt's own sortingEnabled off, so Qt would never paint an arrow itself).
    // Cosmetic only: the header's sortIndicatorChanged is what a sorting-enabled
    // QTableView turns into a real sortByColumn, so it stays blocked here — the
    // plugin's row order is the truth and must never be re-sorted out from under it.
    if (const auto indicator = view.tableSortIndicator(name)) {
      auto* header = tw->horizontalHeader();
      const Qt::SortOrder order = indicator->second ? Qt::AscendingOrder : Qt::DescendingOrder;
      // The delivered state is remembered on the header: Qt's own click handling
      // flips the visible arrow BEFORE emitting sectionClicked, so the
      // header-click wiring re-asserts these after a click the plugin ignored
      // (see connectWidgetSignals) — without them the arrow would lie until the
      // next re-delivery.
      header->setProperty("pjSortIndicatorCol", indicator->first);
      header->setProperty("pjSortIndicatorAsc", indicator->second);
      // Skip-if-unchanged, like every other header aspect: the indicator rides
      // along in EVERY widget-data delivery (streamed ticks included), and Qt
      // repaints — with ResizeToContents, re-measures — the section even when
      // nothing moved.
      if (!header->isSortIndicatorShown() || header->sortIndicatorSection() != indicator->first ||
          header->sortIndicatorOrder() != order) {
        const QSignalBlocker header_blocker(header);
        header->setSortIndicatorShown(true);
        header->setSortIndicator(indicator->first, order);
      }
    }
    // Every index-keyed aspect below arrives in plugin row order; translate it
    // to the current (possibly user-sorted) view order. The maps reflect the
    // rows just applied above and are only built when this delivery actually
    // carries an index-keyed aspect — a rows-only streaming tick skips them.
    const auto visible_rows = view.visibleRows(name);
    const auto disabled_rows = view.disabledRows(name);
    const auto selected_rows = view.selectedRows(name);
    const auto cell_tooltips = view.cellTooltips(name);
    std::vector<int> view_to_plugin;
    std::vector<int> plugin_to_view;
    if (radio_col || visible_rows || disabled_rows || selected_rows || cell_tooltips) {
      view_to_plugin = viewToPluginRowMap(tw);
      plugin_to_view = invertRowMap(view_to_plugin);
    }
    // Radio column: render the designated column as an exclusive radio group and
    // sync the checked row. Build the radios UNCONDITIONALLY — a Modify flow delivers
    // pre-populated rows on the very first apply, which runs BEFORE connectWidgetSignals
    // stashes the holder; gating on the holder there left the radio column empty and
    // its width mis-stretched (unlike Create, whose rows arrive by drop after wiring).
    // The click callback resolves the holder lazily, so clicks still emit once it lands.
    if (radio_col) {
      const int checked_plugin_row = view.tableRadioCheckedRow(name).value_or(-1);
      const int checked_view_row =
          checked_plugin_row >= 0 && static_cast<std::size_t>(checked_plugin_row) < plugin_to_view.size()
              ? plugin_to_view[static_cast<std::size_t>(checked_plugin_row)]
              : -1;
      applyTableRadioColumn(tw, *radio_col, checked_view_row, [tw](int row) {
        if (auto* holder = static_cast<RadioEmitHolder*>(
                tw->findChild<QObject*>(u"pj_radio_emit_holder"_s, Qt::FindDirectChildrenOnly))) {
          holder->emit_row(row);
        }
      });
    }
    // Row visibility (live filtering): hide rows not in the visible set. Absent
    // (clearVisibleRows ⇒ nullopt) means "no change"; an empty set hides all.
    if (visible_rows) {
      std::set<int> visible(visible_rows->begin(), visible_rows->end());
      for (int r = 0; r < tw->rowCount(); ++r) {
        tw->setRowHidden(r, !visible.contains(view_to_plugin[static_cast<std::size_t>(r)]));
      }
    }
    if (disabled_rows) {
      std::set<int> disabled(disabled_rows->begin(), disabled_rows->end());
      for (int r = 0; r < tw->rowCount(); ++r) {
        const bool enabled = disabled.count(view_to_plugin[static_cast<std::size_t>(r)]) == 0;
        for (int c = 0; c < tw->columnCount(); ++c) {
          if (auto* item = tw->item(r, c)) {
            setItemEnabled(item, enabled);
          }
        }
      }
    }
    // Cell tooltips arrive as (plugin row, col, text); translate the row to the
    // current view order and set the tooltip on the item. A delivery that carries
    // cell_tooltips states the complete set, so clear every item tooltip first —
    // otherwise a tooltip the plugin dropped would linger on a stale cell after
    // an in-place row rewrite.
    if (cell_tooltips) {
      for (int r = 0; r < tw->rowCount(); ++r) {
        for (int c = 0; c < tw->columnCount(); ++c) {
          if (auto* item = tw->item(r, c)) {
            item->setToolTip(QString());
          }
        }
      }
      for (const auto& [plugin_row, col, tip] : *cell_tooltips) {
        if (plugin_row < 0 || static_cast<std::size_t>(plugin_row) >= plugin_to_view.size()) {
          continue;
        }
        const int view_row = plugin_to_view[static_cast<std::size_t>(plugin_row)];
        if (auto* item = tw->item(view_row, col)) {
          item->setToolTip(QString::fromStdString(tip));
        }
      }
    }
    if (selected_rows) {
      // Re-applying the selection via selectRow() scrolls the view to the last
      // selected row, so the table "jumps" on every re-render that follows a user
      // selection change (the common case, where the selection is ALREADY what we
      // want). Skip when it already matches; otherwise preserve the scroll position
      // across the change so a programmatic update (deselect-all, filter) does not
      // yank the viewport either.
      // Both sides of the comparison live in plugin row space, so a selection
      // that already matches is recognized even under a user-sorted view.
      std::set<int> want(selected_rows->begin(), selected_rows->end());
      std::set<int> have;
      for (const QModelIndex& idx : tw->selectionModel()->selectedRows()) {
        have.insert(view_to_plugin[static_cast<std::size_t>(idx.row())]);
      }
      if (want != have) {
        QScrollBar* vbar = tw->verticalScrollBar();
        const int scroll = vbar != nullptr ? vbar->value() : 0;
        tw->clearSelection();
        for (int r : *selected_rows) {
          if (r >= 0 && static_cast<std::size_t>(r) < plugin_to_view.size()) {
            tw->selectRow(plugin_to_view[static_cast<std::size_t>(r)]);
          }
        }
        if (vbar != nullptr) {
          vbar->setValue(scroll);
        }
      }
    }
    if (auto v = view.selectedItems(name)) {
      // Text-keyed selection restore (setSelectedItems): match each row by
      // tableRowKeyText — the same key the selection-changed emit uses — so the
      // restore is sort-agnostic (row indices desync under sortingEnabled) and
      // works for tables with a leading radio/widget column. Applied second so
      // it wins over selected_rows if a plugin ever sent both. Same
      // skip-if-unchanged + scroll-preservation rationale as the index path.
      std::set<std::string> want_texts(v->begin(), v->end());
      std::vector<int> want_rows;
      for (int r = 0; r < tw->rowCount(); ++r) {
        if (auto key = tableRowKeyText(tw, r); key && want_texts.count(*key) > 0) {
          want_rows.push_back(r);
        }
      }
      std::set<int> want(want_rows.begin(), want_rows.end());
      std::set<int> have;
      for (const QModelIndex& idx : tw->selectionModel()->selectedRows()) {
        have.insert(idx.row());
      }
      if (want != have) {
        QScrollBar* vbar = tw->verticalScrollBar();
        const int scroll = vbar != nullptr ? vbar->value() : 0;
        tw->clearSelection();
        for (int r : want_rows) {
          tw->selectRow(r);
        }
        if (vbar != nullptr) {
          vbar->setValue(scroll);
        }
      }
    }
    // Empty-state overlay: a centered hint over the table viewport while it has no
    // rows, hidden the moment rows appear (mirrors the QListWidget placeholder).
    // Parented to the viewport so it tracks the table's content area.
    if (auto ph = view.listPlaceholder(name)) {
      auto* overlay = tw->viewport()->findChild<ChartPlaceholderOverlay*>(QString(), Qt::FindDirectChildrenOnly);
      if (overlay == nullptr) {
        overlay = new ChartPlaceholderOverlay(tw->viewport());
      }
      overlay->setText(QString::fromStdString(*ph));
      overlay->setVisible(tw->rowCount() == 0);
      overlay->recenter();
      QTimer::singleShot(0, overlay, [overlay]() { overlay->recenter(); });
    }
    return;
  }

  // --- QLabel ---
  if (auto* lbl = qobject_cast<QLabel*>(w)) {
    if (auto v = view.label(name)) {
      lbl->setText(QString::fromStdString(*v));
    }
    // Also allow "text" for labels
    if (auto v = view.text(name)) {
      lbl->setText(QString::fromStdString(*v));
    }
    return;
  }

  // --- QPushButton ---
  if (auto* btn = qobject_cast<QPushButton*>(w)) {
    if (auto v = view.buttonText(name)) {
      btn->setText(QString::fromStdString(*v));
    }
    if (auto svg = view.buttonIconSvg(name)) {
      QByteArray svg_data = QByteArray::fromStdString(*svg);
      QSvgRenderer renderer(svg_data);
      if (renderer.isValid()) {
        int sz = btn->iconSize().height() > 0 ? btn->iconSize().height() : 16;
        QPixmap pix(sz, sz);
        pix.fill(Qt::transparent);
        QPainter painter(&pix);
        renderer.render(&painter);
        btn->setIcon(QIcon(pix));
      }
    }
    // Named icons: the plugin sends a semantic id (setButtonIconNamed); the
    // host resolves it from its themed icon set. Unknown ids leave the button
    // icon untouched.
    if (auto icon_name = view.buttonIconName(name)) {
      const QString path = resolveNamedIconPath(*icon_name);
      if (!path.isEmpty()) {
        btn->setIcon(QIcon(loadSvg(path, currentTheme())));
      }
    }
    return;
  }

  // --- QTabWidget ---
  if (auto* tw = qobject_cast<QTabWidget*>(w)) {
    // Stretch the tabs across the full bar width. Load-bearing for panels that
    // set documentMode in their .ui — the only mode in which the tab bar gets
    // the full pane width — because QTabWidget::setDocumentMode() resets
    // QTabBar::expanding to false during the .ui load. Harmless for
    // non-document tab widgets (their bar stays at sizeHint, where expanding
    // has nothing to distribute).
    tw->tabBar()->setExpanding(true);
    if (tw->documentMode()) {
      // Document-mode bars paint a base line across the non-selected tabs
      // (PE_FrameTabBarBase); the app's flat tab styling has no pane frame
      // for it to connect to, so it reads as a stray line. Drop it.
      tw->tabBar()->setDrawBase(false);
    }
    if (auto v = view.tabIndex(name)) {
      tw->setCurrentIndex(*v);
    }
    return;
  }

  // --- QDialogButtonBox ---
  if (auto* dbb = qobject_cast<QDialogButtonBox*>(w)) {
    if (auto v = view.okEnabled(name)) {
      if (auto* ok = dbb->button(QDialogButtonBox::Ok)) {
        ok->setEnabled(*v);
      }
    }
    return;
  }

  // --- RangeSlider (two-handle range slider) ---
  if (auto* rs = qobject_cast<RangeSlider*>(w)) {
    // Bounds first — setMinimum/setMaximum reset the handle values, so values
    // (sent in the same tick) must be applied afterwards. Unchanged bounds are
    // skipped: the whole rangeSlider entry rides every diff (values, bounds,
    // span, markers share one key), so during a drag the same bounds arrive on
    // every tick — and a diff that carries no values (e.g. only markers moved)
    // must not slam the handles to the extremes.
    if (auto v = view.rangeSliderMin(name); v && *v != rs->getMinimun()) {
      rs->setMinimum(*v);
    }
    if (auto v = view.rangeSliderMax(name); v && *v != rs->getMaximun()) {
      rs->setMaximum(*v);
    }
    if (auto v = view.rangeSliderLower(name)) {
      rs->setLowerValue(*v);
    }
    if (auto v = view.rangeSliderUpper(name)) {
      rs->setUpperValue(*v);
    }
    // Time labels: when a time span is provided, float each handle's offset-from-start
    // ABOVE it (always shown) and show the selected duration as a chip ON the track.
    // The track keeps the playback scrubber's 24px height; the labels add ONE row above
    // (minimumSizeHint), with no wasted reserve below — grow the widget to fit it.
    if (auto span = view.rangeSliderTimeSpan(name)) {
      const std::int64_t min_ns = span->first;
      const std::int64_t max_ns = span->second;
      if (max_ns > min_ns) {
        const int slider_max = rs->getMaximun();
        rs->setShowHandleValueTooltip(false);
        rs->setFloatingLabelsVisible(true);
        rs->setMinimumHeight(rs->minimumSizeHint().height());
        rs->setLabelFormatter([min_ns, max_ns, slider_max](double pos) -> QString {
          std::int64_t ns = sliderToNs(static_cast<int>(pos), slider_max, min_ns, max_ns);
          return QString::fromStdString(formatDuration(ns - min_ns));
        });
        rs->setCenterLabelFormatter([min_ns, max_ns, slider_max](double lo, double hi) -> QString {
          std::int64_t lo_ns = sliderToNs(static_cast<int>(lo), slider_max, min_ns, max_ns);
          std::int64_t hi_ns = sliderToNs(static_cast<int>(hi), slider_max, min_ns, max_ns);
          return QString::fromStdString(formatDuration(hi_ns - lo_ns));
        });
        rs->update();
      }
    }
    // Boundary markers (chunk lines + labels + in-range shading). nullopt = no
    // change; an explicit empty list clears them.
    if (auto markers = view.rangeSliderMarkers(name)) {
      std::vector<RangeSlider::Marker> out;
      out.reserve(markers->size());
      for (const auto& m : *markers) {
        out.push_back({m.start, m.end, QString::fromStdString(m.label)});
      }
      rs->setMarkers(std::move(out));
    }
    return;
  }

  // --- MarkerTimeline (editable multi-marker strip) ---
  if (auto* mt = qobject_cast<MarkerTimeline*>(w)) {
    // Bounds before marks: setRange clamps existing marks into the new domain.
    if (auto mn = view.markerTimelineMin(name)) {
      if (auto mx = view.markerTimelineMax(name)) {
        mt->setRange(*mn, *mx);
      }
    }
    if (auto span = view.markerTimelineTimeSpan(name)) {
      mt->setTimeSpan(span->first, span->second);
    }
    if (auto marks = view.markerTimelineMarks(name)) {
      QVector<MarkerTimeline::Mark> out;
      out.reserve(static_cast<int>(marks->size()));
      for (const TimelineMark& m : *marks) {
        out.push_back({m.id, m.region ? MarkerTimeline::Kind::kRegion : MarkerTimeline::Kind::kEvent, m.start, m.end});
      }
      // setMarks() is silent (owner-driven), so this does not echo back as an edit.
      mt->setMarks(out);
    }
    return;
  }

  // --- QDateTimeEdit (ISO-8601 value + allowed range) ---
  if (auto* dte = qobject_cast<QDateTimeEdit*>(w)) {
    // Range first: Qt clamps the value against the range in force when it lands.
    if (auto range = view.dateTimeRange(name)) {
      if (const QDateTime mn = QDateTime::fromString(QString::fromStdString(range->first), Qt::ISODate); mn.isValid()) {
        dte->setMinimumDateTime(mn);
      }
      if (const QDateTime mx = QDateTime::fromString(QString::fromStdString(range->second), Qt::ISODate);
          mx.isValid()) {
        dte->setMaximumDateTime(mx);
      }
    }
    if (auto iso = view.dateTime(name)) {
      if (const QDateTime dt = QDateTime::fromString(QString::fromStdString(*iso), Qt::ISODate); dt.isValid()) {
        dte->setDateTime(dt);
      }
    }
    return;
  }

  // --- DateRangePicker (date/time range placeholder hints) ---
  if (auto* drp = qobject_cast<DateRangePicker*>(w)) {
    if (auto iso = view.dateRangeEarliest(name)) {
      drp->setEarliestDate(QDate::fromString(QString::fromStdString(*iso), Qt::ISODate));
    }
    if (auto iso = view.dateRangeLatest(name)) {
      drp->setLatestDate(QDate::fromString(QString::fromStdString(*iso), Qt::ISODate));
    }
    return;
  }

  // --- QFrame with chart_series / chart_markers / chart_zoom_enabled → PlotWidget or ChartPreviewWidget ---
  if (auto* frame = qobject_cast<QFrame*>(w)) {
    auto series_data = view.chartSeries(name);
    auto markers_data = view.chartMarkers(name);
    auto zoom_enabled = view.chartZoomEnabled(name);
    auto auto_zoom = view.chartAutoZoom(name);
    auto chart_placeholder = view.chartPlaceholder(name);
    // chart_placeholder alone must be honored too — a plugin may send the hint
    // before (or without) any series/markers/zoom keys; a placeholder-only payload
    // constructs no chart widget (only the overlay below).
    if (series_data || markers_data || zoom_enabled || chart_placeholder) {
      // A frame carrying chart_markers renders through ChartPreviewWidget — the only
      // chart that draws marker overlays; otherwise prefer the full PlotWidget when
      // the session/catalog are available. The choice is sticky per frame so a tick
      // without markers never swaps an already-created chart's widget type.
      auto* existing_chart = frame->findChild<PJ::ChartPreviewWidget*>();
      const bool has_chart_data = series_data || markers_data || zoom_enabled;
      const bool use_plot_widget =
          frame->findChild<PJ::PlotWidget*>() != nullptr ||
          (existing_chart == nullptr && session != nullptr && catalog != nullptr && !markers_data);
      if (has_chart_data && use_plot_widget) {
        // Full PlotWidget — zoom/tracker/legend, matching FilterEditorPanel preview quality.
        // Right-click context menu disabled per Davide's comment ("embedded PlotWidget
        // should have the right click menu disabled").
        auto* plot = frame->findChild<PJ::PlotWidget*>();
        if (!plot) {
          auto* layout = frame->layout();
          if (!layout) {
            layout = new QVBoxLayout(frame);
            // Flush: the chart fills the whole frame so the darker panel
            // backdrop never shows around it. Breathing room comes from
            // padding INSIDE the chart (contentsMargins below), which the
            // plot paints in its own Data-surface background.
            layout->setContentsMargins(
                theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
                theme::space(theme::Space::None));
          }
          plot = new PJ::PlotWidget(&session->sessionManager(), catalog, frame);
          plot->setContextMenuEnabled(false);
          // PlotWidgetBase starts with the grid disabled; show it so embedded chart
          // previews match the native editor's gridded look.
          plot->setGridVisible(true);
          // Let the canvas fill to the plot's top edge instead of reserving
          // Qwt's top-axis-label margin.
          plot->setCanvasAlignedToScales(false);
          // QwtPlot lays its axes out inside contentsRect, so these margins are
          // Data-toned internal padding, not a hole onto the panel backdrop.
          if (auto* qwt = plot->findChild<QwtPlot*>()) {
            qwt->setContentsMargins(
                theme::space(theme::Space::Comfortable), theme::space(theme::Space::Comfortable),
                theme::space(theme::Space::Comfortable), theme::space(theme::Space::Comfortable));
          }
          layout->addWidget(plot);
        }
        if (series_data) {
          // Mirror FilterEditorPanel's update strategy:
          // - Rebuild curves only when the SET of labels changes (like preview_set_key_).
          // - Otherwise just update samples and replot — preserves the user's zoom.
          // - zoomOut only on rebuild (first paint or new series set).
          // The current label set is stored as a frame property so we can detect changes.
          QStringList new_labels;
          for (const auto& s : *series_data) {
            new_labels << QString::fromStdString(s.label);
          }
          const QString new_set_key = new_labels.join(QLatin1Char('|'));
          const QString old_set_key = frame->property("_chart_set_key").toString();
          const bool rebuilt = (new_set_key != old_set_key);

          if (rebuilt) {
            // Set changed: remove all curves and re-create them with empty samples.
            plot->removeAllCurves();
            for (const auto& s : *series_data) {
              QColor color;
              if (!s.color.empty()) {
                color = QColor(QString::fromStdString(s.color));
              }
              const QString label = QString::fromStdString(s.label);
              plot->PlotWidgetBase::addCurve(
                  label, new QwtPointSeriesData(), color.isValid() ? color : Qt::transparent, label);
            }
            frame->setProperty("_chart_set_key", new_set_key);
            // Reset user-zoom flag so autozoom kicks in for the new series set.
            frame->setProperty("_user_zoomed", false);
          }

          // Update samples on existing curves (no rebuild overhead).
          {
            const auto& curve_list = plot->curveList();
            auto curve_it = curve_list.begin();
            std::size_t i = 0;
            while (curve_it != curve_list.end() && i < series_data->size()) {
              const auto& s = (*series_data)[i];
              QVector<QPointF> pts;
              pts.reserve(static_cast<int>(s.points.size()));
              for (const auto& p : s.points) {
                pts.append(QPointF(p.first, p.second));
              }
              auto* pts_data = new QwtPointSeriesData();
              pts_data->setSamples(pts);
              curve_it->curve->setData(pts_data);
              ++curve_it;
              ++i;
            }
          }

          // Re-apply the app's grid state that the host pushed as a frame property.
          // Done on EVERY update so it survives a curve rebuild. Curve style/width are
          // deliberately NOT pushed: a plugin preview has no originating plot to mirror,
          // so it keeps the PlotWidget default style/width.
          if (frame->property("_pj_view_set").toBool()) {
            plot->setGridVisible(frame->property("_pj_view_grid").toBool());
          }

          // Per-series dashed pattern LAST: a dashed series is the faded "before"
          // ghost (matches FilterEditorPanel/native TransformEditorPanel). Applied
          // after the grid config so it wins over the base pen the curve was built with.
          {
            const auto& curve_list = plot->curveList();
            auto curve_it = curve_list.begin();
            std::size_t i = 0;
            while (curve_it != curve_list.end() && i < series_data->size()) {
              if (curve_it->curve != nullptr && (*series_data)[i].dashed) {
                QPen pen = curve_it->curve->pen();
                pen.setStyle(Qt::DashLine);
                curve_it->curve->setPen(pen);
              }
              ++curve_it;
              ++i;
            }
          }

          // Connect viewResized once to detect manual user zoom.
          if (!frame->property("_zoom_connected").toBool()) {
            QObject::connect(plot, &PJ::PlotWidgetBase::viewResized, frame, [frame](const QRectF& /*r*/) {
              frame->setProperty("_user_zoomed", true);
            });
            frame->setProperty("_zoom_connected", true);
          }

          // Replot, then auto-fit. If the plugin sent an explicit AutoZoom flag,
          // honor it (true => fit every update, like the editor's AutoZoom box;
          // false => keep the user's zoom, but still fit once on a new series set).
          // Otherwise fall back to "fit until the user manually zooms".
          plot->replot();
          bool do_zoom;
          if (auto_zoom.has_value()) {
            do_zoom = rebuilt || *auto_zoom;
          } else {
            do_zoom = !frame->property("_user_zoomed").toBool();
          }
          if (do_zoom) {
            plot->zoomOut(false);
          }
        }
      } else if (has_chart_data) {
        // ChartPreviewWidget: no session/catalog available, or the frame carries
        // chart_markers (the overlay only this chart renders). Guarded by
        // has_chart_data so a placeholder-only payload never constructs a chart.
        auto* chart = existing_chart;
        if (!chart) {
          auto* layout = frame->layout();
          if (!layout) {
            layout = new QVBoxLayout(frame);
            // Flush frame + Data-toned internal padding — same scheme as the
            // PlotWidget branch above.
            layout->setContentsMargins(
                theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
                theme::space(theme::Space::None));
          }
          chart = new PJ::ChartPreviewWidget(frame);
          chart->setContentsMargins(
              theme::space(theme::Space::Comfortable), theme::space(theme::Space::Comfortable),
              theme::space(theme::Space::Comfortable), theme::space(theme::Space::Comfortable));
          layout->addWidget(chart);
        }
        if (series_data) {
          std::vector<PJ::ChartPreviewWidget::Series> chart_series;
          chart_series.reserve(series_data->size());
          for (const auto& s : *series_data) {
            chart_series.push_back({s.label, s.points, s.color});
          }
          chart->setSeries(chart_series);
        }
        if (markers_data) {
          std::vector<PJ::ChartPreviewWidget::Marker> chart_markers;
          chart_markers.reserve(markers_data->size());
          for (const auto& m : *markers_data) {
            chart_markers.push_back({m.kind, m.x0, m.x1, m.y0, m.y1, m.has_value, m.color, m.label});
          }
          chart->setMarkers(chart_markers);
        }
        if (zoom_enabled) {
          chart->setZoomEnabled(*zoom_enabled);
        }
      }

      // Placeholder overlay: a centered translucent hint shown while the chart
      // has no series (e.g. a drop prompt). Created lazily as a child of the
      // frame; shown/hidden per current data.
      if (chart_placeholder) {
        const bool has_data = series_data && !series_data->empty();
        const std::string& placeholder_text = *chart_placeholder;
        auto* overlay = frame->findChild<ChartPlaceholderOverlay*>(QString(), Qt::FindDirectChildrenOnly);
        if (overlay == nullptr) {
          overlay = new ChartPlaceholderOverlay(frame);
        }
        overlay->setMessage(QString::fromStdString(placeholder_text));
        overlay->setVisible(!has_data && !placeholder_text.empty());
      } else if (series_data) {
        // Series delivered WITHOUT re-sending chart_placeholder: recompute the
        // existing overlay so it auto-hides over fresh data (and reappears if
        // the series went empty), per the SDK contract.
        if (auto* overlay = frame->findChild<ChartPlaceholderOverlay*>(QString(), Qt::FindDirectChildrenOnly)) {
          const bool has_data = !series_data->empty();
          overlay->setVisible(!has_data);
          if (!has_data) {
            overlay->recenter();
          }
        }
      }
    }
    return;
  }

  // Containers (QGroupBox, QWidget) — only generic properties applied above.
  // Warn about widget types that have data in the view but aren't handled.
  // Skip known container types that only use generic enabled/visible properties.
  // Plain QWidget is matched by EXACT type (not qobject_cast, which any widget
  // satisfies): a bare container pane taking generic show/hide is legitimate,
  // while an unhandled CUSTOM subclass still deserves the warning.
  if (!qobject_cast<QGroupBox*>(w) && !qobject_cast<QSplitter*>(w) && w->metaObject() != &QWidget::staticMetaObject) {
    qWarning(
        "WidgetBinding: unsupported widget type '%s' for '%s'; "
        "see dialog-plugin-guide.md for supported types",
        w->metaObject()->className(), std::string(name).c_str());
  }
}

void applyWidgetData(
    QWidget* root, const PJ::WidgetDataView& view, PJ::AppSession* session, PJ::CatalogModel* catalog) {
  const std::vector<std::string> names = view.widgetNames();
  for (const auto& name : names) {
    auto* w = findNamedIn<QWidget*>(root, QString::fromStdString(name));
    if (!w) {
      continue;
    }
    applyToWidget(w, name, view, session, catalog);
  }
  syncColumnSelectorTables(root, names);
  // Re-assert "pj_enable_when"/"pj_visible_when" rules: applyToWidget wraps
  // every apply in a QSignalBlocker, so a plugin-pushed combo index never
  // fires the connected rule — and a plugin-pushed `enabled`/`visible` on a
  // rule-governed widget would otherwise fight it. After every data apply,
  // the rules win, deterministically.
  refreshDeclarativeRules(root);
  // NOTE: styled-widget adaptation is NOT re-run here on every data tick. It is
  // structural (depends on the widget tree, built once at load), so the engines
  // call adaptStyledWidgets once after loading the .ui (see widget_adapters),
  // and applyToWidget adapts reactively per widget via tryAdaptStyledWidget for
  // controls that only become adaptable after their first data arrives.
}

// ---------------------------------------------------------------------------
// connect_widget_signals — wire Qt signals to WidgetEventBuilder output
// ---------------------------------------------------------------------------

static bool isInternalWidgetName(const QString& name) {
  return name.startsWith("qt_");
}

void connectWidgetSignals(QWidget* root, WidgetEventCallback callback) {
  using PJ::WidgetEventBuilder;

  // ChartPreviewWidget instances are unnamed children of their parent QFrame.
  // Wire their viewChanged signals using the parent frame's objectName as the event widget name.
  // Must run after applyWidgetData() so charts that were created on first apply are found here.
  for (auto* chart : findAllIn<PJ::ChartPreviewWidget*>(root)) {
    auto* parent_frame = qobject_cast<QFrame*>(chart->parent());
    if (!parent_frame || parent_frame->objectName().isEmpty()) {
      continue;
    }
    std::string chart_name = parent_frame->objectName().toStdString();
    QObject::connect(
        chart, &PJ::ChartPreviewWidget::viewChanged, chart,
        [callback, chart_name](double x_min, double x_max, double y_min, double y_max) {
          callback(chart_name, WidgetEventBuilder::chartViewChanged(x_min, x_max, y_min, y_max));
        });
  }

  // Enter inside any text field triggers the panel's default button, matching
  // native dialog behavior — panels are bare QWidgets, so Qt's own default-button
  // auto-trigger (a QDialog feature) does not apply. Resolved once; click() is a
  // no-op on a disabled button, so a busy/disabled default is ignored naturally,
  // but it does fire on a hidden one, so visibility is checked at Return time.
  QPushButton* default_button = nullptr;
  for (auto* btn : findAllIn<QPushButton*>(root)) {
    if (btn->isDefault()) {
      default_button = btn;
      break;
    }
  }

  for (auto* w : findAllIn<QWidget*>(root)) {
    QString qname = w->objectName();
    if (qname.isEmpty() || isInternalWidgetName(qname)) {
      continue;
    }
    std::string name = qname.toStdString();

    if (auto* le = qobject_cast<QLineEdit*>(w)) {
      QObject::connect(le, &QLineEdit::textChanged, le, [callback, name](const QString& text) {
        callback(name, WidgetEventBuilder::textChanged(text.toStdString()));
      });
      if (default_button != nullptr) {
        // Enter-to-default for hosts WITHOUT native Return handling (docked
        // panels in the QMainWindow). Inside a QDialog, Qt already clicks the
        // default button when a line edit ignores Return — clicking here too
        // would double-fire it — so this connection stands down there.
        QObject::connect(le, &QLineEdit::returnPressed, default_button, [default_button]() {
          if (default_button->isVisible() && qobject_cast<QDialog*>(default_button->window()) == nullptr) {
            default_button->click();
          }
        });
      }
      continue;
    }
    if (auto* pte = qobject_cast<QPlainTextEdit*>(w)) {
      // Only wire code editors (marked by _pj_code_lang property), not read-only plain text.
      if (pte->property("_pj_code_lang").isValid()) {
        // Caret-tracking editors (opt-in via setCodeCaretTracking) emit code +
        // caret offset on both edits and cursor moves, so caret-aware completion
        // can react to the cursor even when the text didn't change. Editors that
        // didn't opt in fire on text changes only and carry no caret — the
        // pre-caret behavior — so an editor that merely validates code isn't
        // re-run on every cursor move.
        if (pte->property("_pj_caret_tracking").toBool()) {
          auto emit_code = [callback, name, pte]() {
            callback(
                name, WidgetEventBuilder::codeChanged(pte->toPlainText().toStdString(), pte->textCursor().position()));
          };
          QObject::connect(pte, &QPlainTextEdit::textChanged, pte, emit_code);
          QObject::connect(pte, &QPlainTextEdit::cursorPositionChanged, pte, emit_code);
        } else {
          QObject::connect(pte, &QPlainTextEdit::textChanged, pte, [callback, name, pte]() {
            callback(name, WidgetEventBuilder::codeChanged(pte->toPlainText().toStdString()));
          });
        }
      }
      continue;
    }
    if (auto* cb = qobject_cast<QComboBox*>(w)) {
      QObject::connect(cb, &QComboBox::currentIndexChanged, cb, [callback, name, cb](int index) {
        callback(name, WidgetEventBuilder::indexChanged(index, cb->currentText().toStdString()));
      });
      // Editable combos let the user type a free value (server URI, etc.).
      // currentIndexChanged alone never fires while typing, and the typed
      // dispatcher routes index events to onIndexChanged — so a plugin reading
      // the value via onTextChanged would never see it. editTextChanged fires
      // for both typing and dropdown selection (selection updates the line
      // edit), so forward it as a text_changed event. currentIndexChanged is
      // kept for index-based consumers (onIndexChanged).
      if (cb->isEditable()) {
        QObject::connect(cb, &QComboBox::editTextChanged, cb, [callback, name](const QString& text) {
          callback(name, WidgetEventBuilder::textChanged(text.toStdString()));
        });
      }
      continue;
    }
    if (auto* ck = qobject_cast<QCheckBox*>(w)) {
      QObject::connect(ck, &QCheckBox::toggled, ck, [callback, name](bool checked) {
        callback(name, WidgetEventBuilder::toggled(checked));
      });
      continue;
    }
    if (auto* ts = qobject_cast<ToggleSwitch*>(w)) {
      QObject::connect(ts, &ToggleSwitch::toggled, ts, [callback, name](bool checked) {
        callback(name, WidgetEventBuilder::toggled(checked));
      });
      continue;
    }
    if (auto* rb = qobject_cast<QRadioButton*>(w)) {
      QObject::connect(rb, &QRadioButton::toggled, rb, [callback, name](bool checked) {
        callback(name, WidgetEventBuilder::toggled(checked));
      });
      continue;
    }
    if (auto* sb = qobject_cast<QSpinBox*>(w)) {
      QObject::connect(sb, &QSpinBox::valueChanged, sb, [callback, name](int value) {
        callback(name, WidgetEventBuilder::valueChanged(value));
      });
      continue;
    }
    if (auto* dsb = qobject_cast<QDoubleSpinBox*>(w)) {
      QObject::connect(dsb, &QDoubleSpinBox::valueChanged, dsb, [callback, name](double value) {
        callback(name, WidgetEventBuilder::valueChanged(value));
      });
      continue;
    }
    if (auto* lw = qobject_cast<QListWidget*>(w)) {
      QObject::connect(lw, &QListWidget::itemSelectionChanged, lw, [callback, name, lw]() {
        std::vector<std::string> sel;
        for (auto* item : lw->selectedItems()) {
          sel.push_back(item->text().toStdString());
        }
        callback(name, WidgetEventBuilder::selectionChanged(sel));
      });
      QObject::connect(lw, &QListWidget::itemDoubleClicked, lw, [callback, name, lw](QListWidgetItem* item) {
        // Report the delivered-order index, not the (possibly sorted) view row.
        callback(name, WidgetEventBuilder::itemDoubleClicked(listItemPluginIndex(lw, item)));
      });
      // Per-row trash button: the delegate only draws / handles it when the list
      // carries the pj_deletable property (set from setListItemsDeletable), so it
      // is inert on ordinary lists.
      lw->setItemDelegate(new ListRowDeleteDelegate(
          lw, [callback, name](int row) { callback(name, WidgetEventBuilder::itemDeleteRequested(row)); }));
      // Opt-in per-row context menu (kContextActionsProperty). An absent or
      // empty property is today's behaviour: no menu, no policy change. Right-
      // click never collides with the trash delegate above, which only
      // consumes Qt::LeftButton.
      if (auto actions = parseContextActions(lw->property(kContextActionsProperty).toString()); !actions.empty()) {
        lw->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(
            lw, &QListWidget::customContextMenuRequested, lw,
            [callback, name, lw, actions = std::move(actions)](const QPoint& pos) {
              QListWidgetItem* item = lw->itemAt(pos);
              if (item == nullptr) {
                return;  // no row under the cursor: no menu
              }
              // The plugin's reply to the selection change below, or a panel
              // tick during the menu's nested loop, can apply new listItems,
              // which rebuilds the list and deletes `item`. A persistent index
              // goes invalid instead of dangling, so the row is re-resolved
              // through the model at every later step, never via the pointer.
              const QPersistentModelIndex row_index = lw->indexFromItem(item);
              // Standard rule: right-clicking a row outside the current
              // selection targets just that row.
              if (!item->isSelected()) {
                lw->setCurrentItem(item);
              }
              item = nullptr;
              if (!row_index.isValid()) {
                return;  // the selection reply replaced the list: no row to act on
              }
              // Flat QPushButtons in QWidgetActions — the house pattern for a
              // PJMenu (CurveListPanel::onTreeContextMenu). QWidgetAction
              // buttons don't dismiss the menu on click — close it ourselves
              // and record the choice (exec blocks, so capturing by
              // reference is safe).
              QMenu menu(lw);
              menu.setObjectName(u"PJMenu"_s);
              QString chosen_id;
              for (const auto& action : actions) {
                auto* button = new QPushButton(action.label, &menu);
                button->setFlat(true);
                auto* widget_action = new QWidgetAction(&menu);
                widget_action->setDefaultWidget(button);
                menu.addAction(widget_action);
                QObject::connect(button, &QPushButton::clicked, &menu, [&menu, &chosen_id, id = action.id]() {
                  chosen_id = id;
                  menu.close();
                });
              }
              menu.exec(lw->viewport()->mapToGlobal(pos));
              QListWidgetItem* row = row_index.isValid() ? lw->itemFromIndex(row_index) : nullptr;
              if (!chosen_id.isEmpty() && row != nullptr) {
                callback(
                    name, WidgetEventBuilder::itemContextAction(listItemPluginIndex(lw, row), chosen_id.toStdString()));
              }
            });
      }
      continue;
    }
    if (auto* tw = qobject_cast<QTableWidget*>(w)) {
      QObject::connect(tw, &QTableWidget::itemSelectionChanged, tw, [callback, name, tw]() {
        // Emit one entry per selected row, keyed by tableRowKeyText (see its
        // doc comment for why a fixed column 0 doesn't work). This is the same
        // key the selected_items apply path matches rows by, so the two
        // directions stay in sync.
        std::vector<std::string> sel;
        std::vector<int> seen_rows;
        for (auto* item : tw->selectedItems()) {
          const int row = item->row();
          bool dup = false;
          for (int r : seen_rows) {
            if (r == row) {
              dup = true;
              break;
            }
          }
          if (dup) {
            continue;
          }
          seen_rows.push_back(row);
          if (auto key = tableRowKeyText(tw, row)) {
            sel.push_back(*key);
          }
        }
        callback(name, WidgetEventBuilder::selectionChanged(sel));
      });
      // Double-click a row -> itemDoubleClicked(row), mirroring QListWidget so a
      // plugin can implement double-click-to-use on a table (e.g. the function
      // library box). Emits the plugin-order index of the double-clicked row,
      // translated from the (possibly user-sorted) view position.
      QObject::connect(tw, &QTableWidget::cellDoubleClicked, tw, [callback, name, tw](int row, int /*col*/) {
        callback(name, WidgetEventBuilder::itemDoubleClicked(viewRowToPluginRow(tw, row)));
      });
      // Per-row trash button, matching QListWidget. The delegate is inert unless
      // setListItemsDeletable enabled the pj_deletable property on this table.
      tw->setItemDelegate(new ListRowDeleteDelegate(
          tw, [callback, name](int row) { callback(name, WidgetEventBuilder::itemDeleteRequested(row)); }));
      // Header click -> headerClicked(section), letting a plugin own its column
      // sorting (it re-orders its row model and re-emits, so index-based selection
      // and visibility stay consistent). A plugin that doesn't override
      // onHeaderClicked returns false from the dispatch, the host then skips the
      // re-read, and the click is a no-op — so wiring this unconditionally is safe.
      QObject::connect(tw->horizontalHeader(), &QHeaderView::sectionClicked, tw, [callback, name, tw](int section) {
        callback(name, WidgetEventBuilder::headerClicked(section));
        // Qt flipped the visible arrow to the clicked section BEFORE this signal
        // fired — even with sorting off. For a plugin-owned indicator the
        // delivered state is the truth: if the callback re-delivered widget
        // data, applyToWidget just refreshed the properties; if the plugin
        // ignored the click (or the re-delivery was diffed away as unchanged),
        // they still hold the last delivered state. Either way, re-asserting
        // them keeps an ignored click from leaving the header lying about the
        // sort. Qt-sorted tables (sortingEnabled) own their arrow — skip.
        if (!tw->isSortingEnabled()) {
          auto* header = tw->horizontalHeader();
          const QVariant col = header->property("pjSortIndicatorCol");
          const QVariant asc = header->property("pjSortIndicatorAsc");
          if (col.isValid() && asc.isValid()) {
            const QSignalBlocker header_blocker(header);
            header->setSortIndicatorShown(true);
            header->setSortIndicator(col.toInt(), asc.toBool() ? Qt::AscendingOrder : Qt::DescendingOrder);
          }
        }
      });
      // Stash the event callback so applyTableRadioColumn can wire radio cells
      // (created lazily as rows arrive) back to the dialog event stream. The
      // clicked radio resolves to a view row; the plugin expects its own order.
      new RadioEmitHolder(tw, [callback, name, tw](int row) {
        callback(name, WidgetEventBuilder::tableRadioSelected(viewRowToPluginRow(tw, row)));
      });
      // Column-selector wiring: a table naming a sibling QListWidget in
      // kColumnSelectorListProperty becomes a read-only column mirror of that
      // list. Selecting a list row highlights the matching table COLUMN (matched
      // by header text), and all direct user selection on the table is disabled —
      // the list is the only driver.
      if (const QString list_name = tw->property(kColumnSelectorListProperty).toString(); !list_name.isEmpty()) {
        if (auto* list = findNamedIn<QListWidget*>(root, list_name)) {
          tw->setSelectionBehavior(QAbstractItemView::SelectColumns);
          tw->setSelectionMode(QAbstractItemView::ExtendedSelection);
          tw->setFocusPolicy(Qt::NoFocus);
          tw->horizontalHeader()->setSectionsClickable(false);
          tw->verticalHeader()->setSectionsClickable(false);
          tw->viewport()->installEventFilter(new SelectionGestureBlocker(tw));
          // Live sync on user selection; the programmatic case is covered by the
          // post-apply pass in applyWidgetData (see syncColumnSelectorTable).
          QObject::connect(
              list, &QListWidget::itemSelectionChanged, tw, [list, tw]() { syncColumnSelectorTable(tw, list); });
        }
      }
      continue;
    }
    if (auto* btn = qobject_cast<QPushButton*>(w)) {
      // Skip buttons that are part of QDialogButtonBox
      if (qobject_cast<QDialogButtonBox*>(btn->parent())) {
        continue;
      }
      QObject::connect(
          btn, &QPushButton::clicked, btn, [callback, name]() { callback(name, WidgetEventBuilder::clicked()); });
      continue;
    }
    if (auto* tw = qobject_cast<QTabWidget*>(w)) {
      QObject::connect(tw, &QTabWidget::currentChanged, tw, [callback, name](int index) {
        callback(name, WidgetEventBuilder::tabChanged(index));
      });
      continue;
    }
    if (auto* rs = qobject_cast<RangeSlider*>(w)) {
      // Both handle signals coalesce into one rangeChanged event carrying the
      // current lower+upper, so dragging either handle keeps the plugin in sync.
      auto emit_range = [callback, name, rs]() {
        callback(name, WidgetEventBuilder::rangeChanged(rs->getLowerValue(), rs->getUpperValue()));
      };
      QObject::connect(rs, &RangeSlider::lowerValueChanged, rs, [emit_range](int) { emit_range(); });
      QObject::connect(rs, &RangeSlider::upperValueChanged, rs, [emit_range](int) { emit_range(); });
      continue;
    }
    if (auto* mt = qobject_cast<MarkerTimeline*>(w)) {
      // A user drag/resize/delete republishes the whole mark set to the plugin.
      QObject::connect(mt, &MarkerTimeline::marksChanged, mt, [callback, name, mt]() {
        std::vector<TimelineMark> marks;
        marks.reserve(mt->marks().size());
        for (const MarkerTimeline::Mark& m : mt->marks()) {
          marks.push_back({m.id, m.kind == MarkerTimeline::Kind::kRegion, m.start, m.end});
        }
        callback(name, WidgetEventBuilder::markerTimelineChanged(marks));
      });
      continue;
    }
    if (auto* drp = qobject_cast<DateRangePicker*>(w)) {
      QObject::connect(drp, &DateRangePicker::filterChanged, drp, [callback, name](const RangeFilter& f) {
        // Combine date + time into UTC ISO datetimes; empty string = unbounded side.
        std::string from_iso;
        std::string to_iso;
        if (f.date_from.has_value()) {
          from_iso =
              QDateTime(*f.date_from, f.from_time, QTimeZone(QTimeZone::UTC)).toString(Qt::ISODate).toStdString();
        }
        if (f.date_to.has_value()) {
          to_iso = QDateTime(*f.date_to, f.to_time, QTimeZone(QTimeZone::UTC)).toString(Qt::ISODate).toStdString();
        }
        callback(name, WidgetEventBuilder::dateRangeChanged(from_iso, to_iso));
      });
      continue;
    }
    if (auto* dte = qobject_cast<QDateTimeEdit*>(w)) {
      QObject::connect(dte, &QDateTimeEdit::dateTimeChanged, dte, [callback, name](const QDateTime& dt) {
        // Wall-clock local time, verbatim. Fractional seconds only when the
        // editor actually carries them, so whole-second events stay bare.
        const Qt::DateFormat format = dt.time().msec() != 0 ? Qt::ISODateWithMs : Qt::ISODate;
        callback(name, WidgetEventBuilder::dateTimeChanged(dt.toString(format).toStdString()));
      });
      continue;
    }
  }
}

// ---------------------------------------------------------------------------
// installButtonShortcuts — create QShortcuts for buttons declaring a shortcut
// ---------------------------------------------------------------------------

void installButtonShortcuts(QWidget* root, const PJ::WidgetDataView& view) {
  for (const auto& name : view.widgetNames()) {
    auto sc = view.shortcut(name);
    if (!sc) {
      continue;
    }
    auto* btn = findNamedIn<QPushButton*>(root, QString::fromStdString(name));
    if (!btn) {
      continue;
    }
    auto* shortcut = new QShortcut(QKeySequence(QString::fromStdString(*sc)), root);
    QObject::connect(shortcut, &QShortcut::activated, btn, &QPushButton::click);
  }
}

namespace {

// Opt-in .ui dynamic properties: one or more "<comboObjectName>:<index>
// [,<index>...]" clauses joined by ';'. A rule holds when EVERY clause's
// combo sits on one of that clause's indices (AND across clauses); a single
// clause is the common case.
constexpr const char* kEnableWhenProperty = "pj_enable_when";
constexpr const char* kVisibleWhenProperty = "pj_visible_when";

// The rules installed under one root, resolved once: refreshDeclarativeRules
// re-runs these closures instead of re-walking the tree and re-parsing every
// rule string on each data tick. Parented to the root so it dies with it, and
// found again by type. A refresh from any ancestor root reaches it: engines
// install on the loaded .ui root but refresh from the dialog that wraps it.
class InstalledRules : public QObject {
  Q_OBJECT
 public:
  explicit InstalledRules(QWidget* root) : QObject(root) {}
  std::vector<std::function<void()>> reassert;
};

// One clause: the governing combo plus the indices on which it holds. The
// combo is a QPointer because the rule outlives injected subtrees (a parser
// dialog swapped for another protocol's) in the root's cache.
struct ComboRule {
  QPointer<QComboBox> combo;
  QList<int> indices;
};

// Parses `property` off `w`. Any clause that fails (no colon, unknown combo,
// no numeric index) invalidates the WHOLE rule — returns empty, leaving the
// widget untouched, same as the single-rule parser this replaces.
std::vector<ComboRule> parseComboRules(QWidget* root, const QWidget* w, const char* property) {
  const QVariant rule = w->property(property);
  if (!rule.isValid()) {
    return {};
  }
  std::vector<ComboRule> rules;
  for (const QString& raw_clause : rule.toString().split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
    const QString clause = raw_clause.trimmed();
    const qsizetype colon = clause.indexOf(QLatin1Char(':'));
    if (colon <= 0) {
      return {};
    }
    auto* combo = findNamedIn<QComboBox*>(root, clause.left(colon).trimmed());
    if (combo == nullptr) {
      return {};
    }
    QList<int> indices;
    for (const QString& tok : clause.mid(colon + 1).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
      bool ok = false;
      const int idx = tok.trimmed().toInt(&ok);
      if (ok) {
        indices.push_back(idx);
      }
    }
    if (indices.isEmpty()) {
      return {};
    }
    rules.push_back(ComboRule{combo, std::move(indices)});
  }
  return rules;
}

bool comboRulesHold(const std::vector<ComboRule>& rules) {
  return std::all_of(rules.begin(), rules.end(), [](const ComboRule& rule) {
    return !rule.combo.isNull() && rule.indices.contains(rule.combo->currentIndex());
  });
}

// Shared walk for both the "pj_enable_when" and "pj_visible_when" rules:
// `apply` is setEnabled for one, redirected setVisible for the other.
void installComboRules(QWidget* root, InstalledRules& installed, const char* property, void (*apply)(QWidget*, bool)) {
  // findAllIn yields descendants only; a rule authored on the loaded root counts too.
  for (QWidget* w : QList<QWidget*>{root} + findAllIn<QWidget*>(root)) {
    const auto rules = parseComboRules(root, w, property);
    if (rules.empty()) {
      continue;
    }
    // The target is a QPointer for the same reason the combos are: a cached
    // rule whose widget was deleted with a replaced subtree must become a no-op.
    const auto reassert = [target = QPointer<QWidget>(w), rules, apply]() {
      if (!target.isNull()) {
        apply(target.data(), comboRulesHold(rules));
      }
    };
    // Any governing combo changing re-evaluates the WHOLE rule (all clauses),
    // not just the one that moved.
    for (const ComboRule& clause : rules) {
      QObject::connect(clause.combo, &QComboBox::currentIndexChanged, w, [reassert](int) { reassert(); });
    }
    reassert();  // initial state through the same predicate every re-assert uses
    installed.reassert.push_back(reassert);
  }
}

void setEnabledFromRule(QWidget* w, bool holds) {
  w->setEnabled(holds);
}

void setVisibleFromRule(QWidget* w, bool holds) {
  // A swapped checkbox/radio hides its adapter replacement, not the hidden
  // original — see redirectAdaptedVisibility. Un-adapted widgets just hide.
  if (!redirectAdaptedVisibility(w, holds)) {
    w->setVisible(holds);
  }
}

}  // namespace

void installDeclarativeRules(QWidget* root) {
  auto* installed = new InstalledRules(root);
  installComboRules(root, *installed, kEnableWhenProperty, setEnabledFromRule);
  installComboRules(root, *installed, kVisibleWhenProperty, setVisibleFromRule);
}

void refreshDeclarativeRules(QWidget* root) {
  for (InstalledRules* installed : root->findChildren<InstalledRules*>()) {
    for (const auto& reassert : installed->reassert) {
      reassert();
    }
  }
}

}  // namespace PJ

#include "widget_binding.moc"
