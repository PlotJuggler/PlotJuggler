// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Standalone visual demo for QComboBox styling: several PJ::ComboBox variants (basic, long content, many items,
// editable, disabled) plus a DoubleScrubber for side-by-side input-chrome comparison and a theme toggle. Click a combo
// box to see the popup styling.

#include <QApplication>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
#include "qss_preprocessor.h"
using namespace Qt::StringLiterals;

namespace {

using pj_widgets_demos::applyTheme;

// Populates a fresh PJ::ComboBox with a labelled variant. Caller owns.
QComboBox* makeCombo(const QString& variant, QWidget* parent) {
  auto* combo = new PJ::ComboBox(parent);
  if (variant == "basic"_L1) {
    combo->addItems({u"Apple"_s, u"Banana"_s, u"Cherry"_s, u"Date"_s, u"Elderberry"_s});
  } else if (variant == "long"_L1) {
    combo->addItems(
        {u"Short"_s, u"Medium length item"_s, u"A very long item name that exceeds the typical combobox width"_s,
         u"Another quite long entry for measuring elision behaviour"_s});
  } else if (variant == "many"_L1) {
    for (int i = 1; i <= 30; ++i) {
      combo->addItem(u"Item %1"_s.arg(i));
    }
  } else if (variant == "editable"_L1) {
    combo->setEditable(true);
    combo->addItems({u"Recent value 1"_s, u"Recent value 2"_s, u"Recent value 3"_s});
  } else if (variant == "disabled"_L1) {
    combo->addItems({u"This is disabled"_s, u"Option B"_s});
    combo->setEnabled(false);
  }
  return combo;
}

QWidget* makeRow(const QString& label, const QString& variant, QWidget* parent) {
  auto* row = new QWidget(parent);
  auto* lay = new QFormLayout(row);
  lay->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  lay->setHorizontalSpacing(PJ::theme::space(PJ::theme::Space::Section));
  auto* lbl = new QLabel(label, row);
  lay->addRow(lbl, makeCombo(variant, row));
  return row;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setOrganizationName(u"PlotJuggler"_s);
  QApplication::setApplicationName(u"ComboBoxDemo"_s);

  QString interactive_theme = u"dark"_s;
  for (int i = 1; i < argc; ++i) {
    const QString a = QString::fromLocal8Bit(argv[i]);
    if (a == "--theme"_L1 && i + 1 < argc) {
      interactive_theme = QString::fromLocal8Bit(argv[++i]);
    }
  }

  applyTheme(interactive_theme);

  QMainWindow win;
  win.setWindowTitle(u"PJ QComboBox demo"_s);
  auto* central = new QWidget;
  auto* lay = new QVBoxLayout(central);
  lay->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::Section), PJ::theme::space(PJ::theme::Space::Section),
      PJ::theme::space(PJ::theme::Space::Section), PJ::theme::space(PJ::theme::Space::Section));
  lay->setSpacing(PJ::theme::space(PJ::theme::Space::Section));

  lay->addWidget(makeRow(u"Basic"_s, u"basic"_s, central));
  lay->addWidget(makeRow(u"Long content"_s, u"long"_s, central));
  lay->addWidget(makeRow(u"Many items"_s, u"many"_s, central));
  lay->addWidget(makeRow(u"Editable"_s, u"editable"_s, central));
  lay->addWidget(makeRow(u"Disabled"_s, u"disabled"_s, central));

  // Reference: a DoubleScrubber for side-by-side comparison of the closed
  // input chrome — the QComboBox above should look visually consistent.
  {
    auto* ref_row = new QWidget(central);
    auto* ref_lay = new QFormLayout(ref_row);
    ref_lay->setContentsMargins(
        PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
        PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
    ref_lay->setHorizontalSpacing(PJ::theme::space(PJ::theme::Space::Section));
    auto* scrubber = new PJ::DoubleScrubber(ref_row);
    scrubber->setRange(0.0, 100.0);
    scrubber->setValue(42.0);
    ref_lay->addRow(new QLabel(u"DoubleScrubber (reference)"_s, ref_row), scrubber);
    lay->addWidget(ref_row);
  }

  auto* toggle = new QPushButton(u"Toggle theme (currently: %1)"_s.arg(interactive_theme), central);
  QObject::connect(toggle, &QPushButton::clicked, central, [toggle, theme = interactive_theme]() mutable {
    theme = (theme == "dark"_L1) ? u"light"_s : u"dark"_s;
    applyTheme(theme);
    toggle->setText(u"Toggle theme (currently: %1)"_s.arg(theme));
  });
  lay->addWidget(toggle);
  lay->addStretch();

  win.setCentralWidget(central);
  win.resize(480, 380);
  win.show();

  return app.exec();
}
