#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QEvent>
#include <QFontMetrics>
#include <QLabel>
#include <QResizeEvent>
#include <QShowEvent>
#include <QString>
#include <QStringList>
#include <QTextLayout>
#include <QTextOption>
#include <QWidget>
#include <algorithm>
#include <utility>

namespace PJ {

// QLabel variant that re-elides its text whenever its width changes and
// can also be driven from the outside. Below a configurable minimum the
// label hides itself so its space is reclaimed by sibling layout items;
// when its parent re-expands, applyAvailableWidth() (called from
// whoever drives the row) brings it back. It only ever re-shows what it
// hid itself: a label that is merely not shown yet (still parentless,
// or hidden by its owner) is left alone, since showing a parentless
// widget makes it a top-level window (on Wayland, one whose activation
// leaves GNOME's launch-busy cursor spinning).
//
// The dual interface (own resizeEvent + applyAvailableWidth) is needed
// because hidden widgets don't receive resize events: once we go to
// 0 px we have to be told from the outside to come back.
//
// setMaxLineCount(n) switches to an n-line summary: the text word-wraps
// onto up to n lines and any overflow elides into the last line, so the
// label never shows a partially clipped line (QLabel has no multi-line
// elide of its own — its word-wrap happily lays out lines the widget
// rect then cuts through).
class ElidingLabel : public QLabel {
 public:
  explicit ElidingLabel(QWidget* parent = nullptr) : QLabel(parent) {
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  }

  void setFullText(QString text) {
    full_text_ = std::move(text);
    // Re-elide using current width without waiting for a resize tick.
    applyAvailableWidth(width());
  }

  [[nodiscard]] QString fullText() const {
    return full_text_;
  }

  void setHideBelowWidth(int px) {
    hide_below_ = px;
    applyAvailableWidth(width());
  }

  void setElideMode(Qt::TextElideMode mode) {
    elide_mode_ = mode;
    applyAvailableWidth(width());
  }

  // Word-wrap onto up to `lines` lines, eliding overflow into the last
  // one. 1 (the default) is the classic single-line elide. QLabel's own
  // wordWrap must stay OFF in both modes — the wrapping happens here so
  // the line budget actually holds.
  void setMaxLineCount(int lines) {
    max_line_count_ = std::max(1, lines);
    applyAvailableWidth(width());
  }

  // Externally-driven update: parent passes the budget it has carved
  // out for the label. This is the path that can RE-SHOW a label the
  // width threshold hid (which wouldn't otherwise see resize events).
  void applyAvailableWidth(int width_px) {
    last_width_px_ = width_px;
    if (width_px < hide_below_) {
      if (!isHidden()) {
        hide();
        hidden_for_width_ = true;
      }
      return;
    }
    if (hidden_for_width_ && parentWidget() != nullptr) {
      show();
    }
    const QFontMetrics metrics(font());
    if (max_line_count_ <= 1) {
      setText(metrics.elidedText(full_text_, elide_mode_, width_px));
    } else {
      setText(wrappedElidedText(metrics, width_px));
    }
  }

 protected:
  // Layout-driven update: when sibling widgets resize naturally the
  // label receives a resize event and re-elides without needing the
  // parent to call applyAvailableWidth.
  void resizeEvent(QResizeEvent* event) override {
    QLabel::resizeEvent(event);
    applyAvailableWidth(width());
  }

  // Any show, ours or the owner's, ends a threshold hide: a later owner
  // hide() is the owner's decision and must stick.
  void showEvent(QShowEvent* event) override {
    QLabel::showEvent(event);
    hidden_for_width_ = false;
  }

  // A QSS font lands after construction (widget polish), which would
  // leave text elided with stale metrics; re-elide at the last known
  // budget — not width(), which may be wider than what an external
  // driver granted.
  void changeEvent(QEvent* event) override {
    QLabel::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
      applyAvailableWidth(last_width_px_ >= 0 ? last_width_px_ : width());
    }
  }

 private:
  // Lay out the (whitespace-normalized) text at width_px, keep the first
  // max_line_count_-1 lines verbatim, and elide everything left over into
  // the final line. WrapAtWordBoundaryOrAnywhere so an unbreakable token
  // (a URL) can't overflow the budget.
  [[nodiscard]] QString wrappedElidedText(const QFontMetrics& metrics, int width_px) const {
    const QString text = full_text_.simplified();
    QTextLayout layout(text, font());
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    QStringList lines;
    while (true) {
      QTextLine line = layout.createLine();
      if (!line.isValid()) {
        break;
      }
      line.setLineWidth(width_px);
      if (lines.size() == max_line_count_ - 1) {
        lines.append(metrics.elidedText(text.mid(line.textStart()), elide_mode_, width_px));
        break;
      }
      lines.append(text.mid(line.textStart(), line.textLength()));
    }
    layout.endLayout();
    return lines.join(u'\n');
  }

  QString full_text_;
  Qt::TextElideMode elide_mode_ = Qt::ElideRight;
  int hide_below_ = 16;
  int max_line_count_ = 1;
  int last_width_px_ = -1;
  // Set only when the width threshold hid the label: the one hide it may undo.
  bool hidden_for_width_ = false;
};

}  // namespace PJ
