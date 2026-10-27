// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "WelcomePresetsDialog.h"

#include <QBuffer>
#include <QFont>
#include <QFrame>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QLayout>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSize>
#include <QVBoxLayout>
#include <cstddef>
#include <utility>

#include "ui_WelcomePresetsDialog.h"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

constexpr int kColumns = 3;
constexpr QSize kThumbnailSize{192, 108};
// A thumbnail decodes whole; an index-supplied image must not claim more than this.
constexpr int kThumbnailAllocationLimitMb = 64;
// The bar's resolution while a download's size is known: a tenth of a percent.
constexpr int kProgressSteps = 1000;
constexpr qint64 kBytesPerMb = 1'000'000;

}  // namespace

WelcomePresetsDialog::WelcomePresetsDialog(QList<PresetEntry> entries, QWidget* parent)
    : Dialog(parent), ui_(new Ui::WelcomePresetsDialog), entries_(std::move(entries)) {
  setDialogTitle(tr("Welcome to PlotJuggler"));
  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);

  entry_widgets_.reserve(entries_.size());
  for (int index = 0; index < entries_.size(); ++index) {
    addEntryCard(entries_[index], index);
  }
  ui_->gridPresets->setRowStretch(ui_->gridPresets->rowCount(), 1);  // cards pack at the top
  connect(ui_->buttonCancel, &QPushButton::clicked, this, &WelcomePresetsDialog::cancelRequested);
  connect(ui_->buttonRetry, &QPushButton::clicked, this, [this]() {
    const QString id = retry_id_;  // selectPreset clears it
    selectPreset(id);
  });
  connect(ui_->buttonUpload, &QPushButton::clicked, this, [this]() {
    close();
    emit uploadRequested();
  });
  resize(720, 460);
  updateControls();
  fetchThumbnails(0);
}

WelcomePresetsDialog::~WelcomePresetsDialog() {
  delete ui_;
}

void WelcomePresetsDialog::selectPreset(const QString& id) {
  const PresetEntry* selected = findPreset(entries_, id);
  if (selected == nullptr || !loading_id_.isEmpty()) {
    return;
  }
  loading_id_ = id;
  retry_id_.clear();
  ui_->progressLoading->setRange(0, 0);
  ui_->labelStatus->setText(tr("Loading %1…").arg(selected->title));
  updateControls();
  emit presetSelected(id, selected->layout_url);
}

void WelcomePresetsDialog::showError(const QString& message) {
  retry_id_.clear();
  ui_->labelStatus->setText(message);
  updateControls();
}

void WelcomePresetsDialog::layoutLoadFinished(const QUrl& url, bool ok, const QString& reason) {
  const PresetEntry* loading = findPreset(entries_, loading_id_);
  if (loading == nullptr || loading->layout_url != url) {
    return;
  }
  loading_id_.clear();
  if (ok || reason == "cancelled"_L1) {
    ui_->labelStatus->clear();
  } else {
    retry_id_ = loading->id;
    // The warning box the shell showed already says why; `reason` is a log token.
    ui_->labelStatus->setText(
        reason == "busy"_L1 ? tr("%1 could not be loaded: another layout is still loading.").arg(loading->title)
                            : tr("%1 could not be loaded.").arg(loading->title));
  }
  updateControls();
  if (ok) {
    close();
  }
}

void WelcomePresetsDialog::layoutLoadProgress(const QUrl& url, qint64 received, qint64 total) {
  const PresetEntry* loading = findPreset(entries_, loading_id_);
  if (loading == nullptr || loading->layout_url != url) {
    return;
  }
  // The title is untrusted: it goes in through a multi-arg arg(), so a "%2" in it is never substituted.
  if (total >= 0 && received >= total) {
    ui_->progressLoading->setRange(0, 0);
    ui_->labelStatus->setText(tr("Opening %1…").arg(loading->title));
  } else if (total > 0) {
    ui_->progressLoading->setRange(0, kProgressSteps);
    ui_->progressLoading->setValue(static_cast<int>(received * kProgressSteps / total));
    ui_->labelStatus->setText(
        tr("Downloading %1: %2 of %3 MB")
            .arg(loading->title, QString::number(received / kBytesPerMb), QString::number(total / kBytesPerMb)));
  } else {
    ui_->progressLoading->setRange(0, 0);
    ui_->labelStatus->setText(tr("Downloading %1: %2 MB").arg(loading->title, QString::number(received / kBytesPerMb)));
  }
}

void WelcomePresetsDialog::reject() {
  if (!loading_id_.isEmpty()) {
    emit cancelRequested();
  }
  Dialog::reject();
}

void WelcomePresetsDialog::addEntryCard(const PresetEntry& preset, int index) {
  auto* card = new QFrame(ui_->presetList);
  card->setObjectName(u"presetCard"_s);
  card->setFrameShape(QFrame::StyledPanel);
  auto* layout = new QVBoxLayout(card);

  auto* thumbnail = new QLabel(card);
  thumbnail->setObjectName(u"labelPresetThumbnail"_s);
  thumbnail->setProperty("presetId", preset.id);
  thumbnail->setFixedSize(kThumbnailSize);
  thumbnail->setAlignment(Qt::AlignCenter);
  thumbnail->setFrameShape(QFrame::StyledPanel);
  thumbnail->setText(tr("No preview"));
  thumbnail->setProperty("thumbnail", preset.thumbnail_url.isEmpty() ? u"placeholder"_s : u"loading"_s);

  // The index is untrusted: its text is never interpreted as markup.
  auto* title = new QLabel(preset.title, card);
  title->setObjectName(u"labelPresetTitle"_s);
  title->setTextFormat(Qt::PlainText);
  QFont title_font = title->font();
  title_font.setBold(true);
  title->setFont(title_font);
  auto* description = new QLabel(preset.description, card);
  description->setObjectName(u"labelPresetDescription"_s);
  description->setTextFormat(Qt::PlainText);
  description->setWordWrap(true);
  description->setVisible(!preset.description.isEmpty());

  auto* open = new QPushButton(tr("Open"), card);
  open->setObjectName(u"buttonOpenPreset"_s);
  open->setProperty("presetId", preset.id);
  connect(open, &QPushButton::clicked, this, [this, id = preset.id]() { selectPreset(id); });

  layout->addWidget(thumbnail, 0, Qt::AlignHCenter);
  layout->addWidget(title);
  layout->addWidget(description);
  layout->addStretch();
  layout->addWidget(open);
  // No alignment: the card fills its cell, so a row's cards share the tallest one's height and the stretch above
  // keeps every Open button on the bottom line.
  ui_->gridPresets->addWidget(card, index / kColumns, index % kColumns);
  entry_widgets_.push_back({thumbnail, open});
}

void WelcomePresetsDialog::fetchThumbnails(int index) {
  // One at a time: a new download() cancels the one in flight.
  while (index < entries_.size() && entries_[index].thumbnail_url.isEmpty()) {
    ++index;
  }
  if (index >= entries_.size()) {
    return;
  }
  thumbnails_.download(
      entries_[index].thumbnail_url, [this, index](const QByteArray& bytes, const QUrl&, const QString&) {
        setThumbnail(index, bytes);  // empty on failure
        fetchThumbnails(index + 1);
      });
}

void WelcomePresetsDialog::setThumbnail(int index, const QByteArray& bytes) {
  QLabel* label = entry_widgets_[static_cast<std::size_t>(index)].thumbnail;
  QBuffer buffer;
  buffer.setData(bytes);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer);
  reader.setAllocationLimit(kThumbnailAllocationLimitMb);
  const QImage image = bytes.isEmpty() ? QImage() : reader.read();
  if (image.isNull()) {
    label->setProperty("thumbnail", u"placeholder"_s);
    return;
  }
  const qreal ratio = devicePixelRatioF();
  QPixmap pixmap =
      QPixmap::fromImage(image.scaled(kThumbnailSize * ratio, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  pixmap.setDevicePixelRatio(ratio);
  label->setPixmap(pixmap);
  label->setProperty("thumbnail", u"image"_s);
}

void WelcomePresetsDialog::updateControls() {
  const bool loading = !loading_id_.isEmpty();
  for (const EntryWidgets& widgets : entry_widgets_) {
    widgets.open->setEnabled(!loading);
  }
  ui_->buttonUpload->setEnabled(!loading);
  ui_->progressLoading->setVisible(loading);
  ui_->buttonCancel->setVisible(loading);
  ui_->buttonRetry->setVisible(!retry_id_.isEmpty());
  ui_->labelStatus->setVisible(!ui_->labelStatus->text().isEmpty());
}

}  // namespace PJ
