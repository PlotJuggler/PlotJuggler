// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QPixmap>
#include <QString>

namespace PJ {

// Persisted QSettings key and values.
inline const QString kSplashModeKey = QStringLiteral("Preferences::splash_mode");
inline const QString kSplashModeMemes = QStringLiteral("memes");
inline const QString kSplashModeSerious = QStringLiteral("serious");

// Pixels, along either axis: the pool mixes landscape and portrait art, and screen height is the scarcer one.
inline constexpr int kMaxMemeExtent = 600;

// Keeps the aspect ratio and never upscales, which scaled(..., Qt::KeepAspectRatio) alone would.
QPixmap capMemeToSplashBox(const QPixmap& pixmap);

// "serious" mode, or an empty meme pool (the browser build), gives the branded banner; otherwise a random meme.
QPixmap makeStartupSplash();

}  // namespace PJ
