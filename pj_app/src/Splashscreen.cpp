// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "Splashscreen.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFont>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QRandomGenerator>
#include <QRectF>
#include <QSettings>
#include <QSize>
#include <QStringList>
#include <QSvgRenderer>
#include <Qt>
#include <algorithm>

#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

// Avoids the most recently shown memes (persisted in QSettings). Null when the pool is empty.
QPixmap getFunnySplashscreen() {
  QDir memes_dir(u":/resources/memes"_s);
  const QStringList files = memes_dir.entryList({u"*.jpg"_s}, QDir::Files, QDir::Name);
  if (files.isEmpty()) {
    return {};
  }

  QSettings settings;
  QStringList recent = settings.value(u"previousFunnyMemesList"_s).toStringList();
  // At most pool - 1, or the pick loop below would spin forever.
  const qsizetype avoid_count = std::min<qsizetype>(recent.size(), files.size() - 1);
  const QStringList avoid = recent.mid(recent.size() - avoid_count);

  QString chosen;
  do {
    chosen = files.at(QRandomGenerator::global()->bounded(static_cast<int>(files.size())));
  } while (avoid.contains(chosen));

  recent.append(chosen);
  while (recent.size() > 10) {
    recent.removeFirst();
  }
  settings.setValue(u"previousFunnyMemesList"_s, recent);

  return capMemeToSplashBox(QPixmap(memes_dir.filePath(chosen)));
}

const QStringList& seriousSubtitles() {
  static const QStringList kSubtitles = {
      u"Now you're plotting"_s,
      u"The plot thickens"_s,
      u"Hold my rosbag"_s,
      u"Make your data confess"_s,
      u"More than just curves now"_s,
      u"Better than it has any right to be"_s,
      u"Objectively the best. Probably."_s,
      u"Still better than printf"_s,
      u"Friends don't let friends use printf"_s,
      u"Lovingly over-engineered"_s,
      u"Suspiciously good for a free tool"_s,
      u"Opening PlotJuggler. Again."_s,
      u"Curves were just the beginning"_s,
      u"Mostly written by an AI"_s,
      u"An AI helped. Allegedly."_s,
  };
  return kSubtitles;
}

// Painted rather than an image, so it stays crisp at any DPI and the subtitle can vary.
QPixmap makeSeriousSplashscreen() {
  constexpr int kW = 720;
  constexpr int kH = 300;
  const qreal dpr = qMax(1.0, qApp->devicePixelRatio());

  QPixmap pixmap(QSize(kW, kH) * dpr);
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(Qt::transparent);

  QPainter p(&pixmap);
  p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);

  const QRectF canvas(0, 0, kW, kH);

  const auto token_theme = theme::themeFor(true);
  const auto brand = theme::gradient(theme::Gradient::Brand, token_theme);

  QLinearGradient bg(canvas.topLeft(), canvas.bottomRight());
  bg.setColorAt(0.0, theme::surface(theme::Surface::Backdrop, token_theme));
  bg.setColorAt(1.0, theme::surface(theme::Surface::Backdrop, token_theme));
  p.fillRect(canvas, bg);

  QLinearGradient accent(canvas.bottomLeft(), canvas.bottomRight());
  accent.setColorAt(0.0, brand.first);
  accent.setColorAt(1.0, brand.second);
  p.fillRect(QRectF(0, kH - 4, kW, 4), accent);

  constexpr qreal kLogoSize = 156;
  const QRectF logo_rect(56, (kH - kLogoSize) / 2.0, kLogoSize, kLogoSize);
  QSvgRenderer logo(u":/resources/plotjuggler.svg"_s);
  if (logo.isValid()) {
    logo.render(&p, logo_rect);
  }

  const qreal text_x = logo_rect.right() + 44;
  const qreal wordmark_baseline = kH / 2.0 - 18;

  // The whole word in grey, then the P and J overdrawn in brand colours, so the spacing is one drawText's.
  QFont wordmark_font;
  wordmark_font.setFamilies({u"Khula"_s, u"Roboto"_s, wordmark_font.defaultFamily()});
  wordmark_font.setPixelSize(76);
  wordmark_font.setWeight(QFont::Thin);
  p.setFont(wordmark_font);
  const QString wordmark = u"PlotJuggler"_s;
  const QFontMetricsF wordmark_fm(wordmark_font);
  const qreal wordmark_width = wordmark_fm.horizontalAdvance(wordmark);
  const QColor wordmark_ink = theme::onSurface(theme::Surface::Backdrop, theme::Emphasis::Muted, token_theme);
  p.setPen(wordmark_ink);
  p.drawText(QPointF(text_x, wordmark_baseline), wordmark);
  p.setPen(brand.first);
  p.drawText(QPointF(text_x, wordmark_baseline), QStringLiteral("P"));
  p.setPen(brand.second);
  p.drawText(
      QPointF(text_x + wordmark_fm.horizontalAdvance(QStringLiteral("Plot")), wordmark_baseline), QStringLiteral("J"));

  QFont subtitle_font;
  subtitle_font.setFamilies({u"Roboto"_s, subtitle_font.defaultFamily()});
  subtitle_font.setPixelSize(26);
  subtitle_font.setWeight(QFont::Light);
  p.setFont(subtitle_font);
  p.setPen(wordmark_ink);
  const QStringList& subs = seriousSubtitles();
  const QString subtitle = subs.at(QRandomGenerator::global()->bounded(static_cast<int>(subs.size())));
  const qreal subtitle_width = QFontMetricsF(subtitle_font).horizontalAdvance(subtitle);
  p.drawText(QPointF(text_x + (wordmark_width - subtitle_width) / 2.0, wordmark_baseline + 56), subtitle);

  QFont version_font;
  version_font.setPixelSize(13);
  p.setFont(version_font);
  p.setPen(theme::onSurface(theme::Surface::Backdrop, theme::Emphasis::Disabled, token_theme));
  p.drawText(
      QRectF(0, kH - 30, kW - 18, 20), Qt::AlignRight | Qt::AlignVCenter, QCoreApplication::applicationVersion());

  p.end();
  return pixmap;
}

}  // namespace

QPixmap capMemeToSplashBox(const QPixmap& pixmap) {
  if (pixmap.width() <= kMaxMemeExtent && pixmap.height() <= kMaxMemeExtent) {
    return pixmap;
  }
  return pixmap.scaled(QSize(kMaxMemeExtent, kMaxMemeExtent), Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QPixmap makeStartupSplash() {
  QSettings settings;
  const QString mode = settings.value(kSplashModeKey, kSplashModeMemes).toString();
  if (mode != kSplashModeSerious) {
    // No meme pool in the browser build: fall back to the serious splash.
    if (QPixmap meme = getFunnySplashscreen(); !meme.isNull()) {
      return meme;
    }
  }
  return makeSeriousSplashscreen();
}

}  // namespace PJ
