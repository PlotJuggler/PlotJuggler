// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/Search.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QStyle>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgButton.h"

namespace PJ {
namespace {

// The magnifying glass and the clear "x", recolored to the active theme by SvgButton; `search_light` and
// `close_windows_light` are the light-authored masters, which loadSvg flips for the dark theme.
constexpr const char* kSearchIcon = ":/resources/svg/search_light.svg";
constexpr const char* kClearIcon = ":/resources/svg/close_windows_light.svg";

QString variantToken(Search::Variant variant) {
  return variant == Search::Variant::kStandalone ? QStringLiteral("standalone") : QString();
}

}  // namespace

Search::Search(QWidget* parent) : QWidget(parent) {
  // A QWidget subclass ignores a QSS background unless told to paint one; the
  // fill behind the glyph + field comes from the `PJ--Search` rule.
  setAttribute(Qt::WA_StyledBackground, true);
  setProperty("variant", variantToken(variant_));

  layout_ = new QHBoxLayout(this);
  layout_->setContentsMargins(
      theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
      theme::space(theme::Space::None));
  // No gap between the glyph, the field and the clear button, so the shared background reads as one control.
  layout_->setSpacing(theme::space(theme::Space::None));

  // Non-interactive glyph: mouse-transparent so a click lands in the field, not
  // the glyph, while SvgButton still auto-retints it on a theme change.
  glyph_ = new SvgButton(QString::fromLatin1(kSearchIcon), SvgButton::Size::kSmaller, this);
  glyph_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  glyph_->setFocusPolicy(Qt::NoFocus);
  layout_->addWidget(glyph_, 0, Qt::AlignVCenter);

  line_edit_ = new QLineEdit(this);
  line_edit_->setPlaceholderText(tr("Filter..."));
  // Flush the leading text against the glyph; the flat, borderless look comes
  // from the `PJ--Search QLineEdit` QSS rule, so no per-instance styling here.
  line_edit_->setTextMargins(
      theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
      theme::space(theme::Space::None));
  line_edit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  layout_->addWidget(line_edit_, 1);

  // Clear affordance: interactive (unlike the glyph), themed to match the dialog
  // chrome, and hidden until the field holds text.
  clear_button_ = new SvgButton(QString::fromLatin1(kClearIcon), SvgButton::Size::kSmaller, this);
  clear_button_->setVisible(false);
  layout_->addWidget(clear_button_, 0, Qt::AlignVCenter);

  connect(line_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
    clear_button_->setVisible(!text.isEmpty());
    emit textChanged(text);
  });
  connect(line_edit_, &QLineEdit::returnPressed, this, &Search::returnPressed);
  connect(clear_button_, &SvgButton::clicked, line_edit_, &QLineEdit::clear);

  // Seed the border-state properties so the QSS selectors have a value to match.
  setProperty("pjHovered", false);
  setProperty("pjFocused", false);
  // The frame and children are watched for hover, and the field for focus, so the border treatment covers the whole
  // control (the glyph is mouse-transparent, so its hover surfaces on the frame, hence `this` is watched too).
  installEventFilter(this);
  line_edit_->installEventFilter(this);
  clear_button_->installEventFilter(this);

  setChromeMetrics(chrome_metrics_);
}

bool Search::eventFilter(QObject* watched, QEvent* event) {
  switch (event->type()) {
    case QEvent::Enter:
      hovered_objects_.insert(watched);
      setStateProperty("pjHovered", !hovered_objects_.isEmpty());
      break;
    case QEvent::Leave:
      hovered_objects_.remove(watched);
      setStateProperty("pjHovered", !hovered_objects_.isEmpty());
      break;
    case QEvent::FocusIn:
      if (watched == line_edit_) {
        setStateProperty("pjFocused", true);
      }
      break;
    case QEvent::FocusOut:
      if (watched == line_edit_) {
        setStateProperty("pjFocused", false);
      }
      break;
    default:
      break;
  }
  return QWidget::eventFilter(watched, event);
}

void Search::setStateProperty(const char* name, bool on) {
  if (property(name).toBool() == on) {
    return;
  }
  setProperty(name, on);
  style()->unpolish(this);
  style()->polish(this);
}

Search::~Search() = default;

void Search::setPlaceholder(const QString& text) {
  line_edit_->setPlaceholderText(text);
}

QString Search::placeholder() const {
  return line_edit_->placeholderText();
}

QString Search::text() const {
  return line_edit_->text();
}

void Search::setText(const QString& text) {
  line_edit_->setText(text);
}

void Search::clear() {
  line_edit_->clear();
}

void Search::setFieldObjectName(const QString& name) {
  line_edit_->setObjectName(name);
}

QString Search::fieldObjectName() const {
  return line_edit_->objectName();
}

void Search::setVariant(Variant variant) {
  if (variant_ == variant) {
    return;
  }
  variant_ = variant;
  setProperty("variant", variantToken(variant_));
  // The stylesheet is re-run, so the new `[variant=...]` fill (frame) and ink (inner field) take effect.
  style()->unpolish(this);
  style()->polish(this);
  style()->unpolish(line_edit_);
  style()->polish(line_edit_);
}

void Search::setChromeMetrics(const ChromeMetrics& metrics) {
  chrome_metrics_ = metrics;
  const int button_extent = metrics.icon_size + metrics.icon_padding;
  setFixedHeight(button_extent);
  glyph_->setExtent(button_extent, metrics.icon_size);
  clear_button_->setExtent(button_extent, metrics.icon_size);
}

}  // namespace PJ
