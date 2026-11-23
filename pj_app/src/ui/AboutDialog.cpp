// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/AboutDialog.h"

#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QLayout>

#include "pj_widgets/SvgUtil.h"
#include "ui_AboutDialog.h"
#include "ui_NoticesDialog.h"
using namespace Qt::StringLiterals;

namespace PJ {

#ifndef PJ_TARGET_WASM
namespace {

// The desktop build embeds THIRD_PARTY_NOTICES.txt (cmake/PjNotices.cmake, pj_app/CMakeLists.txt); test runners do not.
const auto kNoticesResource = u":/THIRD_PARTY_NOTICES.txt"_s;

void showNotices(QWidget* parent) {
  QFile file(kNoticesResource);
  if (!file.open(QIODevice::ReadOnly)) {
    return;
  }
  auto* dialog = new Dialog(parent);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setDialogTitle(QObject::tr("Third-party licences"));
  auto* body = new QWidget;
  Ui::NoticesDialog ui;
  ui.setupUi(body);
  // The licence texts are laid out for a fixed-width font.
  ui.noticesText->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  ui.noticesText->setPlainText(QString::fromUtf8(file.readAll()));
  dialog->contentLayout()->addWidget(body);
  dialog->open();
}

}  // namespace
#endif

AboutDialog::AboutDialog(QWidget* parent) : Dialog(parent), ui_(new Ui::AboutDialog) {
  setDialogTitle(tr("About PlotJuggler"));

  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);

  // LoadSvg caches a 64x64 render; the logo shows at that native size.
  ui_->logoLabel->setPixmap(loadSvg(u":/resources/svg/plotjuggler.svg"_s, currentTheme()));
  ui_->versionLabel->setText(tr("Version %1").arg(QApplication::applicationVersion()));
#ifdef PJ_TARGET_WASM
  // The browser bundle is GPL-3.0 (THIRD_PARTY_NOTICES.txt says why); the relative hrefs resolve against the page URL,
  // where scripts/package_wasm.mjs serves the notices and wasm_source_offer.mjs the offer.
  ui_->licenseLabel->setText(tr("Browser build: GNU GPL v3. PlotJuggler's source: MPL 2.0"));
  ui_->noticesLabel->setText(
      u"<a href=\"THIRD_PARTY_NOTICES.txt\">%1</a> \u00b7 <a href=\"sources/SOURCE-OFFER.txt\">%2</a>"_s.arg(
          tr("Third-party licences"), tr("Source code")));
#else
  if (QFile::exists(kNoticesResource)) {
    ui_->noticesLabel->setOpenExternalLinks(false);
    connect(ui_->noticesLabel, &QLabel::linkActivated, this, [this] { showNotices(this); });
  } else {
    ui_->noticesLabel->hide();
  }
#endif

  // The frameless Dialog chrome otherwise opens at its base minimum and clips the content.
  layout()->activate();
  setFixedSize(sizeHint());
}

AboutDialog::~AboutDialog() {
  delete ui_;
}

}  // namespace PJ
