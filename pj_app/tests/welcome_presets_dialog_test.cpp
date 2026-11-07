// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The browser welcome dialog lists a deployment's presets beside "Upload your
// own…". Picking one reports it and shows the load until the shell says how it
// ended: Cancel asks to stop it, Retry repeats a failed one, success closes the
// dialog. A thumbnail that cannot be fetched or decoded leaves its entry on the
// placeholder and never blocks it.

#include <gtest/gtest.h>

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QList>
#include <QPoint>
#include <QProgressBar>
#include <QPushButton>
#include <QRect>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QToolButton>
#include <QUrl>

#include "WelcomePresetsDialog.h"
#include "http_test_utils.h"
#include "pj_runtime/PresetCatalog.h"

using namespace Qt::StringLiterals;
using PJ::test::LocalHttpServer;

namespace {

PJ::PresetEntry presetEntry(const QString& id, const QUrl& thumbnail_url = {}) {
  return PJ::PresetEntry{
      .id = id,
      .title = id.toUpper(),
      .description = u"About "_s + id,
      .layout_url = QUrl(u"https://presets.test/%1/layout.pj4.xml"_s.arg(id)),
      .thumbnail_url = thumbnail_url,
  };
}

// The widget of `id`'s entry with `object_name` (each entry carries its id).
template <typename Widget>
Widget* entryWidget(const PJ::WelcomePresetsDialog& dialog, const QString& object_name, const QString& id) {
  for (Widget* widget : dialog.findChildren<Widget*>(object_name)) {
    if (widget->property("presetId").toString() == id) {
      return widget;
    }
  }
  return nullptr;
}

QString thumbnailState(const PJ::WelcomePresetsDialog& dialog, const QString& id) {
  const auto* label = entryWidget<QLabel>(dialog, u"labelPresetThumbnail"_s, id);
  return label != nullptr ? label->property("thumbnail").toString() : u"<no label>"_s;
}

QByteArray pngBytes() {
  QImage image(8, 8, QImage::Format_RGB32);
  image.fill(Qt::darkCyan);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return bytes;
}

TEST(WelcomePresetsDialogTest, SelectionUploadAndCancel) {
  const PJ::PresetEntry first = presetEntry(u"first"_s);
  const PJ::PresetEntry second = presetEntry(u"second"_s);
  PJ::WelcomePresetsDialog dialog({first, second});
  QSignalSpy selected(&dialog, &PJ::WelcomePresetsDialog::presetSelected);
  QSignalSpy upload(&dialog, &PJ::WelcomePresetsDialog::uploadRequested);
  QSignalSpy cancel(&dialog, &PJ::WelcomePresetsDialog::cancelRequested);
  dialog.show();

  auto* open_first = entryWidget<QPushButton>(dialog, u"buttonOpenPreset"_s, first.id);
  auto* open_second = entryWidget<QPushButton>(dialog, u"buttonOpenPreset"_s, second.id);
  auto* cancel_button = dialog.findChild<QPushButton*>(u"buttonCancel"_s);
  auto* retry_button = dialog.findChild<QPushButton*>(u"buttonRetry"_s);
  auto* upload_button = dialog.findChild<QPushButton*>(u"buttonUpload"_s);
  auto* status = dialog.findChild<QLabel*>(u"labelStatus"_s);
  ASSERT_NE(open_first, nullptr);
  ASSERT_NE(open_second, nullptr);
  ASSERT_NE(cancel_button, nullptr);
  ASSERT_NE(retry_button, nullptr);
  ASSERT_NE(upload_button, nullptr);
  ASSERT_NE(status, nullptr);
  EXPECT_FALSE(cancel_button->isVisible());
  EXPECT_FALSE(retry_button->isVisible());
  EXPECT_FALSE(status->isVisible());

  // Picking a preset reports it and shows the load until the shell settles it.
  open_second->click();
  ASSERT_EQ(selected.size(), 1);
  EXPECT_EQ(selected.at(0).at(0).toString(), second.id);
  EXPECT_TRUE(cancel_button->isVisible());
  EXPECT_TRUE(status->text().contains(second.title)) << status->text().toStdString();
  EXPECT_FALSE(open_first->isEnabled());
  EXPECT_FALSE(upload_button->isEnabled());

  // Cancel asks the shell; only this load's terminal brings the entries back.
  cancel_button->click();
  EXPECT_EQ(cancel.size(), 1);
  dialog.layoutLoadFinished(first.layout_url, false, u"busy"_s);
  EXPECT_TRUE(cancel_button->isVisible());
  dialog.layoutLoadFinished(second.layout_url, false, u"cancelled"_s);
  EXPECT_FALSE(cancel_button->isVisible());
  EXPECT_FALSE(retry_button->isVisible());
  EXPECT_TRUE(open_first->isEnabled());
  EXPECT_TRUE(upload_button->isEnabled());

  // A failure says so and offers to repeat the same preset.
  open_first->click();
  dialog.layoutLoadFinished(first.layout_url, false, u"download"_s);
  EXPECT_TRUE(retry_button->isVisible());
  EXPECT_FALSE(cancel_button->isVisible());
  EXPECT_TRUE(status->isVisible());
  // A sentence for the user; the reason token is for the log.
  EXPECT_EQ(status->text(), u"FIRST could not be loaded."_s);
  EXPECT_TRUE(open_second->isEnabled());
  retry_button->click();
  ASSERT_EQ(selected.size(), 3);
  EXPECT_EQ(selected.at(2).at(0).toString(), first.id);

  // A busy refusal says why, so Retry later makes sense.
  dialog.layoutLoadFinished(first.layout_url, false, u"busy"_s);
  EXPECT_EQ(status->text(), u"FIRST could not be loaded: another layout is still loading."_s);
  retry_button->click();
  ASSERT_EQ(selected.size(), 4);

  // Success closes the dialog; reopened, it is idle again.
  dialog.layoutLoadFinished(first.layout_url, true, {});
  EXPECT_FALSE(dialog.isVisible());
  dialog.show();
  EXPECT_FALSE(cancel_button->isVisible());
  EXPECT_FALSE(retry_button->isVisible());

  // A deep link selects without a click, through the same path.
  dialog.selectPreset(second.id);
  ASSERT_EQ(selected.size(), 5);
  EXPECT_EQ(selected.at(4).at(0).toString(), second.id);
  EXPECT_TRUE(cancel_button->isVisible());
  dialog.layoutLoadFinished(second.layout_url, false, u"cancelled"_s);

  // Upload closes the dialog and hands over to the data-file picker.
  upload_button->click();
  EXPECT_EQ(upload.size(), 1);
  EXPECT_FALSE(dialog.isVisible());
  EXPECT_EQ(selected.size(), 5);
}

// Closing the dialog while a preset loads stops that load, as Cancel does;
// closing it idle asks for nothing.
TEST(WelcomePresetsDialogTest, ClosingWhileLoadingCancels) {
  const PJ::PresetEntry first = presetEntry(u"first"_s);
  PJ::WelcomePresetsDialog dialog({first});
  QSignalSpy cancel(&dialog, &PJ::WelcomePresetsDialog::cancelRequested);
  auto* close_button = dialog.findChild<QToolButton*>(u"buttonClose"_s);
  ASSERT_NE(close_button, nullptr);

  dialog.show();
  close_button->click();
  EXPECT_FALSE(dialog.isVisible());
  EXPECT_EQ(cancel.size(), 0);

  dialog.show();
  dialog.selectPreset(first.id);
  close_button->click();
  EXPECT_FALSE(dialog.isVisible());
  EXPECT_EQ(cancel.size(), 1);

  // Escape closes it the same way.
  dialog.layoutLoadFinished(first.layout_url, false, u"cancelled"_s);
  dialog.show();
  dialog.selectPreset(first.id);
  QTest::keyClick(&dialog, Qt::Key_Escape);
  EXPECT_FALSE(dialog.isVisible());
  EXPECT_EQ(cancel.size(), 2);
}

// While the picked preset's recording downloads, the bar shows how much of it has arrived; once it has, the bar
// goes busy again while the app opens it. Progress for another URL is not this load's.
TEST(WelcomePresetsDialogTest, DownloadProgressFillsTheBar) {
  const PJ::PresetEntry first = presetEntry(u"first"_s);
  const PJ::PresetEntry second = presetEntry(u"second"_s);
  PJ::WelcomePresetsDialog dialog({first, second});
  dialog.show();
  auto* bar = dialog.findChild<QProgressBar*>(u"progressLoading"_s);
  auto* status = dialog.findChild<QLabel*>(u"labelStatus"_s);
  auto* retry_button = dialog.findChild<QPushButton*>(u"buttonRetry"_s);
  ASSERT_NE(bar, nullptr);
  ASSERT_NE(status, nullptr);
  ASSERT_NE(retry_button, nullptr);

  dialog.selectPreset(first.id);
  EXPECT_EQ(bar->maximum(), 0);  // busy until a byte count arrives

  dialog.layoutLoadProgress(first.layout_url, 120'000'000, 261'000'000);
  EXPECT_EQ(bar->minimum(), 0);
  EXPECT_EQ(bar->maximum(), 1000);
  EXPECT_EQ(bar->value(), 459);
  EXPECT_EQ(status->text(), u"Downloading FIRST: 120 of 261 MB"_s);

  dialog.layoutLoadProgress(second.layout_url, 1, 2);
  EXPECT_EQ(bar->value(), 459);
  EXPECT_EQ(status->text(), u"Downloading FIRST: 120 of 261 MB"_s);

  // No declared size: the count without a percentage.
  dialog.layoutLoadProgress(first.layout_url, 130'000'000, -1);
  EXPECT_EQ(bar->maximum(), 0);
  EXPECT_EQ(status->text(), u"Downloading FIRST: 130 MB"_s);

  dialog.layoutLoadProgress(first.layout_url, 261'000'000, 261'000'000);
  EXPECT_EQ(bar->maximum(), 0);
  EXPECT_EQ(status->text(), u"Opening FIRST…"_s);

  // The terminal still decides; Retry starts over on a busy bar.
  dialog.layoutLoadFinished(first.layout_url, false, u"download"_s);
  EXPECT_EQ(status->text(), u"FIRST could not be loaded."_s);
  dialog.layoutLoadProgress(first.layout_url, 1, 2);  // no load shown: ignored
  EXPECT_EQ(status->text(), u"FIRST could not be loaded."_s);
  retry_button->click();
  EXPECT_EQ(bar->maximum(), 0);
  EXPECT_EQ(status->text(), u"Loading FIRST…"_s);
  dialog.layoutLoadProgress(first.layout_url, 0, 261'000'000);
  EXPECT_EQ(bar->maximum(), 1000);
  EXPECT_EQ(bar->value(), 0);
  dialog.layoutLoadFinished(first.layout_url, true, {});
  EXPECT_FALSE(dialog.isVisible());
}

// Cards line up whatever their descriptions' lengths: one height (the tallest's), thumbnails of one size on one
// top, and the Open buttons on one line at the bottom.
TEST(WelcomePresetsDialogTest, CardsShareOneHeightAndAlignment) {
  PJ::PresetEntry wordy = presetEntry(u"wordy"_s);
  wordy.description =
      u"A long description that wraps over several lines of its card, much longer than the others, "
      u"so that its card would be the tallest of the row on its own."_s;
  PJ::PresetEntry terse = presetEntry(u"terse"_s);
  terse.description = u"Short."_s;
  const PJ::PresetEntry plain = presetEntry(u"plain"_s);
  PJ::WelcomePresetsDialog dialog({wordy, terse, plain});
  dialog.resize(720, 460);
  dialog.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&dialog));
  QCoreApplication::processEvents();

  const QList<QFrame*> cards = dialog.findChildren<QFrame*>(u"presetCard"_s);
  ASSERT_EQ(cards.size(), 3);
  const auto top_in_dialog = [&dialog](const QWidget* widget) { return widget->mapTo(&dialog, QPoint(0, 0)).y(); };
  QList<QRect> thumbnails;
  QList<int> open_tops;
  for (const PJ::PresetEntry& entry : {wordy, terse, plain}) {
    const auto* thumbnail = entryWidget<QLabel>(dialog, u"labelPresetThumbnail"_s, entry.id);
    const auto* open = entryWidget<QPushButton>(dialog, u"buttonOpenPreset"_s, entry.id);
    ASSERT_NE(thumbnail, nullptr);
    ASSERT_NE(open, nullptr);
    thumbnails.push_back(QRect(QPoint(0, top_in_dialog(thumbnail)), thumbnail->size()));
    open_tops.push_back(top_in_dialog(open));
  }
  for (int index = 1; index < 3; ++index) {
    EXPECT_EQ(cards[index]->height(), cards[0]->height()) << index;
    EXPECT_EQ(top_in_dialog(cards[index]), top_in_dialog(cards[0])) << index;
    EXPECT_EQ(thumbnails[index], thumbnails[0]) << index;
    EXPECT_EQ(open_tops[index], open_tops[0]) << index;
  }
}

TEST(WelcomePresetsDialogTest, BrokenThumbnailKeepsThePlaceholder) {
  LocalHttpServer image;
  image.setResponse("200 OK", pngBytes());
  LocalHttpServer missing;
  missing.setResponse("404 Not Found", "missing");
  LocalHttpServer garbage;
  garbage.setResponse("200 OK", "not an image");
  PJ::WelcomePresetsDialog dialog({
      presetEntry(u"image"_s, image.url()),
      presetEntry(u"missing"_s, missing.url()),
      presetEntry(u"garbage"_s, garbage.url()),
      presetEntry(u"none"_s),
  });
  dialog.show();

  EXPECT_EQ(thumbnailState(dialog, u"none"_s), u"placeholder"_s);
  ASSERT_TRUE(
      QTest::qWaitFor(
          [&] {
            return thumbnailState(dialog, u"image"_s) == u"image"_s &&
                   thumbnailState(dialog, u"missing"_s) == u"placeholder"_s &&
                   thumbnailState(dialog, u"garbage"_s) == u"placeholder"_s;
          },
          10000))
      << thumbnailState(dialog, u"image"_s).toStdString() << " " << thumbnailState(dialog, u"missing"_s).toStdString()
      << " " << thumbnailState(dialog, u"garbage"_s).toStdString();
  for (const QString& id : {u"missing"_s, u"garbage"_s, u"none"_s}) {
    auto* open = entryWidget<QPushButton>(dialog, u"buttonOpenPreset"_s, id);
    ASSERT_NE(open, nullptr);
    EXPECT_TRUE(open->isEnabled()) << id.toStdString();
  }
}

}  // namespace
