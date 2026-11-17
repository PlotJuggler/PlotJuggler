// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QObject>

class QHeaderView;
class QWidget;

namespace PJ {

/// Splitter-style column resizing for any QHeaderView.
///
/// Qt's stock header grows the whole header when a divider is dragged, which strands dead space to the right of the
/// last column or pushes the table into a horizontal scrollbar. The usual workaround, marking one column `Stretch`, is
/// worse: a `Stretch` section absorbs the entire delta, so the divider under the cursor does not move, the boundary to
/// its left slides instead, and the divider next to the stretched section cannot be dragged at all.
///
/// This policy makes a header behave like a QSplitter: the dragged divider follows the cursor, the next column gives or
/// takes exactly that much width, no other column moves, and the sections always sum to the header's width. Every
/// section stays `Interactive`, so every divider is live.
///
/// One section is nominated as the fill section (the wide "name" column by convention). It absorbs the slack when the
/// header is resized or a column is shown or hidden, which keeps the table exactly viewport-wide without making the
/// section non-interactive. While the header is hidden, the owning view's viewport width stands in for the header width
/// and its resizes drive the rebalance.
///
/// A header whose columns cannot fit the view even at their label floors (a wide data grid) has no width to trade, so
/// splitter semantics would freeze every divider. There the policy falls back to the stock behaviour: a drag resizes
/// the dragged column alone (still never under its label) and the horizontal scrollbar absorbs the change. A width the
/// user gives the fill section in that state is kept until the columns fit the view again.
///
/// Install once per header; a second install on the same header is a no-op. The policy owns itself as a child of the
/// header and dies with it.
class HeaderResizePolicy : public QObject {
  Q_OBJECT

 public:
  /// Applies splitter semantics to @p header: every section `Interactive`, `stretchLastSection` cleared, and @p
  /// minimum_section_size enforced as the floor a neighbour may be squeezed to. Safe before the header has any
  /// sections: the policy configures whatever arrives later.
  ///
  /// @param fill_section  logical index that absorbs slack on header resize; clamped into range once sections exist.
  ///
  /// Returns the installed policy, or the existing one if @p header already has it; never nullptr for a non-null @p
  /// header.
  static HeaderResizePolicy* install(QHeaderView* header, int fill_section = 0, int minimum_section_size = 20);

  /// Sets @p section's width without redistributing it. QHeaderView::resizeSection for seeding widths would be read
  /// back as a user drag and taken out of the following column, starving the rightmost columns one seed at a time:
  /// callers laying out initial widths use this. The width is raised to the section's label minimum, and the fill
  /// section reclaims the slack afterwards.
  void setSectionWidth(int section, int width);

  /// Widens any section narrower than its label needs. Runs on install and whenever the column count changes, so a
  /// table whose .ui authored narrow defaults still opens readable.
  void enforceMinimumWidths();

  /// Re-applies the fill section's width so the sections sum to the header width. Call after populating a table whose
  /// columns were sized externally; header resizes and column show and hide already trigger it.
  void rebalance();

  /// Re-nominates the section that absorbs slack and rebalances at once. install() is idempotent, so a later caller
  /// that knows better which column should be wide (a table whose first column turns out to hold radio buttons) cannot
  /// say so through it.
  void setFillSection(int fill_section);

  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  HeaderResizePolicy(QHeaderView* header, int fill_section, int minimum_section_size);

  /// Redistributes a user drag of @p section onto the next visible section.
  void onSectionResized(int section, int old_size, int new_size);

  /// The narrowest @p section may become: its header label plus a margin on each side, never below the absolute floor
  /// passed to install(). It stops a column being squashed until its title is unreadable;
  /// QHeaderView::setMinimumSectionSize is one value for the whole header, so a per-label floor is enforced here.
  [[nodiscard]] int minimumWidthFor(int section) const;

  /// The width the sections must sum to: the header's, or the owning view's viewport while the header is hidden (a
  /// hidden header never gets resized).
  [[nodiscard]] int availableWidth() const;

  /// Gives sections [old_count, new_count) a width the fill section can afford.
  void sizeNewSections(int old_count, int new_count);

  /// The next visible section after @p section, or -1 when it is the last.
  [[nodiscard]] int nextVisibleSection(int section) const;

  /// The first visible section, preferring fill_section_, or -1 when all are hidden.
  [[nodiscard]] int fillSection() const;

  /// Summed width of every visible section except @p section.
  [[nodiscard]] int visibleWidthExcept(int section) const;

  QHeaderView* header_;
  /// The owning view's viewport, watched so a hidden header still rebalances; null without a view.
  QWidget* viewport_ = nullptr;
  int fill_section_;
  int minimum_section_size_;
  /// Keeps the programmatic resizes this class issues from re-entering onSectionResized as user drags.
  bool adjusting_ = false;
  /// The width the user dragged the fill section to while the columns overflowed the view: rebalance() keeps it instead
  /// of shrinking the section back to its floor. 0 = none; cleared once the columns fit the view again.
  int user_fill_width_ = 0;
  /// The section user_fill_width_ belongs to; the pin is dropped when another section becomes the fill (setFillSection,
  /// or the fill being hidden).
  int user_fill_section_ = -1;
};

}  // namespace PJ
