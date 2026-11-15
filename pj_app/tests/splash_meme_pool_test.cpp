// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QPixmap>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include "Splashscreen.h"

// Q_INIT_RESOURCE / Q_CLEANUP_RESOURCE declare their functions in the current scope, so they must be used outside
// any namespace.
static void unregisterMemePool() {
  Q_CLEANUP_RESOURCE(memes);
}
static void registerMemePool() {
  Q_INIT_RESOURCE(memes);
}

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

QStringList memesIn(const QString& dir) {
  return QDir(dir).entryList({u"*.jpg"_s}, QDir::Files, QDir::Name);
}

// The pool the running app actually sees: rcc's virtual filesystem.
QStringList compiledPool() {
  return memesIn(u":/resources/memes"_s);
}

// The pool as it exists in the working tree.
QStringList sourcePool() {
  return memesIn(QString::fromUtf8(PJ_MEMES_SOURCE_DIR));
}

// rcc has no globbing, so a meme reaches the splash only if memes.qrc lists
// it explicitly. A file added to the directory but left out of the manifest is
// invisible at runtime with no build error and no warning — it simply never
// shows, and the only symptom is a meme nobody ever sees.
TEST(SplashMemePool, EveryMemeOnDiskIsListedInTheManifest) {
  const QStringList on_disk = sourcePool();
  ASSERT_FALSE(on_disk.isEmpty()) << "no memes found under " << PJ_MEMES_SOURCE_DIR;

  const QStringList compiled = compiledPool();
  for (const QString& meme : on_disk) {
    EXPECT_TRUE(compiled.contains(meme)) << meme.toStdString()
                                         << " exists in resources/memes/ but is missing from memes.qrc, "
                                         << "so the splash can never show it";
  }
}

// A manifest entry pointing at a missing file fails the build in rcc, but one
// pointing at a corrupt or truncated image does not — that surfaces only as a
// blank splash at runtime.
TEST(SplashMemePool, EveryCompiledMemeDecodes) {
  const QStringList compiled = compiledPool();
  ASSERT_FALSE(compiled.isEmpty());

  const QDir dir(u":/resources/memes"_s);
  for (const QString& meme : compiled) {
    EXPECT_FALSE(QPixmap(dir.filePath(meme)).isNull())
        << meme.toStdString() << " is listed in memes.qrc but does not decode; the splash would come up blank";
  }
}

TEST(SplashMemePool, EveryMemeFitsTheSplashBoxOnBothAxes) {
  const QDir dir(u":/resources/memes"_s);
  for (const QString& meme : compiledPool()) {
    const QPixmap capped = capMemeToSplashBox(QPixmap(dir.filePath(meme)));
    EXPECT_LE(capped.width(), kMaxMemeExtent) << meme.toStdString();
    EXPECT_LE(capped.height(), kMaxMemeExtent) << meme.toStdString();
  }
}

TEST(SplashMemePool, CapNeverUpscalesAPoolMeme) {
  const QDir dir(u":/resources/memes"_s);
  for (const QString& meme : compiledPool()) {
    const QPixmap original(dir.filePath(meme));
    const QPixmap capped = capMemeToSplashBox(original);
    EXPECT_LE(capped.width(), original.width()) << meme.toStdString();
    EXPECT_LE(capped.height(), original.height()) << meme.toStdString();
  }
}

// A width-only cap leaves a tall image at full height, which is what put an
// 883px-tall meme on screen. Synthetic sizes pin both orientations regardless of
// what the pool happens to contain today.
TEST(SplashMemePool, CapBoundsPortraitAndLandscapeAlike) {
  QPixmap landscape(1200, 300);
  landscape.fill(Qt::red);
  const QPixmap capped_landscape = capMemeToSplashBox(landscape);
  EXPECT_EQ(capped_landscape.width(), kMaxMemeExtent);
  EXPECT_EQ(capped_landscape.height(), kMaxMemeExtent / 4);

  QPixmap portrait(300, 1200);
  portrait.fill(Qt::red);
  const QPixmap capped_portrait = capMemeToSplashBox(portrait);
  EXPECT_EQ(capped_portrait.height(), kMaxMemeExtent);
  EXPECT_EQ(capped_portrait.width(), kMaxMemeExtent / 4);
}

// Qt::KeepAspectRatio enlarges an image smaller than the target box, so the
// no-upscale rule has to be an explicit guard rather than a property of the
// scale mode.
TEST(SplashMemePool, CapLeavesImagesInsideTheBoxUntouched) {
  QPixmap small(320, 240);
  small.fill(Qt::blue);
  EXPECT_EQ(capMemeToSplashBox(small).size(), small.size());

  QPixmap exactly_at_the_bound(kMaxMemeExtent, kMaxMemeExtent);
  exactly_at_the_bound.fill(Qt::green);
  EXPECT_EQ(capMemeToSplashBox(exactly_at_the_bound).size(), exactly_at_the_bound.size());
}

// The browser build compiles no memes: a funny splash request over an empty pool must still show the serious
// banner, not nothing.
TEST(SplashMemePool, EmptyPoolFallsBackToTheSeriousSplash) {
  unregisterMemePool();
  EXPECT_TRUE(QDir(QStringLiteral(":/resources/memes")).entryList(QDir::Files).isEmpty());
  EXPECT_FALSE(makeStartupSplash().isNull());
  registerMemePool();
}

}  // namespace
}  // namespace PJ

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QStandardPaths::setTestModeEnabled(true);
  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
