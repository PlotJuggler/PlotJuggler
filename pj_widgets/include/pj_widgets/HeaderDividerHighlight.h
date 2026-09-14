// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QObject>

class QHeaderView;

namespace PJ {

/// The overlay line itself; defined in the implementation because nothing
/// outside it needs the type.
class HeaderDividerLine;

/**
 * Lights a table header's column divider while it is grabbable, matching the
 * way a QSplitter handle reacts: the Highlight variant's Hovered colour under
 * the cursor, its Pressed colour while being dragged.
 *
 * This cannot be a stylesheet rule. `QHeaderView::section:hover` fires when the
 * pointer is anywhere over a section, not over its ~4-px resize grip, and QSS
 * has no selector for "this section is currently being resized" at all. So the
 * highlight is a thin overlay widget positioned over the gripped boundary.
 *
 * Only boundaries that actually resize are lit: the last section's right edge
 * is skipped, because there is no following column to trade width with (see
 * HeaderResizePolicy) and highlighting it would advertise a drag that does
 * nothing.
 *
 * Install once per header; a second install on the same header is a no-op. The
 * object parents itself to the header and dies with it.
 */
class HeaderDividerHighlight : public QObject {
  Q_OBJECT

 public:
  /// Returns the installed highlighter, or the existing one for @p header.
  /// Never returns nullptr for a non-null @p header.
  static HeaderDividerHighlight* install(QHeaderView* header);

  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  explicit HeaderDividerHighlight(QHeaderView* header);

  /// Logical section whose right edge is within grab range of @p viewport_x,
  /// or -1 when the pointer is not over a resizable boundary.
  [[nodiscard]] int dividerAt(int viewport_x) const;

  /// Whether @p section has any visible section after it.
  [[nodiscard]] bool hasFollowingSection(int section) const;

  /// Moves the overlay onto @p section's right edge and tints it for the
  /// pressed or hovered state. A negative @p section hides it.
  void showDivider(int section, bool pressed);

  QHeaderView* header_;
  /// The header as a plain QObject*, recorded while it was alive. eventFilter()
  /// checks it with qobject_cast before calling header_->viewport(): the viewport
  /// is deleted while the header is already down to its QWidget base, and a
  /// QHeaderView member call at that point is undefined behavior.
  QObject* header_object_;
  /// Child of the header's viewport, so its x maps straight onto section
  /// viewport positions. Nulled on destruction (a viewport swap would take it
  /// with it), so it is only ever dereferenced while alive.
  HeaderDividerLine* overlay_ = nullptr;
  int hovered_section_ = -1;
  /// Section whose edge is currently being dragged, or -1. Held separately from
  /// the hovered one because the pointer routinely leaves the grip during a
  /// drag while the resize is still in progress.
  int pressed_section_ = -1;
};

}  // namespace PJ
