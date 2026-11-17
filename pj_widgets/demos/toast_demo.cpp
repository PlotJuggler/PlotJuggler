// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Standalone visual demo for pj_widgets/ToastManager and ToastNotification.
//
// Interactive: buttons trigger each toast the release-update feature uses (a "new release available" toast with the
// success_kid icon and a clickable "View on GitHub" link, an "up to date" toast, and a failure toast).
//
// Headless: `--screenshot <path>` shows the release toast, waits for the slide-in to finish, grabs the window to a PNG
// and exits; QWidget::grab renders in-process, so it works under QT_QPA_PLATFORM=offscreen. `--theme dark|light`
// selects the palette.

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>
#include <functional>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/ToastManager.h"
#include "qss_preprocessor.h"
using namespace Qt::StringLiterals;

namespace {

using pj_widgets_demos::applyTheme;

// Mirrors MainWindow::checkForUpdates: rich text + escaped release name +
// a clickable link the toast label opens in the browser (openExternalLinks).
QString releaseMessage() {
  return QStringLiteral(
      "New release available: <span style='font-weight:600;'>PlotJuggler 3.999.1</span><br>"
      "<a href=\"https://github.com/PlotJuggler/PlotJuggler/releases/latest\">View on GitHub</a>");
}

// A QMainWindow that keeps the toast stack pinned bottom-right on resize,
// exactly as PJ::MainWindow does via its resizeEvent override.
class DemoWindow : public QMainWindow {
 public:
  PJ::ToastManager* toasts = nullptr;

 protected:
  void resizeEvent(QResizeEvent* event) override {
    QMainWindow::resizeEvent(event);
    if (toasts != nullptr) {
      toasts->updatePosition();
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setOrganizationName(u"PlotJuggler"_s);
  QApplication::setApplicationName(u"ToastDemo"_s);

  QString theme = u"dark"_s;
  QString screenshot_path;
  for (int i = 1; i < argc; ++i) {
    const QString a = QString::fromLocal8Bit(argv[i]);
    if (a == "--theme"_L1 && i + 1 < argc) {
      theme = QString::fromLocal8Bit(argv[++i]);
    } else if (a == "--screenshot"_L1 && i + 1 < argc) {
      screenshot_path = QString::fromLocal8Bit(argv[++i]);
    }
  }
  applyTheme(theme);

  auto* win = new DemoWindow;
  win->setWindowTitle(u"PJ Toast demo"_s);
  auto* toasts = new PJ::ToastManager(win);  // parents its container into the window
  win->toasts = toasts;

  auto* central = new QWidget;
  auto* lay = new QVBoxLayout(central);
  lay->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::Section), PJ::theme::space(PJ::theme::Space::Section),
      PJ::theme::space(PJ::theme::Space::Section), PJ::theme::space(PJ::theme::Space::Section));
  lay->setSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  lay->addWidget(new QLabel(u"Trigger a toast (slides in bottom-right):"_s, central));

  auto add = [&](const QString& label, const std::function<void()>& on_click) {
    auto* btn = new QPushButton(label, central);
    QObject::connect(btn, &QPushButton::clicked, central, on_click);
    lay->addWidget(btn);
  };
  add(u"New release available"_s,
      [toasts]() { toasts->showToast(releaseMessage(), QPixmap(u":/resources/success_kid.png"_s)); });
  add(u"Up to date"_s, [toasts]() { toasts->showToast(u"PlotJuggler is up to date."_s); });
  add(u"Could not check"_s,
      [toasts]() { toasts->showToast(u"Could not check for updates. Please try again later."_s); });
  lay->addStretch();
  win->setCentralWidget(central);
  win->resize(820, 520);
  win->show();

  if (!screenshot_path.isEmpty()) {
    // Show the release toast, let the 300 ms slide-in settle, grab, quit.
    toasts->showToast(releaseMessage(), QPixmap(u":/resources/success_kid.png"_s));
    QTimer::singleShot(600, win, [win, screenshot_path]() {
      const bool ok = win->grab().save(screenshot_path);
      std::fprintf(
          ok ? stdout : stderr, "[toast-demo] %s: %s\n", ok ? "saved" : "FAILED", qUtf8Printable(screenshot_path));
      QCoreApplication::quit();
    });
  }

  return app.exec();
}
