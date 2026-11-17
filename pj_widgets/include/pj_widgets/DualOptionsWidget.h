// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QString>
#include <QStringList>
#include <QWidget>

class QVariantAnimation;

namespace PJ {

// A segmented control: a joined strip of pill segments that replaces an exclusive group of QRadioButtons in config
// panels. Clicking a segment selects it; the selected segment is a raised neutral chip that slides to the new position,
// the others show the input background. Any number of segments >= 2 is supported; the two-option constructors are the
// common-case convenience.
//
// Orientation is configurable (default Horizontal): a horizontal strip lays segments left to right and the chip slides
// sideways, a vertical one stacks them top to bottom and the chip slides up and down. The host radio-group adapter
// picks it from the source radios' layout axis.
//
//   auto* w = new DualOptionsWidget("Frame", "Arrow");
//   auto* m = new DualOptionsWidget(QStringList{"Contains", "Wildcard", "RegExp"});
//   w->setSelectedIndex(0);                           // 0 = first (left/top)
//   w->setOrientation(Qt::Vertical);                  // stack + slide up/down
//   connect(w, &DualOptionsWidget::selectionChanged, [](int i){ ... });
//
// Colors come from the QSS through qproperties:
//   PJ--DualOptionsWidget {
//     qproperty-accentColor:       ${border_checked};
//     qproperty-borderColor:       ${border_default};
//     qproperty-baseFillColor:     ${input_background};
//     qproperty-selectedFillColor: ${item_selection_background};
//     qproperty-textColor:         ${default_text};
//   }
class DualOptionsWidget : public QWidget {
  Q_OBJECT
  Q_PROPERTY(int selectedIndex READ selectedIndex WRITE setSelectedIndex NOTIFY selectionChanged)
  Q_PROPERTY(Qt::Orientation orientation READ orientation WRITE setOrientation)
  Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor)
  Q_PROPERTY(QColor borderColor READ borderColor WRITE setBorderColor)
  Q_PROPERTY(QColor baseFillColor READ baseFillColor WRITE setBaseFillColor)
  Q_PROPERTY(QColor selectedFillColor READ selectedFillColor WRITE setSelectedFillColor)
  Q_PROPERTY(QColor textColor READ textColor WRITE setTextColor)

 public:
  explicit DualOptionsWidget(QWidget* parent = nullptr);
  explicit DualOptionsWidget(const QString& opt0, const QString& opt1, QWidget* parent = nullptr);
  // Applies `options` through setOptions, so a list with fewer than 2 entries keeps the "Option A" / "Option B"
  // defaults.
  explicit DualOptionsWidget(const QStringList& options, QWidget* parent = nullptr);

  // Replaces all labels at once. Lists with fewer than 2 entries are ignored; a selection beyond the new list clamps to
  // the last segment (emitting selectionChanged).
  void setOptions(const QStringList& options);
  void setOptions(const QString& opt0, const QString& opt1);

  [[nodiscard]] int optionCount() const {
    return static_cast<int>(options_.size());
  }
  [[nodiscard]] int selectedIndex() const {
    return selected_;
  }
  [[nodiscard]] bool isFirstSelected() const {
    return selected_ == 0;
  }
  [[nodiscard]] bool isSecondSelected() const {
    return selected_ == 1;
  }

  [[nodiscard]] Qt::Orientation orientation() const {
    return orientation_;
  }
  // Horizontal (default) lays segments left to right; Vertical stacks them and slides the chip up and down. Swaps the
  // size policy and geometry.
  void setOrientation(Qt::Orientation orientation);

  [[nodiscard]] QColor accentColor() const {
    return accent_color_;
  }
  void setAccentColor(const QColor& color);

  [[nodiscard]] QColor borderColor() const {
    return border_color_;
  }
  void setBorderColor(const QColor& color);

  [[nodiscard]] QColor baseFillColor() const {
    return base_fill_color_;
  }
  void setBaseFillColor(const QColor& color);

  [[nodiscard]] QColor selectedFillColor() const {
    return selected_fill_color_;
  }
  void setSelectedFillColor(const QColor& color);

  [[nodiscard]] QColor textColor() const {
    return text_color_;
  }
  void setTextColor(const QColor& color);

  [[nodiscard]] QSize sizeHint() const override;
  [[nodiscard]] QSize minimumSizeHint() const override;

  // Per-segment enable. A disabled segment renders greyed and cannot be selected: click, keyboard navigation and
  // setSelectedIndex all skip it. Out-of-range indices are ignored; a segment with no explicit state is enabled.
  void setSegmentEnabled(int index, bool enabled);
  [[nodiscard]] bool isSegmentEnabled(int index) const;

 public slots:
  // Selects the segment at index (0 = leftmost). Out-of-range or disabled indices are ignored, as is the current one;
  // emits selectionChanged when the value changes.
  void setSelectedIndex(int index);

 signals:
  void selectionChanged(int index);

 protected:
  bool event(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  void animateSelectedIndex(int index);
  // The first enabled segment reached by stepping `step` from the current selection. `wrap` rolls past the ends (Space
  // cycling) instead of stopping (arrow keys). The current selection when no other segment is reachable.
  [[nodiscard]] int nextEnabledIndex(int step, bool wrap) const;

  QStringList options_{"Option A", "Option B"};
  // Per-index enable flags, always sized to options_.
  QList<bool> segment_enabled_{true, true};
  Qt::Orientation orientation_ = Qt::Horizontal;
  int selected_ = 0;
  qreal visual_selection_ = 0.0;
  QVariantAnimation* selection_animation_ = nullptr;
  QColor accent_color_;
  QColor border_color_;
  QColor base_fill_color_;
  QColor selected_fill_color_;
  QColor text_color_;
};

}  // namespace PJ
