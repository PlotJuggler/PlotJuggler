#include "BundledFont.h"

#include <QFontDatabase>
#include <QStringList>

namespace PJ {

QString registerBundledFont(const QString& directory) {
  QString family;
  for (const QString& face :
       {QStringLiteral("PlotJugglerSans-Regular.ttf"), QStringLiteral("PlotJugglerSans-Bold.ttf")}) {
    const int id = QFontDatabase::addApplicationFont(directory + u'/' + face);
    if (id < 0) {
      qWarning("Failed to load bundled PlotJuggler Sans font: %s", qPrintable(face));
      continue;
    }
    if (family.isEmpty()) {
      const QStringList families = QFontDatabase::applicationFontFamilies(id);
      family = families.isEmpty() ? QString() : families.first();
    }
  }
  return family;
}

}  // namespace PJ
