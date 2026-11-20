// SPDX-License-Identifier: MPL-2.0
#include "pj_widgets/HeaderResizePolicy.h"

#include <QAbstractItemModel>
#include <QAbstractScrollArea>
#include <QEvent>
#include <QFontMetrics>
#include <QHeaderView>
#include <QStyle>
#include <algorithm>

namespace PJ {
namespace {

/// Breathing room on each side of a header label, so a column at its floor still shows the title with a gap and not
/// butted against the dividers.
constexpr int kLabelSideMargin = 6;

}  // namespace

HeaderResizePolicy* HeaderResizePolicy::install(QHeaderView* header, int fill_section, int minimum_section_size) {
  if (header == nullptr) {
    return nullptr;
  }
  // The policy is its own installation marker: it parents itself to the header,
  // so a second install on the same header finds and returns the first.
  if (auto* existing = header->findChild<HeaderResizePolicy*>({}, Qt::FindDirectChildrenOnly)) {
    return existing;
  }
  return new HeaderResizePolicy(header, fill_section, minimum_section_size);
}

HeaderResizePolicy::HeaderResizePolicy(QHeaderView* header, int fill_section, int minimum_section_size)
    : QObject(header), header_(header), fill_section_(fill_section), minimum_section_size_(minimum_section_size) {
  // Interactive on every section is what makes every divider live; a Stretch or
  // ResizeToContents section is auto-sized and refuses to be dragged.
  header_->setSectionResizeMode(QHeaderView::Interactive);
  header_->setStretchLastSection(false);
  header_->setCascadingSectionResizes(false);
  header_->setMinimumSectionSize(minimum_section_size_);

  connect(header_, &QHeaderView::sectionResized, this, &HeaderResizePolicy::onSectionResized);
  // A new column arrives Interactive by default only if the global mode is re-applied, and the fill section must
  // reclaim the slack either way.
  connect(header_, &QHeaderView::sectionCountChanged, this, [this](int old_count, int new_count) {
    header_->setSectionResizeMode(QHeaderView::Interactive);
    sizeNewSections(old_count, new_count);
    enforceMinimumWidths();
  });
  header_->installEventFilter(this);
  // A hidden header never receives a Resize event, so the owning view's viewport
  // is the only resize signal such a table gives.
  if (auto* view = qobject_cast<QAbstractScrollArea*>(header_->parentWidget())) {
    viewport_ = view->viewport();
    viewport_->installEventFilter(this);
  }
  enforceMinimumWidths();
}

bool HeaderResizePolicy::eventFilter(QObject* watched, QEvent* event) {
  if ((watched == header_ || (viewport_ != nullptr && watched == viewport_)) && event->type() == QEvent::Resize) {
    rebalance();
  }
  return QObject::eventFilter(watched, event);
}

int HeaderResizePolicy::availableWidth() const {
  // A hidden header keeps a stale width, so the viewport it would have spanned
  // is the authority then.
  if (header_->isHidden() && viewport_ != nullptr) {
    return viewport_->width();
  }
  return header_->width();
}

void HeaderResizePolicy::sizeNewSections(int old_count, int new_count) {
  const int fill = fillSection();
  if (fill < 0 || fill >= old_count) {
    // No established fill column to take width from; it will absorb the slack.
    return;
  }
  const bool was_adjusting = adjusting_;
  adjusting_ = true;
  for (int i = old_count; i < new_count && i < header_->count(); ++i) {
    if (header_->isSectionHidden(i)) {
      continue;
    }
    // The default width, but never more than the fill section can give up while staying at its own floor, so the new
    // column does not overflow the view.
    const int spare = std::max(0, header_->sectionSize(fill) - minimumWidthFor(fill));
    const int width = std::max(minimumWidthFor(i), std::min(header_->defaultSectionSize(), spare));
    header_->resizeSection(i, width);
  }
  adjusting_ = was_adjusting;
}

int HeaderResizePolicy::nextVisibleSection(int section) const {
  for (int i = section + 1; i < header_->count(); ++i) {
    if (!header_->isSectionHidden(i)) {
      return i;
    }
  }
  return -1;
}

int HeaderResizePolicy::fillSection(int except) const {
  // sectionSize() is 0 for a hidden section and for one being hidden, which Qt has zeroed but not yet flagged.
  if (fill_section_ >= 0 && fill_section_ < header_->count() && fill_section_ != except &&
      header_->sectionSize(fill_section_) > 0) {
    return fill_section_;
  }
  for (int i = 0; i < header_->count(); ++i) {
    if (i != except && header_->sectionSize(i) > 0) {
      return i;
    }
  }
  return -1;
}

int HeaderResizePolicy::visibleWidthExcept(int section) const {
  int width = 0;
  for (int i = 0; i < header_->count(); ++i) {
    if (i != section && !header_->isSectionHidden(i)) {
      width += header_->sectionSize(i);
    }
  }
  return width;
}

int HeaderResizePolicy::minimumWidthFor(int section) const {
  const QAbstractItemModel* model = header_->model();
  if (model == nullptr) {
    return minimum_section_size_;
  }
  const QString label = model->headerData(section, header_->orientation(), Qt::DisplayRole).toString();
  if (label.isEmpty()) {
    return minimum_section_size_;
  }
  int width = QFontMetrics(header_->font()).horizontalAdvance(label) + (2 * kLabelSideMargin);
  if (header_->isSortIndicatorShown()) {
    // The arrow is laid out beside the text, so the label would be clipped by
    // exactly its width if the floor did not account for it.
    width += header_->style()->pixelMetric(QStyle::PM_HeaderMarkSize, nullptr, header_);
  }
  return std::max(minimum_section_size_, width);
}

void HeaderResizePolicy::setSectionWidth(int section, int width) {
  if (section < 0 || section >= header_->count()) {
    return;
  }
  const bool was_adjusting = adjusting_;
  adjusting_ = true;
  header_->resizeSection(section, std::max(width, minimumWidthFor(section)));
  adjusting_ = was_adjusting;
  rebalance();
}

void HeaderResizePolicy::enforceMinimumWidths() {
  const bool was_adjusting = adjusting_;
  adjusting_ = true;
  for (int i = 0; i < header_->count(); ++i) {
    if (header_->isSectionHidden(i)) {
      continue;
    }
    const int floor = minimumWidthFor(i);
    if (header_->sectionSize(i) < floor) {
      header_->resizeSection(i, floor);
    }
  }
  adjusting_ = was_adjusting;
  rebalance();
}

void HeaderResizePolicy::onSectionResized(int section, int old_size, int new_size) {
  if (adjusting_ || header_->isSectionHidden(section)) {
    return;
  }
  if (new_size == 0 || old_size == 0) {
    // QHeaderView::setSectionHidden (Qt 5 through 6.12) resizes a section to 0 just before flagging it hidden, and
    // from 0 to its stored width just after showing it. A caller sizing a visible section to 0 is read as a hide too.
    // Neither is a drag: traded with the neighbour, the hidden section would keep its label floor as a gap and a shown
    // last section would snap back to 0. The fill section absorbs both instead; a shown fill section takes its stored
    // width back from the section that stood in for it, exact only when the viewport and the visible columns did not
    // change while it was hidden.
    if (new_size > 0) {
      const int width = std::max(new_size, minimumWidthFor(section));
      const int donor = fillSection(section);
      adjusting_ = true;
      header_->resizeSection(section, width);
      if (donor >= 0) {
        header_->resizeSection(donor, std::max(minimumWidthFor(donor), header_->sectionSize(donor) - width));
      }
      adjusting_ = false;
    }
    rebalance();
    return;
  }
  const int before = visibleWidthExcept(section) + old_size;
  const int available = availableWidth();
  if (before > available) {
    // Overflowing: no width to trade, so behave like a stock header (see the class doc). A narrowing stops where the
    // columns exactly fit; the rest of the drag continues under splitter semantics, with the divider under the cursor.
    const int width = std::max(new_size, old_size - (before - available));
    if (section == fillSection()) {
      user_fill_width_ = std::max(width, minimumWidthFor(section));
      user_fill_section_ = section;
    }
    setSectionWidth(section, width);
    return;
  }
  const int next = nextVisibleSection(section);
  if (next < 0) {
    // The last section has no neighbour to trade with, so widening it could
    // only overflow the header. Snap it back and leave the layout untouched.
    adjusting_ = true;
    header_->resizeSection(section, old_size);
    adjusting_ = false;
    return;
  }

  adjusting_ = true;
  int granted = 0;
  if (new_size > old_size) {
    // Widening takes width from the columns that follow, nearest first, each down to its own label floor. Draining only
    // the immediate neighbour would strand the divider as soon as that neighbour is at its floor, even with room
    // further along.
    int wanted = new_size - old_size;
    for (int i = next; i >= 0 && wanted > 0; i = nextVisibleSection(i)) {
      const int size = header_->sectionSize(i);
      const int spare = std::max(0, size - minimumWidthFor(i));
      const int taken = std::min(spare, wanted);
      if (taken > 0) {
        header_->resizeSection(i, size - taken);
        wanted -= taken;
        granted += taken;
      }
    }
  } else {
    // Narrowing stops at the dragged column's own floor; the freed width all
    // goes to the immediate neighbour, which mirrors a splitter handle.
    granted = std::max(new_size - old_size, minimumWidthFor(section) - old_size);
    header_->resizeSection(next, header_->sectionSize(next) - granted);
  }
  header_->resizeSection(section, old_size + granted);
  adjusting_ = false;
}

void HeaderResizePolicy::setFillSection(int fill_section) {
  if (fill_section_ == fill_section) {
    return;
  }
  fill_section_ = fill_section;
  rebalance();
}

void HeaderResizePolicy::rebalance() {
  const int fill = fillSection();
  if (fill < 0 || header_->count() == 0) {
    return;
  }
  const int slack = availableWidth() - visibleWidthExcept(fill);
  if (fill != user_fill_section_ || slack >= user_fill_width_) {
    user_fill_width_ = 0;  // another column is the fill, or the columns fit again
  }
  const int target = std::max({minimumWidthFor(fill), slack, user_fill_width_});
  if (header_->sectionSize(fill) == target) {
    return;
  }
  // Programmatic, not a user drag: routing it through onSectionResized would
  // steal the difference back out of the neighbour and cascade indefinitely.
  const bool was_adjusting = adjusting_;
  adjusting_ = true;
  header_->resizeSection(fill, target);
  adjusting_ = was_adjusting;
}

}  // namespace PJ
