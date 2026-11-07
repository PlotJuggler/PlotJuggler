// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/MessageBox.h"

#include <QCheckBox>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QRect>
#include <QShowEvent>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// Maps the C++ enum to the string the QSS selectors key off.
const char* roleToToken(MessageBox::ButtonRole role) {
  switch (role) {
    case MessageBox::kPrimaryRole:
      return "primary";
    case MessageBox::kNeutralRole:
      return "neutral";
    case MessageBox::kDestructiveRole:
      return "destructive";
    case MessageBox::kCancelRole:
      return "cancel";
  }
  return "neutral";
}

// Layout insets used to derive the per-line wrap budget for button labels.
// They mirror values set elsewhere — the card content margin in the ctor
// (root->setContentsMargins()), the 1px card border and the button's
// horizontal padding from the QSS rule for #pjMessageBoxButton. They only
// need to be *conservative*: under-estimating the budget
// wraps a hair early but never lets a line get clipped, so small drift in the
// QSS values is harmless.
constexpr auto kCardContentMargin = theme::Space::Section;
constexpr int kCardBorder = 1;
constexpr auto kButtonHPadding = theme::Space::Section;
constexpr int kWrapSlack = 4;  // extra safety against font-metric rounding

// Greedy word-wrap: insert '\n' so no single rendered line exceeds max_width.
// Returns text unchanged when it already fits on one line (the common case for
// short labels, which then render exactly as before). A single word wider than
// max_width is kept on its own line rather than dropped or split mid-word.
QString wrapLabelToWidth(const QString& text, const QFontMetrics& fm, int max_width) {
  if (max_width <= 0 || fm.horizontalAdvance(text) <= max_width) {
    return text;
  }
  const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  if (words.isEmpty()) {
    return text;
  }
  // Greedily pack words onto the current line; start a new line when the next
  // word would overflow. A single word wider than max_width still gets its own
  // line (it just can't be packed with anything).
  QStringList lines;
  QString line = words.first();
  for (int i = 1; i < words.size(); ++i) {
    const QString candidate = line + QLatin1Char(' ') + words[i];
    if (fm.horizontalAdvance(candidate) <= max_width) {
      line = candidate;
    } else {
      lines.append(line);
      line = words[i];
    }
  }
  lines.append(line);
  return lines.join(QLatin1Char('\n'));
}

}  // namespace

MessageBox::MessageBox(QWidget* parent) : QDialog(parent) {
  setObjectName(u"pjMessageBox"_s);

  // Frameless modal so the WM's title-bar chrome doesn't fight the in-body
  // heading. WA_StyledBackground lets the QDialog#pjMessageBox QSS rule
  // actually paint the dark/light surface and the 1-px border.
  setWindowFlag(Qt::FramelessWindowHint, true);
  setWindowFlag(Qt::Dialog, true);
  // ARGB surface so the corner pixels outside the inner card's rounded
  // background can be alpha=0. The card itself paints opaque, so text and
  // children never composite against a partly-transparent surface — only
  // the four corner pixels do, which is what gives the dialog its rounded
  // shape on compositors that ignore setMask (i.e. most Wayland setups).
  setAttribute(Qt::WA_TranslucentBackground, true);
  setModal(true);

  // Width contract. Word-wrapped QLabels report an arbitrarily small minimum
  // width (height grows via heightForWidth instead), so without these the
  // layout collapses to fit the narrowest widget and the body text wraps
  // into a thin strip. 320 reads comfortably for the short messages in
  // information()/warning()/critical(); 460 caps growth for verbose bodies
  // so a paragraph wraps onto a few lines rather than stretching across the
  // screen. Picked by eye against the info_dialog.png reference.
  setMinimumWidth(320);
  setMaximumWidth(460);

  // Outer layout: just hosts the card; no margins so the card fills the
  // window flush and its rounded corners are the dialog's only shape.
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(
      theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
      theme::space(theme::Space::None));
  outer->setSpacing(theme::space(theme::Space::None));

  auto* card = new QFrame(this);
  card->setObjectName(u"pjMessageBoxCard"_s);
  outer->addWidget(card);

  // Inner layout on the card. Uniform spacing throughout so every gap reads
  // the same: title→body, body→checkbox, body→buttons (when no checkbox),
  // checkbox→buttons — every adjacent visible pair uses Section spacing.
  // Earlier versions added a compact sub-section spacer above the buttons,
  // which made the spacing inconsistent when the checkbox was hidden.
  auto* root = new QVBoxLayout(card);
  root->setContentsMargins(
      theme::space(kCardContentMargin), theme::space(kCardContentMargin), theme::space(kCardContentMargin),
      theme::space(kCardContentMargin));
  root->setSpacing(theme::space(theme::Space::Section));

  title_label_ = new QLabel(card);
  title_label_->setObjectName(u"pjMessageBoxTitle"_s);
  title_label_->setWordWrap(true);
  root->addWidget(title_label_);

  body_label_ = new QLabel(card);
  body_label_->setObjectName(u"pjMessageBoxBody"_s);
  body_label_->setWordWrap(true);
  // Expanding width + heightForWidth so a wrapped body is laid out at the full
  // card width and allotted its full wrapped height. The explicit policy must
  // re-enable heightForWidth (the (horiz, vert) QSizePolicy ctor clears it),
  // otherwise the layout gives the label a single line and the text clips.
  QSizePolicy body_policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  body_policy.setHeightForWidth(true);
  body_label_->setSizePolicy(body_policy);
  root->addWidget(body_label_);

  dont_show_again_ = new QCheckBox(card);
  dont_show_again_->setObjectName(u"pjMessageBoxDontShowAgain"_s);
  dont_show_again_->setText(tr("Don't show again."));
  dont_show_again_->hide();  // opt-in via setShowDontShowAgain()
  root->addWidget(dont_show_again_);

  auto* btn_holder = new QVBoxLayout;
  btn_holder->setContentsMargins(
      theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
      theme::space(theme::Space::None));
  // Comfortable gap between stacked buttons — tighter than the Section body→button
  // gap so the buttons read as one grouped affordance. This value must render
  // identically in every dialog; showEvent() resizes the dialog to the exact
  // height its content needs so the layout can never compress this spacing
  // (see the comment there).
  btn_holder->setSpacing(theme::space(theme::Space::Comfortable));
  button_column_ = btn_holder;
  root->addLayout(btn_holder);
}

MessageBox::~MessageBox() = default;

void MessageBox::setTitle(const QString& title) {
  title_label_->setText(title);
  setWindowTitle(title);
}

void MessageBox::setText(const QString& text) {
  body_label_->setText(text);
}

void MessageBox::setShowDontShowAgain(bool show, const QString& label) {
  dont_show_again_->setVisible(show);
  if (!label.isEmpty()) {
    dont_show_again_->setText(label);
  }
}

bool MessageBox::dontShowAgainChecked() const {
  // isHidden(), not isVisible(): callers read this after the dialog closed,
  // when every child reports invisible.
  return !dont_show_again_->isHidden() && dont_show_again_->isChecked();
}

QPushButton* MessageBox::addButton(const QString& label, ButtonRole role) {
  return addButton(label, role, true);
}

QPushButton* MessageBox::addButton(const QString& label, ButtonRole role, bool close_on_click) {
  auto* btn = new QPushButton(label, this);
  btn->setObjectName(u"pjMessageBoxButton"_s);
  btn->setProperty("msgbox_role", QLatin1String(roleToToken(role)));
  // Disable autoDefault so Enter doesn't trigger a non-primary button just
  // because focus traversed onto it. The primary button still receives
  // default-button focus.
  btn->setAutoDefault(false);
  btn->setDefault(role == kPrimaryRole);

  const int index = static_cast<int>(buttons_.size());
  buttons_.append(btn);
  button_roles_.append(role);
  button_labels_.append(label);  // un-wrapped original; see rewrapButtonLabels()
  button_column_->addWidget(btn);

  QObject::connect(btn, &QPushButton::clicked, this, [this, index, close_on_click]() {
    clicked_index_ = index;
    if (close_on_click) {
      accept();
    }
  });
  return btn;
}

int MessageBox::clickedIndex() const {
  return clicked_index_;
}

void MessageBox::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape) {
    // Map Esc to the first CancelRole button if one is present, so callers
    // that supply a Cancel button get its index returned. Otherwise reject
    // with clicked_index_ = -1.
    for (int i = 0; i < button_roles_.size(); ++i) {
      if (button_roles_[i] == kCancelRole) {
        clicked_index_ = i;
        accept();
        return;
      }
    }
    reject();
    return;
  }
  QDialog::keyPressEvent(event);
}

bool MessageBox::event(QEvent* event) {
  // Wrap long button labels on Polish: by then QStyleSheetStyle has applied the
  // QSS font (so QFontMetrics are accurate), and Polish runs before the dialog
  // auto-sizes. Wrapping in showEvent() would be too late — the dialog would
  // already be sized for single-line buttons and clip the wrapped text.
  if (event->type() == QEvent::Polish) {
    const bool handled = QDialog::event(event);  // let the style apply the font first
    rewrapButtonLabels();
    return handled;
  }
  return QDialog::event(event);
}

void MessageBox::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);

  const bool first_show = !size_finalized_;
  size_finalized_ = true;

  // (1) Grow to fit the word-wrapped BODY on first show. Widths are resolved by
  // now (the dialog was sized to its hint and laid out before this event), so the
  // body's real wrap width is known. heightForWidth does not feed sizeHint and
  // the styled card breaks its propagation, so the dialog can open a single line
  // tall and clip a multi-line body — re-measure at the resolved width and grow.
  if (first_show) {
    const int label_w = body_label_->width();
    if (label_w > 0) {
      const int needed = body_label_->heightForWidth(label_w);
      const int extra = needed - body_label_->height();
      if (extra > 0) {
        body_label_->setMinimumHeight(needed);  // hard floor: the wrapped text never clips
        resize(width(), height() + extra);
      }
    }
  }

  // (2) Pin the dialog to the exact height its content needs at the shown width
  // so the QVBoxLayout never compresses the inter-button spacings. Otherwise the
  // layout steals the wrapped-body height deficit from the most compressible
  // items — the gaps between stacked buttons — and they shrink by a different
  // amount per dialog. Runs after (1) so the grown body minimum height is
  // already reflected in the layout's heightForWidth (6 px between buttons,
  // 14 px elsewhere).
  if (layout() != nullptr && layout()->hasHeightForWidth()) {
    const int needed = layout()->heightForWidth(width());
    if (needed > 0 && needed != height()) {
      resize(width(), needed);
    }
  }

  // exec() centered us at the pre-grow size; re-center on first show after any
  // growth so the dialog stays balanced relative to its parent window. Guarded by
  // first_show so a later re-show (which still runs the spacing fix) does not
  // move a dialog the user may have positioned.
  if (first_show) {
    if (const QWidget* anchor = parentWidget() != nullptr ? parentWidget()->window() : nullptr) {
      const QRect parent_geom = anchor->geometry();
      move(parent_geom.center().x() - (width() / 2), parent_geom.center().y() - (height() / 2));
    }
  }
}

void MessageBox::rewrapButtonLabels() {
  // Widest a single button line may be before the dialog would exceed its
  // maximumWidth(): the cap minus the card margins, border and button padding.
  const int max_line = maximumWidth() - 2 * theme::space(kCardContentMargin) - 2 * kCardBorder -
                       2 * theme::space(kButtonHPadding) - kWrapSlack;
  for (int i = 0; i < buttons_.size(); ++i) {
    // Force the button's own polish so its font reflects the QSS font-size
    // before we measure: the dialog's Polish fires before its children's, so
    // without this we'd wrap against the default font and overflow at the
    // larger rendered size.
    buttons_[i]->ensurePolished();
    buttons_[i]->setText(wrapLabelToWidth(button_labels_[i], QFontMetrics(buttons_[i]->font()), max_line));
  }
}

// --- Static helpers ----------------------------------------------------------

namespace {

// Shows a single-OK notice. Desktop keeps the modal exec(); the browser cannot
// nest an event loop, so the dialog owns itself and opens non-blocking.
void showNotice(QWidget* parent, const QString& title, const QString& text) {
#ifdef PJ_TARGET_WASM
  auto* dlg = new MessageBox(parent);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setTitle(title);
  dlg->setText(text);
  dlg->addButton(MessageBox::tr("OK"), MessageBox::kPrimaryRole);
  dlg->open();
#else
  MessageBox dlg(parent);
  dlg.setTitle(title);
  dlg.setText(text);
  dlg.addButton(MessageBox::tr("OK"), MessageBox::kPrimaryRole);
  dlg.exec();
#endif
}

}  // namespace

void MessageBox::information(QWidget* parent, const QString& title, const QString& text) {
  showNotice(parent, title, text);
}

void MessageBox::warning(QWidget* parent, const QString& title, const QString& text) {
  showNotice(parent, title, text);
}

void MessageBox::critical(QWidget* parent, const QString& title, const QString& text) {
  showNotice(parent, title, text);
}

int MessageBox::question(
    QWidget* parent, const QString& title, const QString& text, std::initializer_list<ButtonSpec> buttons,
    bool* dont_show_again) {
  MessageBox dlg(parent);
  dlg.setTitle(title);
  dlg.setText(text);
  if (dont_show_again != nullptr) {
    dlg.setShowDontShowAgain(true);
  }
  for (const auto& spec : buttons) {
    dlg.addButton(spec.label, spec.role);
  }
  dlg.exec();
  if (dont_show_again != nullptr) {
    *dont_show_again = dlg.dontShowAgainChecked();
  }
  return dlg.clickedIndex();
}

void MessageBox::questionAsync(
    QWidget* parent, const QString& title, const QString& text, std::vector<ButtonSpec> buttons,
    std::function<void(int choice, bool dont_show_again)> on_done, bool show_dont_show_again) {
  auto* dlg = new MessageBox(parent);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setTitle(title);
  dlg->setText(text);
  dlg->setShowDontShowAgain(show_dont_show_again);
  for (const auto& spec : buttons) {
    dlg->addButton(spec.label, spec.role);
  }
  connect(dlg, &QDialog::finished, dlg, [dlg, on_done = std::move(on_done)](int) {
    if (on_done) {
      on_done(dlg->clickedIndex(), dlg->dontShowAgainChecked());
    }
  });
#ifdef PJ_TARGET_WASM
  dlg->open();
#else
  dlg->exec();
#endif
}

}  // namespace PJ
