// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "TrackerTextEngine.h"

#include <qwt_text_engine.h>

#include <QFontMetricsF>
#include <QPainter>
#include <QStringView>
#include <algorithm>
#include <cmath>

namespace PJ {

namespace {

// A row is its ink as "#rrggbb" followed by its text; rows are separated by a line feed.
constexpr qsizetype kInkChars = 7;
constexpr QChar kRowSeparator = u'\n';

template <typename Visit>
void forEachRow(const QString& text, Visit&& visit) {
  for (const QStringView row : QStringView(text).split(kRowSeparator)) {
    if (row.size() >= kInkChars) {
      visit(row.first(kInkChars), row.sliced(kInkChars));
    }
  }
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
    double width = 0.0;
    int rows = 0;
    forEachRow(text, [&](QStringView /*ink*/, QStringView row) {
      width = std::max(width, metrics.horizontalAdvance(row.toString()));
      ++rows;
    });
    return {std::ceil(width), std::ceil(rows * rowHeight(metrics))};
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
    const double row_height = rowHeight(metrics);
    double y = rect.top();
    if ((flags & (Qt::AlignBottom | Qt::AlignVCenter)) != 0) {
      const double slack = rect.height() - textSize(painter->font(), flags, text).height();
      y += (flags & Qt::AlignBottom) != 0 ? slack : slack / 2.0;
    }
    painter->save();
    forEachRow(text, [&](QStringView ink, QStringView row) {
      const QString row_text = row.toString();
      double x = rect.left();
      if ((flags & (Qt::AlignRight | Qt::AlignHCenter)) != 0) {
        const double slack = rect.width() - metrics.horizontalAdvance(row_text);
        x += (flags & Qt::AlignRight) != 0 ? slack : slack / 2.0;
      }
      painter->setPen(QColor::fromString(ink));
      painter->drawText(QPointF(x, y + metrics.ascent()), row_text);
      y += row_height;
    });
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
    text += QString(row.text).replace(kRowSeparator, u' ');
  }
  return QwtText(text, valueBoxFormat());
}

}  // namespace PJ
