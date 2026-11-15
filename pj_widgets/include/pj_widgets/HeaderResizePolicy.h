// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QObject>

class QHeaderView;
class QWidget;

namespace PJ {

/**
 * Splitter-style column resizing for any QHeaderView.
 *
 * Qt's stock header grows the whole header when a divider is dragged, which
 * either strands dead space to the right of the last column or pushes the table
 * into a horizontal scrollbar. The usual workaround — marking one column
 * `Stretch` — is worse: a `Stretch` section silently absorbs the entire delta,
 * so the divider under the cursor does not move at all and the boundary to its
 * *left* slides instead, and the divider adjacent to the stretched section
 * cannot be dragged at all.
 *
 * This policy makes a header behave like a QSplitter: the dragged divider
 * follows the cursor, the immediately following column gives or takes exactly
 * that much width, no other column moves, and the sections always sum to the
 * header's width. Every section stays `Interactive`, so every divider is live.
 *
 * One section is nominated as the *fill* section (the wide "name" column by
 * convention). It absorbs the slack whenever the header itself is resized or a
 * column is shown/hidden, which keeps the table exactly viewport-wide without
 * making the section non-interactive. While the header is hidden, the owning
 * view's viewport width stands in for the header width and its resizes drive
 * the rebalance.
 *
 * A header whose columns cannot fit the view even at their label floors (a wide
 * data grid) has no width to trade, so splitter semantics would freeze every
 * divider. There the policy falls back to the stock behaviour: a drag resizes
 * the dragged column alone (still never under its label) and the horizontal
 * scrollbar absorbs the change. A width the user gives the fill section in
 * that state is kept until the columns fit the view again.
 *
 * Install once per header; a second install on the same header is a no-op. The
 * policy owns itself as a child of the header and dies with it.
 */
class HeaderResizePolicy : public QObject {
  Q_OBJECT

 public:
  /**
   * Applies splitter semantics to @p header.
   *
   * Sets every section `Interactive`, clears `stretchLastSection`, and enforces
   * @p minimum_section_size as the floor a neighbour may be squeezed to. Safe
   * to call before the header has any sections: the policy configures whatever
   * arrives later.
   *
   * @param fill_section  logical index that absorbs slack on header resize.
   *                      Clamped into range once sections exist.
   *
   * Returns the installed policy, or the existing one if @p header already has
   * it. Never returns nullptr for a non-null @p header.
   */
  static HeaderResizePolicy* install(QHeaderView* header, int fill_section = 0, int minimum_section_size = 20);

  /**
   * Sets @p section's width without redistributing it.
   *
   * Seeding widths with QHeaderView::resizeSection would be read back as a user
   * drag and taken out of the following column, which starves the rightmost
   * columns one seed at a time. Callers laying out initial widths must use this
   * instead. The width is raised to the section's label minimum, and the fill
   * section reclaims the slack afterwards.
   */
  void setSectionWidth(int section, int width);

  /**
   * Widens any section currently narrower than its label needs.
   *
   * Runs on install and whenever the column count changes, so a table whose
   * .ui authored narrow defaults still opens readable.
   */
  void enforceMinimumWidths();

  /**
   * Re-applies the fill section's width so the sections sum to the header
   * width. Call after populating a table whose columns were sized externally;
   * header resizes and column show/hide already trigger it.
   */
  void rebalance();

  /**
   * Re-nominates the section that absorbs slack and rebalances immediately.
   *
   * Needed because install() is idempotent: a later caller that knows better
   * which column should be the wide one (e.g. a table whose first column turns
   * out to hold radio buttons) cannot express that through install().
   */
  void setFillSection(int fill_section);

  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  HeaderResizePolicy(QHeaderView* header, int fill_section, int minimum_section_size);

  /// Redistributes a user drag of @p section onto the next visible section.
  void onSectionResized(int section, int old_size, int new_size);

  /**
   * Narrowest @p section may become: enough for its header label plus a margin
   * on each side, never below the absolute floor passed to install().
   *
   * This is what stops a column being squashed until its title is unreadable.
   * QHeaderView::setMinimumSectionSize is a single value for the whole header,
   * so a per-label floor has to be enforced here instead.
   */
  [[nodiscard]] int minimumWidthFor(int section) const;

  /// Width the sections must sum to: the header's, or the owning view's viewport
  /// while the header is hidden (a hidden header never gets resized).
  [[nodiscard]] int availableWidth() const;

  /// Gives sections [old_count, new_count) a width the fill section can afford.
  void sizeNewSections(int old_count, int new_count);

  /// Next visible section after @p section, or -1 when @p section is the last.
  [[nodiscard]] int nextVisibleSection(int section) const;

  /// First visible section, preferring fill_section_, or -1 when all are hidden.
  [[nodiscard]] int fillSection() const;

  /// Summed width of every visible section except @p section.
  [[nodiscard]] int visibleWidthExcept(int section) const;

  QHeaderView* header_;
  /// Owning view viewport, watched so a hidden header still rebalances; null without a view.
  QWidget* viewport_ = nullptr;
  int fill_section_;
  int minimum_section_size_;
  /// Guards the programmatic resizes this class issues from re-entering
  /// onSectionResized as if they were user drags.
  bool adjusting_ = false;
  /// Width the user dragged the fill section to while the columns overflowed
  /// the view; rebalance() keeps it instead of shrinking the section back to
  /// its floor. 0 = none. Cleared once the columns fit the view again.
  int user_fill_width_ = 0;
  /// The section user_fill_width_ belongs to; the pin is dropped when another
  /// section becomes the fill (setFillSection, or the fill being hidden).
  int user_fill_section_ = -1;
};

}  // namespace PJ
