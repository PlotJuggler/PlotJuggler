// SPDX-License-Identifier: MPL-2.0
#include "pj_widgets/HeaderDividerHighlight.h"

#include <QEvent>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QWidget>
#include <cstdlib>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {
namespace {

/// Fallback grab half-width when the style reports no header grip margin.
constexpr int kFallbackGripMargin = 4;
/// The divider is a hairline, matching the `border-right` the header's
/// stylesheet paints on each section.
constexpr int kDividerWidth = 1;
/// Mirrors `QHeaderView::section { border-top: 1px }` in the app stylesheet.
/// That rule is painted by the section, so it lands *inside* the viewport and
/// the highlight has to start below it; the header's bottom border belongs to
/// the widget frame instead, so it falls outside the viewport on its own.
constexpr int kSectionTopBorder = 1;

theme::Theme frameworkTheme() {
  // Read the app-global palette, not the header's: under the app stylesheet
  // QStyleSheetStyle clobbers per-widget palettes, so a widget's own palette can
  // report the wrong theme.
  const QColor window = QGuiApplication::palette().color(QPalette::Window);
  return theme::themeFor(window.lightness() >= 128);
}

}  // namespace

/// The bare coloured line drawn over a gripped boundary. Transparent to the
/// mouse so it never steals the drag it is advertising.
class HeaderDividerLine : public QWidget {
 public:
  explicit HeaderDividerLine(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    hide();
  }

  void setColor(const QColor& color) {
    if (color_ != color) {
      color_ = color;
      update();
    }
  }

 protected:
  void paintEvent(QPaintEvent* /*event*/) override {
    QPainter painter(this);
    painter.fillRect(rect(), color_);
  }

 private:
  QColor color_;
};

HeaderDividerHighlight* HeaderDividerHighlight::install(QHeaderView* header) {
  if (header == nullptr) {
    return nullptr;
  }
  // The object is its own installation marker: it parents itself to the header,
  // so a second install finds and returns the first.
  if (auto* existing = header->findChild<HeaderDividerHighlight*>({}, Qt::FindDirectChildrenOnly)) {
    return existing;
  }
  return new HeaderDividerHighlight(header);
}

HeaderDividerHighlight::HeaderDividerHighlight(QHeaderView* header)
    : QObject(header), header_(header), header_object_(header) {
  overlay_ = new HeaderDividerLine(header_->viewport());
  connect(overlay_, &QObject::destroyed, this, [this]() { overlay_ = nullptr; });

  // QHeaderView is a scroll area: pointer events are delivered to its viewport,
  // not to the header itself. Hover events need the attribute set to be sent at
  // all, which the header's own setup may or may not have done already.
  header_->viewport()->setAttribute(Qt::WA_Hover, true);
  header_->viewport()->installEventFilter(this);

  // A live drag moves the boundary under the cursor, so the overlay has to
  // follow the section it is pinned to.
  connect(header_, &QHeaderView::sectionResized, this, [this](int section, int, int) {
    if (pressed_section_ >= 0 && section == pressed_section_) {
      showDivider(pressed_section_, true);
    }
  });
}

bool HeaderDividerHighlight::hasFollowingSection(int section) const {
  for (int i = section + 1; i < header_->count(); ++i) {
    if (!header_->isSectionHidden(i)) {
      return true;
    }
  }
  return false;
}

int HeaderDividerHighlight::dividerAt(int viewport_x) const {
  int margin = header_->style()->pixelMetric(QStyle::PM_HeaderGripMargin, nullptr, header_);
  if (margin <= 0) {
    margin = kFallbackGripMargin;
  }
  for (int i = 0; i < header_->count(); ++i) {
    if (header_->isSectionHidden(i) || !hasFollowingSection(i)) {
      continue;
    }
    const int edge = header_->sectionViewportPosition(i) + header_->sectionSize(i);
    if (std::abs(viewport_x - edge) <= margin) {
      return i;
    }
  }
  return -1;
}

void HeaderDividerHighlight::showDivider(int section, bool pressed) {
  if (overlay_ == nullptr) {
    return;
  }
  if (section < 0) {
    overlay_->hide();
    return;
  }
  const theme::State state = pressed ? theme::State::Pressed : theme::State::Hovered;
  overlay_->setColor(theme::interaction(theme::Variant::Highlight, state, frameworkTheme()));
  // The section's stylesheet border-right occupies the last pixel *inside* the
  // section, so the overlay sits there rather than on the following section, and
  // starts below the section's border-top so the band's top rule stays unbroken.
  const int edge = header_->sectionViewportPosition(section) + header_->sectionSize(section);
  const int height = header_->viewport()->height() - kSectionTopBorder;
  overlay_->setGeometry(edge - kDividerWidth, kSectionTopBorder, kDividerWidth, height);
  overlay_->raise();
  overlay_->show();
}

bool HeaderDividerHighlight::eventFilter(QObject* watched, QEvent* event) {
  if (qobject_cast<QHeaderView*>(header_object_) == nullptr || watched != header_->viewport()) {
    return QObject::eventFilter(watched, event);
  }

  switch (event->type()) {
    case QEvent::HoverMove:
    case QEvent::MouseMove: {
      if (pressed_section_ >= 0) {
        break;  // a drag owns the highlight until the button is released
      }
      const auto* hover = dynamic_cast<QHoverEvent*>(event);
      const auto* mouse = dynamic_cast<QMouseEvent*>(event);
      const int x = hover != nullptr ? static_cast<int>(hover->position().x())
                                     : (mouse != nullptr ? static_cast<int>(mouse->position().x()) : -1);
      hovered_section_ = x < 0 ? -1 : dividerAt(x);
      showDivider(hovered_section_, false);
      break;
    }
    case QEvent::MouseButtonPress: {
      const auto* mouse = dynamic_cast<QMouseEvent*>(event);
      if (mouse != nullptr && mouse->button() == Qt::LeftButton) {
        pressed_section_ = dividerAt(static_cast<int>(mouse->position().x()));
        if (pressed_section_ >= 0) {
          showDivider(pressed_section_, true);
        }
      }
      break;
    }
    case QEvent::MouseButtonRelease: {
      pressed_section_ = -1;
      const auto* mouse = dynamic_cast<QMouseEvent*>(event);
      hovered_section_ = mouse == nullptr ? -1 : dividerAt(static_cast<int>(mouse->position().x()));
      showDivider(hovered_section_, false);
      break;
    }
    case QEvent::HoverLeave:
    case QEvent::Leave:
      if (pressed_section_ < 0) {
        hovered_section_ = -1;
        showDivider(-1, false);
      }
      break;
    case QEvent::Resize:
      // Keep the line spanning the full header height as the view resizes.
      if (hovered_section_ >= 0 || pressed_section_ >= 0) {
        showDivider(pressed_section_ >= 0 ? pressed_section_ : hovered_section_, pressed_section_ >= 0);
      }
      break;
    default:
      break;
  }
  return QObject::eventFilter(watched, event);
}

}  // namespace PJ
