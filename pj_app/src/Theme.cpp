// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "Theme.h"

#include <QColor>
#include <QFile>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QPalette>
#include <QSettings>
#include <QStringList>
#include <QStyleHints>
#include <map>
#include <utility>

#include "pj_widgets/SvgUtil.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

Q_LOGGING_CATEGORY(lcTheme, "pj.app.theme")

constexpr const char* kPaletteStartMarker = "PALETTE START";
constexpr const char* kPaletteEndMarker = "PALETTE END";

QString resourcePathFor(const QString& name) {
  return u":/resources/stylesheet_%1.qss"_s.arg(name);
}

// Expand `${KEY}` tokens against `palette`. Unknown keys are left as
// literal `${KEY}` so they're visible in the rendered styling and the
// developer notices, but we don't crash the app, since QSS files are
// resources we control and a missing key is a build-time bug.
QString expandPlaceholders(const QString& body, const std::map<QString, QString>& palette) {
  QString out;
  out.reserve(body.size());

  qsizetype i = 0;
  while (i < body.size()) {
    const qsizetype start = body.indexOf("${"_L1, i);
    if (start < 0) {
      out.append(QStringView{body}.mid(i));
      break;
    }
    out.append(QStringView{body}.mid(i, start - i));
    const qsizetype end = body.indexOf(QLatin1Char('}'), start + 2);
    if (end < 0) {
      qCWarning(lcTheme) << "Unclosed ${...} token in QSS, leaving literal:" << body.mid(start, 32);
      out.append(QStringView{body}.mid(start));
      break;
    }
    const QString key = body.mid(start + 2, end - start - 2);
    auto it = palette.find(key);
    if (it == palette.end()) {
      qCWarning(lcTheme) << "Unknown palette key" << key << ", leaving literal placeholder";
      out.append(QStringView{body}.mid(start, end - start + 1));
    } else {
      out.append(it->second);
    }
    i = end + 1;
  }
  return out;
}

QString parseAndExpand(const QString& qss, std::map<QString, QString>* tokens_out = nullptr) {
  const QStringList lines = qss.split(QLatin1Char('\n'));

  std::map<QString, QString> palette;
  int i = 0;
  while (i < lines.size() && !lines[i].contains(QLatin1String(kPaletteStartMarker))) {
    ++i;
  }
  if (i == lines.size()) {
    qCWarning(lcTheme) << "No PALETTE START marker found, returning QSS unchanged";
    return qss;
  }
  ++i;  // skip the START marker line itself

  while (i < lines.size() && !lines[i].contains(QLatin1String(kPaletteEndMarker))) {
    const QString trimmed = lines[i].trimmed();
    if (!trimmed.isEmpty() && !trimmed.startsWith(QLatin1String("//"))) {
      const qsizetype colon = trimmed.indexOf(QLatin1Char(':'));
      if (colon > 0) {
        QString key = trimmed.left(colon).trimmed();
        QString value = trimmed.mid(colon + 1).trimmed();
        if (value.endsWith(QLatin1Char(';'))) {
          value.chop(1);
          value = value.trimmed();
        }
        palette.emplace(std::move(key), std::move(value));
      }
    }
    ++i;
  }
  if (i == lines.size()) {
    qCWarning(lcTheme) << "Reached end of QSS without PALETTE END marker, palette may be incomplete";
  } else {
    ++i;  // skip END marker
  }

  QString body;
  body.reserve(qss.size());
  for (; i < lines.size(); ++i) {
    body.append(lines[i]);
    body.append(QLatin1Char('\n'));
  }
  QString expanded = expandPlaceholders(body, palette);
  if (tokens_out != nullptr) {
    *tokens_out = std::move(palette);
  }
  return expanded;
}

// Qt style sheets do NOT update QPalette, so widgets that paint themselves
// outside QSS (the 3D view's GL background derives dark/light from
// palette().color(QPalette::Window) luminance) would otherwise read the
// PLATFORM palette and guess the theme wrong (dark scene in light mode on a
// dark desktop). Keep the application palette's Window/WindowText in lockstep
// with the theme tokens; QGuiApplication::setPalette also delivers
// ApplicationPaletteChange to every widget, repainting them on theme switches.
void syncApplicationPalette(const std::map<QString, QString>& tokens) {
  if (qGuiApp == nullptr) {
    return;  // Theme constructed without a GUI application (tests)
  }
  const auto window_it = tokens.find(u"backdrop"_s);
  const auto text_it = tokens.find(u"on_backdrop_default"_s);
  if (window_it == tokens.end() || text_it == tokens.end()) {
    qCWarning(lcTheme) << "Palette tokens backdrop/on_backdrop_default missing; application palette not synced";
    return;
  }
  const QColor window_color(window_it->second);
  const QColor text_color(text_it->second);
  if (!window_color.isValid() || !text_color.isValid()) {
    qCWarning(lcTheme) << "Palette tokens are not valid colors; application palette not synced";
    return;
  }
  QPalette pal = QGuiApplication::palette();
  pal.setColor(QPalette::Window, window_color);
  pal.setColor(QPalette::WindowText, text_color);
  QGuiApplication::setPalette(pal);
}

}  // namespace

QStringList Theme::availableThemes() {
  return {u"light"_s, u"dark"_s};
}

Theme::Theme(QObject* parent) : QObject(parent), name_(QSettings().value(kThemeSettingsKey, "light").toString()) {
  if (!availableThemes().contains(name_)) {
    name_ = u"light"_s;
    // Rewrite stale or corrupt persisted name so subsequent readers
    // (e.g. SvgUtil::currentTheme) see the same value Theme is using.
    QSettings().setValue(kThemeSettingsKey, name_);
  }
  rebuildQss();
}

Qt::ColorScheme Theme::colorScheme() const {
  return name_ == u"light"_s ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark;
}

QString Theme::currentTheme() const {
  return name_;
}

QString Theme::expandedQss() const {
  return expanded_qss_;
}

void Theme::setTheme(const QString& name) {
  if (name == name_) {
    return;
  }
  if (!availableThemes().contains(name)) {
    qCWarning(lcTheme) << "Ignoring setTheme with unknown name:" << name;
    return;
  }
  name_ = name;
  QSettings().setValue(kThemeSettingsKey, name_);
  rebuildQss();
  emit themeChanged(name_);
  emit qssChanged();
}

void Theme::rebuildQss() {
  // The system title bar is drawn by the platform, not by QSS, so the app's own
  // light/dark choice reaches it only through Qt's color scheme (honoured on
  // Windows and macOS; a Linux window manager keeps drawing its own frame).
  if (qGuiApp != nullptr) {  // no GUI application in some tests
    QGuiApplication::styleHints()->setColorScheme(colorScheme());
  }
  const QString path = resourcePathFor(name_);
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qCWarning(lcTheme) << "Cannot open" << path << ", stylesheet will be empty";
    expanded_qss_.clear();
    return;
  }
  const QString raw = QString::fromUtf8(file.readAll());
  std::map<QString, QString> tokens;
  expanded_qss_ = parseAndExpand(raw, &tokens);
  syncApplicationPalette(tokens);
}

}  // namespace PJ
