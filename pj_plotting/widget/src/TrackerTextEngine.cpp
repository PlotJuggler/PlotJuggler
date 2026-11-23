// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "TrackerTextEngine.h"

#include <qwt_text_engine.h>

#include <QFontMetricsF>
#include <QList>
#include <QPainter>
#include <QStringView>
#include <algorithm>
#include <cmath>
#include <vector>

namespace PJ {

namespace {

// A row is its ink as "#rrggbb" followed by its cells, separated by kValueBoxCellSeparator; rows are separated by a
// line feed. Every cell but a row's last is a column, right-aligned across the rows that have it; the last cell
// follows the columns.
constexpr qsizetype kInkChars = 7;
constexpr QChar kRowSeparator = u'\n';

struct Row {
  QStringView ink;
  QList<QStringView> cells;
  double tail_width = 0.0;  // of the last cell
};

struct Layout {
  std::vector<Row> rows;
  std::vector<double> column_widths;

  [[nodiscard]] double widthOf(const Row& row) const {
    double width = row.tail_width;
    for (qsizetype column = 0; column + 1 < row.cells.size(); ++column) {
      width += column_widths[static_cast<size_t>(column)];
    }
    return width;
  }
};

[[nodiscard]] Layout layOut(const QString& text, const QFontMetricsF& metrics) {
  Layout layout;
  for (const QStringView line : QStringView(text).tokenize(kRowSeparator)) {
    if (line.size() < kInkChars) {
      continue;
    }
    Row row{.ink = line.first(kInkChars), .cells = line.sliced(kInkChars).split(kValueBoxCellSeparator)};
    for (qsizetype column = 0; column + 1 < row.cells.size(); ++column) {
      if (layout.column_widths.size() <= static_cast<size_t>(column)) {
        layout.column_widths.push_back(0.0);
      }
      double& width = layout.column_widths[static_cast<size_t>(column)];
      width = std::max(width, metrics.horizontalAdvance(row.cells[column].toString()));
    }
    row.tail_width = metrics.horizontalAdvance(row.cells.last().toString());
    layout.rows.push_back(std::move(row));
  }
  return layout;
}

[[nodiscard]] double rowHeight(const QFontMetricsF& metrics) {
  return std::max(metrics.height(), metrics.lineSpacing());
}

class TrackerTextEngine final : public QwtTextEngine {
 public:
  double heightForWidth(const QFont& font, int flags, const QString& text, double /*width*/) const override {
    return textSize(font, flags, text).height();
  }

  QSizeF textSize(const QFont& font, int /*flags*/, const QString& text) const override {
    const QFontMetricsF metrics(font);
    const Layout layout = layOut(text, metrics);
    double width = 0.0;
    for (const Row& row : layout.rows) {
      width = std::max(width, layout.widthOf(row));
    }
    return {std::ceil(width), std::ceil(static_cast<double>(layout.rows.size()) * rowHeight(metrics))};
  }

  bool mightRender(const QString& /*text*/) const override {
    return false;  // never picked for QwtText::AutoText
  }

  void textMargins(
      const QFont& /*font*/, const QString& /*text*/, double& left, double& right, double& top,
      double& bottom) const override {
    left = right = top = bottom = 0.0;
  }

  void draw(QPainter* painter, const QRectF& rect, int flags, const QString& text) const override {
    const QFontMetricsF metrics(painter->font());
    const Layout layout = layOut(text, metrics);
    const double row_height = rowHeight(metrics);
    double y = rect.top();
    if ((flags & (Qt::AlignBottom | Qt::AlignVCenter)) != 0) {
      const double slack = rect.height() - std::ceil(static_cast<double>(layout.rows.size()) * row_height);
      y += (flags & Qt::AlignBottom) != 0 ? slack : slack / 2.0;
    }
    painter->save();
    for (const Row& row : layout.rows) {
      double x = rect.left();
      if ((flags & (Qt::AlignRight | Qt::AlignHCenter)) != 0) {
        const double slack = rect.width() - layout.widthOf(row);
        x += (flags & Qt::AlignRight) != 0 ? slack : slack / 2.0;
      }
      painter->setPen(QColor::fromString(row.ink));
      const double baseline = y + metrics.ascent();
      for (qsizetype column = 0; column + 1 < row.cells.size(); ++column) {
        const QString cell = row.cells[column].toString();
        x += layout.column_widths[static_cast<size_t>(column)];
        painter->drawText(QPointF(x - metrics.horizontalAdvance(cell), baseline), cell);
      }
      painter->drawText(QPointF(x, baseline), row.cells.last().toString());
      y += row_height;
    }
    painter->restore();
  }
};

// Registered with Qwt on first use; Qwt owns the engine.
[[nodiscard]] QwtText::TextFormat valueBoxFormat() {
  static const QwtText::TextFormat format = [] {
    const auto value = static_cast<QwtText::TextFormat>(QwtText::OtherFormat + 1);
    QwtText::setTextEngine(value, new TrackerTextEngine());
    return value;
  }();
  return format;
}

}  // namespace

QwtText valueBoxLabel(const std::vector<ValueBoxRow>& rows) {
  QString text;
  for (const ValueBoxRow& row : rows) {
    if (!text.isEmpty()) {
      text += kRowSeparator;
    }
    text += row.ink.name(QColor::HexRgb);
    text += QString(row.text).replace(kRowSeparator, u' ');  // kValueBoxCellSeparator stays: it marks the cells
  }
  return QwtText(text, valueBoxFormat());
}

}  // namespace PJ
