// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QApplication>
#include <QUrl>

#include "pj_marketplace/marketplace_window.hpp"
using namespace Qt::StringLiterals;

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  // Match pj_app so AppDataLocation resolves to the same install tree.
  QCoreApplication::setOrganizationName(u"PlotJuggler"_s);
  QCoreApplication::setApplicationName(u"PlotJuggler4"_s);
  const QUrl registry_url =
      QUrl("https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry/refs/heads/development/registry.json");
  PJ::MarketplaceWindow w(registry_url);
  w.resize(700, 500);
  w.show();
  return app.exec();
}
