// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "qss_preprocessor.h"

#include <QApplication>
#include <QChar>
#include <QColor>
#include <QDebug>
#include <QFile>
#include <QGuiApplication>
#include <QIODevice>
#include <QPalette>
#include <QSettings>
#include <QStringList>
#include <QStringView>
#include <map>
using namespace Qt::StringLiterals;

namespace pj_widgets_demos {

namespace {

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
      out.append(QStringView{body}.mid(start));
      break;
    }
    const QString key = body.mid(start + 2, end - start - 2);
    auto it = palette.find(key);
    if (it == palette.end()) {
      qWarning() << "Unknown palette key" << key;
      out.append(QStringView{body}.mid(start, end - start + 1));
    } else {
      out.append(it->second);
    }
    i = end + 1;
  }
  return out;
}

QString loadAndExpandQss(const QString& theme, std::map<QString, QString>* tokens_out) {
  const QString path = u"%1/stylesheet_%2.qss"_s.arg(QStringLiteral(PJ_QSS_DIR), theme);
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "Cannot open" << path;
    return {};
  }
  const QString raw = QString::fromUtf8(file.readAll());
  const QStringList lines = raw.split(QLatin1Char('\n'));

  std::map<QString, QString> palette;
  int i = 0;
  while (i < lines.size() && !lines[i].contains(QLatin1String("PALETTE START"))) {
    ++i;
  }
  ++i;
  while (i < lines.size() && !lines[i].contains(QLatin1String("PALETTE END"))) {
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
  ++i;

  QString body;
  for (; i < lines.size(); ++i) {
    body.append(lines[i]);
    body.append(QLatin1Char('\n'));
  }
  if (tokens_out != nullptr) {
    *tokens_out = palette;
  }
  return expandPlaceholders(body, palette);
}

void syncApplicationPalette(const std::map<QString, QString>& tokens) {
  if (qGuiApp == nullptr) {
    return;
  }

  const auto color_for = [&](const QString& key) -> QColor {
    const auto it = tokens.find(key);
    return it == tokens.end() ? QColor() : QColor(it->second);
  };

  const QColor window = color_for(u"backdrop"_s);
  const QColor text = color_for(u"text"_s);
  const QColor base = color_for(u"input"_s);
  const QColor button = color_for(u"backdrop"_s);
  const QColor highlight = color_for(u"selection_fill"_s);
  const QColor highlighted_text = color_for(u"on_selection"_s);
  if (!window.isValid() || !text.isValid() || !base.isValid() || !button.isValid() || !highlight.isValid() ||
      !highlighted_text.isValid()) {
    qWarning() << "Cannot sync demo palette from theme tokens";
    return;
  }

  QPalette pal = QGuiApplication::palette();
  pal.setColor(QPalette::Window, window);
  pal.setColor(QPalette::WindowText, text);
  pal.setColor(QPalette::Base, base);
  pal.setColor(QPalette::Text, text);
  pal.setColor(QPalette::Button, button);
  pal.setColor(QPalette::ButtonText, text);
  pal.setColor(QPalette::Highlight, highlight);
  pal.setColor(QPalette::HighlightedText, highlighted_text);
  QGuiApplication::setPalette(pal);
}

}  // namespace

QString loadAndExpandQss(const QString& theme) {
  return loadAndExpandQss(theme, nullptr);
}

void applyTheme(const QString& theme) {
  QSettings settings;
  settings.setValue(u"StyleSheet::theme"_s, theme);
  settings.sync();

  std::map<QString, QString> tokens;
  qApp->setStyleSheet(loadAndExpandQss(theme, &tokens));
  syncApplicationPalette(tokens);
}

}  // namespace pj_widgets_demos
