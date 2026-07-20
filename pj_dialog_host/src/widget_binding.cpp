// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <pj_plotting/PlotWidget.h>
#include <pj_runtime/AppSession.h>
#include <pj_runtime/CatalogModel.h>
#include <pj_widgets/DateRangePicker.h>
#include <pj_widgets/RangeSlider.h>
#include <pj_widgets/SvgUtil.h>
#include <pj_widgets/ToggleSwitch.h>
#include <qwt_plot_curve.h>
#include <qwt_point_data.h>

#include <QAbstractItemModel>
#include <QBoxLayout>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
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
#include <QTextCursor>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
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
#include "python_syntax_highlighter.hpp"
using namespace Qt::StringLiterals;

namespace PJ {

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
  if (icon_name == "search") {
    return u":/resources/svg/search_light.svg"_s;
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

// Paints a trailing trash icon on every row of a QListWidget and turns a click
// on that icon into an itemDeleteRequested(row) event. Only active when the list
// carries a true "pj_deletable" dynamic property (set from WidgetData), so the
// same delegate is harmless on non-deletable lists. Clicks off the icon fall
// through untouched, so selection and double-click-to-load still work.
class ListRowDeleteDelegate : public QStyledItemDelegate {
 public:
  static constexpr int kIconExtent = 16;
  static constexpr int kIconMargin = 6;
  static constexpr int kRowVPadding = 6;  // matches QPushButton's QSS padding

  ListRowDeleteDelegate(QObject* parent, std::function<void(int)> on_delete)
      : QStyledItemDelegate(parent), on_delete_(std::move(on_delete)) {}

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    QStyledItemDelegate::paint(painter, option, index);
    if (!deletable()) {
      return;
    }
    const qreal dpr = painter->device() != nullptr ? painter->device()->devicePixelRatioF() : 1.0;
    painter->drawPixmap(iconRect(option.rect), rowTrashPixmap(option.palette.color(QPalette::Text), kIconExtent, dpr));
  }

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    if (deletable()) {
      s.setWidth(s.width() + kIconExtent + (2 * kIconMargin));
      // Match the Save button's height: text + the same 6px vertical padding a
      // QPushButton uses, so a row reads as the same size as the button below.
      s.setHeight(qMax(s.height(), option.fontMetrics.height() + (2 * kRowVPadding)));
    }
    return s;
  }

  bool editorEvent(
      QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index) override {
    if (deletable() && event->type() == QEvent::MouseButtonRelease) {
      const auto* me = static_cast<QMouseEvent*>(event);
      if (me->button() == Qt::LeftButton && iconRect(option.rect).contains(me->pos())) {
        // Report the delivered-order (plugin) index, not the view row: on a
        // sorted list index.row() names a DIFFERENT underlying item, so the
        // trash button would delete the wrong thing (double-click already
        // translates the same way — see listItemPluginIndex).
        const QVariant tag = index.data(kPluginRowRole);
        on_delete_(tag.isValid() ? tag.toInt() : index.row());
        return true;  // consume so it isn't also read as a selection change
      }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
  }

 private:
  [[nodiscard]] bool deletable() const {
    const auto* list = qobject_cast<const QWidget*>(parent());
    return list != nullptr && list->property("pj_deletable").toBool();
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

  // Keep the radio column just wide enough for the button. Marking it Fixed both
  // pins its width and redirects TreeLikeHeaderSizer (which fills the first
  // non-Fixed column) to the first real data column, so the radio never
  // over-widens when it sits first. The data columns stay Interactive so their
  // dividers keep dragging.
  auto* header = tw->horizontalHeader();
  header->setSectionResizeMode(col, QHeaderView::Fixed);
  tw->setColumnWidth(col, 36);
  for (int c = 0; c < tw->columnCount(); ++c) {
    if (c != col) {
      header->setSectionResizeMode(c, QHeaderView::Interactive);
      break;
    }
  }
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

// Owns the tree-like header's default column sizing, with every section left
// QHeaderView::Interactive so every divider drags (Stretch/ResizeToContents
// sections are auto-sized and Qt refuses to drag their dividers):
//
// - The fill column — the first draggable one, normally 0 — absorbs the
//   leftover viewport width, visually a Stretch section: it refits on viewport
//   resizes and gives-and-takes on data-column drags.
// - Data columns track their content width (the formula of Qt's divider
//   double-click auto-fit) as rows arrive and change, so cell text is not
//   clipped by default.
// - The fill column refits no lower than its own content width: a too-narrow
//   dialog grows a horizontal scrollbar instead of clipping.
// - A column whose divider the user drags becomes user-owned: content sizing
//   and refit stop touching it. Ownership is detected by a left press on the
//   header itself — NOT by global mouse state, because a first-show layout
//   pass can emit sectionResized while the button is down on an unrelated
//   widget (e.g. the tab whose click just revealed this table). A column-count
//   rebuild resets all ownership.
//
// Parented to the header, so it dies with the table.
class TreeLikeHeaderSizer : public QObject {
 public:
  explicit TreeLikeHeaderSizer(QTableWidget* tw) : QObject(tw->horizontalHeader()), tw_(tw) {
    auto* header = tw->horizontalHeader();
    QObject::connect(header, &QHeaderView::sectionResized, this, [this](int logical, int, int) {
      if (refitting_) {
        return;
      }
      if (header_pressed_) {
        user_columns_.insert(logical);
        if (logical == fillColumn()) {
          return;  // never refit against the user's own drag of the fill column
        }
      }
      refit();
    });
    // A column-count rebuild recreates sections with default sizes and voids
    // every per-column decision made so far.
    QObject::connect(header, &QHeaderView::sectionCountChanged, this, [this](int, int) {
      user_columns_.clear();
      fill_floor_ = 0;
      fill_floor_column_ = -1;
      scheduleContentSize();
      scheduleRefit();
    });
    header->viewport()->installEventFilter(this);  // press/release = user-drag detection
    // Watch the table and both scrollbars, not just the viewport: when a
    // scrollbar appears during show(), Qt resizes the still-hidden viewport and
    // the pending resize event is never delivered, so a viewport-only filter
    // misses the change.
    tw->installEventFilter(this);
    tw->viewport()->installEventFilter(this);
    tw->verticalScrollBar()->installEventFilter(this);
    tw->horizontalScrollBar()->installEventFilter(this);
    // Content widths follow the rows. rowsInserted fires on setRowCount —
    // before the items are filled in — so sizing runs queued, after the
    // delivery settles. The constructor schedules once for rows that predate
    // the sizer (a table whose rows were delivered before its headers).
    auto* model = tw->model();
    QObject::connect(model, &QAbstractItemModel::rowsInserted, this, [this]() { scheduleContentSize(); });
    QObject::connect(model, &QAbstractItemModel::rowsRemoved, this, [this]() { scheduleContentSize(); });
    QObject::connect(model, &QAbstractItemModel::modelReset, this, [this]() { scheduleContentSize(); });
    // A same-row-count re-delivery updates items in place: no rows signals, only
    // dataChanged (coalesced by the pending flag into one pass per loop turn).
    QObject::connect(
        model, &QAbstractItemModel::dataChanged, this,
        [this](const QModelIndex&, const QModelIndex&, const QList<int>&) { scheduleContentSize(); });
    scheduleContentSize();
    refit();
  }

  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == tw_->horizontalHeader()->viewport()) {
      if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        header_pressed_ = true;
      } else if (event->type() == QEvent::MouseButtonRelease) {
        header_pressed_ = false;
      }
      return QObject::eventFilter(watched, event);
    }
    switch (event->type()) {
      case QEvent::Resize:
        if (watched == tw_->viewport()) {
          // Synchronous: the viewport geometry is final inside its resize
          // event, and a queued refit would paint one frame with a stale fill
          // width (visible as lag while the user drags the dialog edge).
          refit();
        } else {
          scheduleRefit();
        }
        break;
      case QEvent::Show:
      case QEvent::Hide:
        scheduleRefit();
        break;
      default:
        break;
    }
    return QObject::eventFilter(watched, event);
  }

 private:
  // QTableView re-protects QAbstractItemView's public sizeHintForColumn, so the
  // content width is read through the base class.
  int contentHint(int column) const {
    return static_cast<const QAbstractItemView*>(tw_)->sizeHintForColumn(column);
  }

  // Normally 0; when a Fixed section leads (e.g. applyTableRadioColumn's radio
  // column), fill the first draggable column after it instead.
  int fillColumn() const {
    auto* header = tw_->horizontalHeader();
    for (int i = 0; i < header->count(); ++i) {
      if (header->sectionResizeMode(i) != QHeaderView::Fixed) {
        return i;
      }
    }
    return -1;
  }

  void scheduleRefit() {
    if (refit_pending_) {
      return;
    }
    refit_pending_ = true;
    QTimer::singleShot(0, this, [this]() {
      refit_pending_ = false;
      refit();
    });
  }

  void scheduleContentSize() {
    if (content_size_pending_) {
      return;
    }
    content_size_pending_ = true;
    QTimer::singleShot(0, this, [this]() {
      content_size_pending_ = false;
      contentSizeColumns();
    });
  }

  // Fit every draggable, non-user-owned, non-fill column to the width Qt's own
  // double-click auto-fit would give it (max of content and header label), so
  // no cell text is clipped. Re-runs whenever the row set changes; columns the
  // user has dragged are left alone. The fill column is not resized here — it
  // gets a content floor so refit() stops shrinking it below its longest entry
  // (a horizontal scrollbar appears instead).
  void contentSizeColumns() {
    auto* header = tw_->horizontalHeader();
    if (tw_->rowCount() == 0 || header->count() == 0) {
      return;
    }
    const int fill = fillColumn();
    for (int i = 0; i < header->count(); ++i) {
      if (i == fill || user_columns_.count(i) > 0 || header->sectionResizeMode(i) != QHeaderView::Interactive) {
        continue;
      }
      const int content = std::max(contentHint(i), header->sectionSizeHint(i));
      if (content > 0 && content != header->sectionSize(i)) {
        refitting_ = true;
        header->resizeSection(i, content);
        refitting_ = false;
      }
    }
    if (fill >= 0) {
      fill_floor_ = std::max(contentHint(fill), header->sectionSizeHint(fill));
      fill_floor_column_ = fill;
    }
    refit();
  }

  void refit() {
    auto* header = tw_->horizontalHeader();
    const int fill = fillColumn();
    if (fill < 0 || user_columns_.count(fill) > 0) {
      return;
    }
    // The fill column can change after sizing (a later delivery pinning a radio
    // column Fixed); the floor belongs to the column, not the slot.
    if (fill != fill_floor_column_ && tw_->rowCount() > 0) {
      fill_floor_ = std::max(contentHint(fill), header->sectionSizeHint(fill));
      fill_floor_column_ = fill;
    }
    int others = 0;
    for (int i = 0; i < header->count(); ++i) {
      if (i != fill) {
        others += header->sectionSize(i);
      }
    }
    const int floor_width = std::max(fill_floor_, header->minimumSectionSize());
    const int target = std::max(tw_->viewport()->width() - others, floor_width);
    if (target == header->sectionSize(fill)) {
      return;
    }
    refitting_ = true;
    header->resizeSection(fill, target);
    refitting_ = false;
  }

  QTableWidget* tw_;
  std::set<int> user_columns_;  ///< columns the user has dragged; sizing keeps hands off
  bool refitting_ = false;
  bool refit_pending_ = false;
  bool content_size_pending_ = false;
  bool header_pressed_ = false;  ///< left button currently down on the header viewport
  int fill_floor_ = 0;           ///< content width of fill_floor_column_; 0 until computed
  int fill_floor_column_ = -1;
};

// Size a topic/curve table the way it reads best: the first column fills the
// viewport (no dead grey space to the right; emulated by TreeLikeHeaderSizer so it
// stays draggable) while every other column is a user-draggable width. WA_Hover lets the QSS
// `QHeaderView::section:hover` divider tint fire; header weight is left to the app stylesheet (the global
// `QHeaderView::section { font-weight: normal }` rule), which reads consistently
// with CurveTreeView — a widget-side setFont would be ignored while a stylesheet
// is active anyway.
//
// The resize modes are persistent (set once and kept), so this is guarded by a
// dynamic property and a column-count check: it's safe to call on every
// widget_data delivery — it configures the first time the table actually has
// columns and no-ops after. This is deliberately NOT gated on the header *labels*
// changing: dialogs whose .ui predefines column headers (e.g. MCAP's tableWidget)
// match the plugin's setTableHeaders() verbatim, so a label-change gate would skip
// them entirely and leave the .ui's default un-stretched sizing — the very bug
// this fixes.
static void installTreeLikeHeader(QTableWidget* tw) {
  auto* header = tw->horizontalHeader();
  if (header->count() == 0 || tw->property("pjTreeLikeHeader").toBool()) {
    return;
  }
  tw->setProperty("pjTreeLikeHeader", true);

  header->setStretchLastSection(false);
  header->setMinimumSectionSize(20);
  header->setAttribute(Qt::WA_Hover, true);
  header->viewport()->setAttribute(Qt::WA_Hover, true);

  // Every column is Interactive so every divider drags (Stretch/ResizeToContents
  // sections are auto-sized and Qt makes their dividers dead). The name column's
  // fill-the-viewport behavior is emulated by TreeLikeHeaderSizer instead of
  // QHeaderView::Stretch precisely so it stays user-resizable; resizing a data
  // column gives and takes from the name column until the user claims it.
  header->setSectionResizeMode(0, QHeaderView::Interactive);
  for (int i = 1; i < header->count(); ++i) {
    header->setSectionResizeMode(i, QHeaderView::Interactive);
    header->resizeSection(i, 96);
  }
  new TreeLikeHeaderSizer(tw);
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
  // PJ3 parity: invalid input fields use an error background, not a border.
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
          sel + QStringLiteral(" { background-color: %1; color: %2; }")
                    .arg(
                        theme::statusErrorSurface(fw_theme).name(QColor::HexArgb),
                        theme::onStatusErrorSurface(fw_theme).name(QColor::HexArgb)));
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
      pte->setPlainText(QString::fromStdString(*pt));
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
    if (auto v = view.selectedItems(name)) {
      std::set<std::string> selected(v->begin(), v->end());
      for (int i = 0; i < lw->count(); ++i) {
        auto* item = lw->item(i);
        item->setSelected(selected.count(item->text().toStdString()) > 0);
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
    // The dialog protocol carries no cell-edit event (only selection, double-click
    // and radio), so an edited cell can never be read back by the plugin — an
    // editable cell silently discards the edit on accept. Force read-only on every
    // protocol table so no picker looks editable when it isn't. Persistent and
    // idempotent, so setting it on each delivery is free. A future editable table
    // would need a new protocol event anyway, and would opt out here then.
    tw->setEditTriggers(QAbstractItemView::NoEditTriggers);
    if (auto v = view.tableHeaders(name)) {
      QStringList hdr;
      for (const auto& h : *v) {
        hdr << QString::fromStdString(h);
      }
      // Re-setting labels reconfigures the header (not free), so only do it when
      // they actually changed. The sizing setup below is separate: it must also
      // run for dialogs whose .ui predefines matching headers (e.g. MCAP), where
      // this branch is skipped — hence InstallTreeLikeHeader lives outside it.
      if (!tableMatchesHeaders(tw, hdr)) {
        tw->setColumnCount(static_cast<int>(hdr.size()));
        tw->setHorizontalHeaderLabels(hdr);
      }
      // First column fills the width, the rest hug content. Idempotent + guarded,
      // so calling it on every delivery is cheap (port/fix of #90).
      installTreeLikeHeader(tw);
    }
    // The radio column is read up front: recordPluginKeyColumn needs to know
    // which column carries radio widgets (no item text) to pick the key column.
    const std::optional<int> radio_col = view.tableRadioColumn(name);
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
      // Radio tables need the tree-like sizing even when the plugin never sends
      // table headers (.ui-predefined columns): the fill behavior lives in the
      // sizer, and applyTableRadioColumn below relies on it to fill the first
      // draggable column. Idempotent — a no-op when the headers branch above
      // already installed it.
      installTreeLikeHeader(tw);
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
        bool is_disabled = disabled.count(view_to_plugin[static_cast<std::size_t>(r)]) > 0;
        for (int c = 0; c < tw->columnCount(); ++c) {
          if (auto* item = tw->item(r, c)) {
            auto flags = item->flags();
            if (is_disabled) {
              flags &= ~Qt::ItemIsEnabled;
              flags &= ~Qt::ItemIsSelectable;
            } else {
              flags |= Qt::ItemIsEnabled;
              flags |= Qt::ItemIsSelectable;
            }
            item->setFlags(flags);
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
    // (sent in the same tick) must be applied afterwards.
    if (auto v = view.rangeSliderMin(name)) {
      rs->setMinimum(*v);
    }
    if (auto v = view.rangeSliderMax(name)) {
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

  // --- QFrame with chart_series or chart_zoom_enabled → PlotWidget or ChartPreviewWidget ---
  if (auto* frame = qobject_cast<QFrame*>(w)) {
    auto series_data = view.chartSeries(name);
    auto zoom_enabled = view.chartZoomEnabled(name);
    auto auto_zoom = view.chartAutoZoom(name);
    auto chart_placeholder = view.chartPlaceholder(name);
    // chart_placeholder alone must be honored too — a plugin may send the
    // hint before (or without) any series/zoom keys.
    if (series_data || zoom_enabled || chart_placeholder) {
      if ((series_data || zoom_enabled) && session != nullptr && catalog != nullptr) {
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
      } else if (series_data || zoom_enabled) {
        // Fallback: ChartPreviewWidget (no session/catalog available). Guarded
        // like the PlotWidget branch so a placeholder-only payload never
        // constructs a chart widget.
        auto* chart = frame->findChild<PJ::ChartPreviewWidget*>();
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
        if (zoom_enabled) {
          chart->setZoomEnabled(*zoom_enabled);
        }
      }

      // Placeholder overlay: a centered translucent hint shown while the chart
      // has no series (e.g. a drop prompt). Created lazily as a child of the
      // frame; shown/hidden per current data.
      if (chart_placeholder) {
        const bool has_data = series_data && !series_data->empty();
        auto* overlay = frame->findChild<ChartPlaceholderOverlay*>(QString(), Qt::FindDirectChildrenOnly);
        if (overlay == nullptr) {
          overlay = new ChartPlaceholderOverlay(frame);
        }
        overlay->setText(QString::fromStdString(*chart_placeholder));
        overlay->setVisible(!has_data);
        if (!has_data) {
          overlay->recenter();
        }
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
  for (const auto& name : view.widgetNames()) {
    auto* w = root->findChild<QWidget*>(QString::fromStdString(name));
    if (!w) {
      continue;
    }
    applyToWidget(w, name, view, session, catalog);
  }
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
  for (auto* chart : root->findChildren<PJ::ChartPreviewWidget*>()) {
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

  for (auto* w : root->findChildren<QWidget*>()) {
    QString qname = w->objectName();
    if (qname.isEmpty() || isInternalWidgetName(qname)) {
      continue;
    }
    std::string name = qname.toStdString();

    if (auto* le = qobject_cast<QLineEdit*>(w)) {
      QObject::connect(le, &QLineEdit::textChanged, le, [callback, name](const QString& text) {
        callback(name, WidgetEventBuilder::textChanged(text.toStdString()));
      });
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
    if (auto* drp = qobject_cast<DateRangePicker*>(w)) {
      QObject::connect(drp, &DateRangePicker::filterChanged, drp, [callback, name](const RangeFilter& f) {
        // Combine date + time into UTC ISO datetimes; empty string = unbounded side.
        std::string from_iso;
        std::string to_iso;
        if (f.date_from.has_value()) {
          from_iso = QDateTime(*f.date_from, f.from_time, QTimeZone::utc()).toString(Qt::ISODate).toStdString();
        }
        if (f.date_to.has_value()) {
          to_iso = QDateTime(*f.date_to, f.to_time, QTimeZone::utc()).toString(Qt::ISODate).toStdString();
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
    auto* btn = root->findChild<QPushButton*>(QString::fromStdString(name));
    if (!btn) {
      continue;
    }
    auto* shortcut = new QShortcut(QKeySequence(QString::fromStdString(*sc)), root);
    QObject::connect(shortcut, &QShortcut::activated, btn, &QPushButton::click);
  }
}

}  // namespace PJ
