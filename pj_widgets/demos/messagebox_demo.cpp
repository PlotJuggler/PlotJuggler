// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Standalone visual demo for pj_widgets/MessageBox: one button per variant (information, warning, critical, question,
// destructive) plus a theme toggle.

#include <QApplication>
#include <QDebug>
#include <QMainWindow>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/MessageBox.h"
#include "qss_preprocessor.h"
using namespace Qt::StringLiterals;

namespace {

using pj_widgets_demos::applyTheme;

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setOrganizationName(u"PlotJuggler"_s);
  QApplication::setApplicationName(u"MessageBoxDemo"_s);

  QString interactive_theme = u"dark"_s;
  for (int i = 1; i < argc; ++i) {
    const QString a = QString::fromLocal8Bit(argv[i]);
    if (a == "--theme"_L1 && i + 1 < argc) {
      interactive_theme = QString::fromLocal8Bit(argv[++i]);
    }
  }

  applyTheme(interactive_theme);

  QMainWindow win;
  win.setWindowTitle(u"PJ MessageBox demo"_s);
  auto* central = new QWidget;
  auto* lay = new QVBoxLayout(central);
  lay->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::Section), PJ::theme::space(PJ::theme::Space::Section),
      PJ::theme::space(PJ::theme::Space::Section), PJ::theme::space(PJ::theme::Space::Section));
  lay->setSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));

  auto add_trigger = [&](const QString& label, std::function<void()> on_click) {
    auto* btn = new QPushButton(label, central);
    QObject::connect(btn, &QPushButton::clicked, central, on_click);
    lay->addWidget(btn);
  };

  add_trigger(u"information() — single OK"_s, [&win]() {
    PJ::MessageBox::information(&win, u"Layout saved"_s, u"Workspace layout written to disk."_s);
  });
  add_trigger(u"warning() — single OK"_s, [&win]() {
    PJ::MessageBox::warning(&win, u"Load failed"_s, u"Cannot read /tmp/missing.csv — file does not exist."_s);
  });
  add_trigger(u"critical() — single OK"_s, [&win]() {
    PJ::MessageBox::critical(&win, u"Fatal error"_s, u"Internal pipeline crashed; restart required."_s);
  });
  add_trigger(u"question() — 3 vertical buttons (canonical)"_s, [&win]() {
    bool dont_show = false;
    const int chosen = PJ::MessageBox::question(
        &win, u"Close pane?"_s, u"You have 1 process running in this pane."_s,
        {{u"Yes, close"_s, PJ::MessageBox::kPrimaryRole},
         {u"Show running processes"_s, PJ::MessageBox::kNeutralRole},
         {u"Cancel"_s, PJ::MessageBox::kCancelRole}},
        &dont_show);
    qInfo() << "chosen=" << chosen << "dont_show_again=" << dont_show;
  });
  add_trigger(u"question() — destructive role"_s, [&win]() {
    const int chosen = PJ::MessageBox::question(
        &win, u"Delete dataset?"_s, u"This will permanently remove 'experiment_42.bag' and its derived series."_s,
        {{u"Delete"_s, PJ::MessageBox::kDestructiveRole}, {u"Cancel"_s, PJ::MessageBox::kCancelRole}});
    qInfo() << "chosen=" << chosen;
  });

  auto* toggle = new QPushButton(u"Toggle theme (currently: %1)"_s.arg(interactive_theme), central);
  QObject::connect(toggle, &QPushButton::clicked, central, [toggle, theme = interactive_theme]() mutable {
    theme = (theme == "dark"_L1) ? u"light"_s : u"dark"_s;
    applyTheme(theme);
    toggle->setText(u"Toggle theme (currently: %1)"_s.arg(theme));
  });
  lay->addWidget(toggle);
  lay->addStretch();

  win.setCentralWidget(central);
  win.resize(360, 280);
  win.show();
  return app.exec();
}
