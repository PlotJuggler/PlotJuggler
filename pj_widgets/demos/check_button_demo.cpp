// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Standalone visual demo for PJ::CheckButton (pill toggle), PJ::DualOptionsWidget (segmented control) and
// PJ::ColorPickerWidget (swatch): both selected states and the "Override color" row layout (CheckButton and swatch on
// one line), under the real app theme.
//
// Interactive: ./build/pj_widgets/demos/pj_widgets_check_button_demo Screenshot :
// ./build/pj_widgets/demos/pj_widgets_check_button_demo --screenshot out.png [--theme dark|light]

#include <QApplication>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "pj_widgets/CheckButton.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/DualOptionsWidget.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/IntScrubber.h"
#include "pj_widgets/Style.h"
#include "qss_preprocessor.h"
using namespace Qt::StringLiterals;

namespace {
using pj_widgets_demos::applyTheme;
}  // namespace

int main(int argc, char** argv) {
  QApplication::setOrganizationName(u"PlotJuggler"_s);
  QApplication::setApplicationName(u"CheckButtonDemo"_s);
  QApplication app(argc, argv);
  QApplication::setStyle(new PJ::Style(u"Fusion"_s));  // same as the app: pins inputs to 20px

  QString screenshot;
  QString theme = u"dark"_s;
  for (int i = 1; i < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == "--screenshot"_L1 && i + 1 < argc) {
      screenshot = QString::fromLocal8Bit(argv[++i]);
    } else if (arg == "--theme"_L1 && i + 1 < argc) {
      theme = QString::fromLocal8Bit(argv[++i]);
    }
  }
  applyTheme(theme);

  QMainWindow win;
  win.setWindowTitle(u"pj_widgets compact controls"_s);

  auto* root = new QWidget(&win);
  root->setObjectName(u"ConfigPanel"_s);
  auto* form = new QFormLayout(root);
  form->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::Section), PJ::theme::space(PJ::theme::Space::Section),
      PJ::theme::space(PJ::theme::Space::Section), PJ::theme::space(PJ::theme::Space::Section));
  form->setSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));

  // Every input type that must end up the SAME compact height (20px): a native
  // line edit + our combobox + the self-painted scrubbers + the pills + swatch.
  auto* line_edit = new QLineEdit(root);
  line_edit->setText(u"base_link"_s);
  form->addRow(u"LineEdit:"_s, line_edit);
  auto* combo = new PJ::ComboBox(root);
  combo->addItems({u"File"_s, u"Topic"_s, u"URL"_s});
  form->addRow(u"Combo:"_s, combo);
  auto* dscrub = new PJ::DoubleScrubber(root);
  dscrub->setValue(0.15);
  form->addRow(u"DoubleScrubber:"_s, dscrub);
  auto* iscrub = new PJ::IntScrubber(root);
  iscrub->setValue(10);
  form->addRow(u"IntScrubber:"_s, iscrub);

  auto* off = new PJ::CheckButton(u"X arrow only"_s, root);
  form->addRow(u"CheckButton:"_s, off);
  auto* on = new PJ::CheckButton(u"Override color"_s, root);
  on->setChecked(true);

  auto* segmented_left = new PJ::DualOptionsWidget(u"Frame"_s, u"Arrow"_s, root);
  form->addRow(u"Segmented L:"_s, segmented_left);
  auto* segmented_right = new PJ::DualOptionsWidget(u"Frame"_s, u"Arrow"_s, root);
  segmented_right->setSelectedIndex(1);
  form->addRow(u"Segmented R:"_s, segmented_right);

  // "Override color" row: pill toggle + swatch side by side (Image #3 layout).
  auto* override_row = new QWidget(root);
  auto* row_layout = new QHBoxLayout(override_row);
  row_layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  row_layout->setSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  auto* swatch = new PJ::ColorPickerWidget(override_row);
  swatch->setColor(QColor(0xE0, 0x39, 0x39));
  row_layout->addWidget(on);
  row_layout->addWidget(swatch);
  row_layout->addStretch();
  form->addRow(u"Checked + swatch:"_s, override_row);

  auto* toggle = new QPushButton(u"Toggle theme (currently: %1)"_s.arg(theme), root);
  QObject::connect(toggle, &QPushButton::clicked, root, [toggle, theme]() mutable {
    theme = (theme == "dark"_L1) ? u"light"_s : u"dark"_s;
    applyTheme(theme);
    toggle->setText(u"Toggle theme (currently: %1)"_s.arg(theme));
  });
  form->addRow(QString(), toggle);

  win.setCentralWidget(root);
  win.resize(380, 300);
  win.show();

  // Print the resolved heights once laid out so the QSS can be tuned by number.
  auto report = [=]() {
    qInfo(
        "HEIGHTS  LineEdit=%d  Combo=%d  DoubleScrubber=%d  IntScrubber=%d  CheckButton=%d  DualOptions=%d  Swatch=%d",
        line_edit->height(), combo->height(), dscrub->height(), iscrub->height(), off->height(),
        segmented_left->height(), swatch->height());
  };

  if (!screenshot.isEmpty()) {
    QTimer::singleShot(300, &win, [&win, screenshot, &app, report] {
      report();
      const QPixmap pix = win.grab();
      if (pix.save(screenshot)) {
        qInfo("[check_button_demo] screenshot saved: %s (%dx%d)", qPrintable(screenshot), pix.width(), pix.height());
      } else {
        qWarning("[check_button_demo] screenshot FAILED");
      }
      app.quit();
    });
  } else {
    QTimer::singleShot(300, root, report);
  }

  return app.exec();
}
