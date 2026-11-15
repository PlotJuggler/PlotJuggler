// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QPixmap>
#include <QString>

namespace PJ {

// Preferences::splash_mode storage key and its two values, shared between the
// Preferences dialog (which writes them) and the startup path (which reads
// them) so the strings can't drift out of sync.
inline const QString kSplashModeKey = QStringLiteral("Preferences::splash_mode");
inline const QString kSplashModeMemes = QStringLiteral("memes");
inline const QString kSplashModeSerious = QStringLiteral("serious");

// Largest on-screen extent, in pixels, of a meme splash along either axis. The
// bound is a square box rather than a width limit because the pool mixes
// landscape and portrait art, and screen height is the scarcer dimension — a
// tall meme overflows a typical display long before a wide one does.
inline constexpr int kMaxMemeExtent = 600;

// Shrinks `pixmap` to fit inside a kMaxMemeExtent square, preserving its aspect
// ratio. Never upscales: a pixmap already inside the box is returned untouched,
// which Qt::KeepAspectRatio alone would not guarantee.
QPixmap capMemeToSplashBox(const QPixmap& pixmap);

// Builds the startup splash pixmap honouring Preferences::splash_mode:
// "serious" → a composed branded banner (logo + wordmark + random subtitle),
// anything else → a random meme from the bundled pool, or the serious banner
// when the pool is empty (the browser build bundles no memes).
QPixmap makeStartupSplash();

}  // namespace PJ
