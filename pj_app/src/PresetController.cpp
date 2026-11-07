// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PresetController.h"

#include <QCryptographicHash>
#include <QFile>
#include <QStringList>
#include <QUrlQuery>
#include <chrono>
#include <utility>

#include "pj_runtime/PresetCatalog.h"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// An inactivity timeout, not wall clock, so it suits a body of up to the cap.
constexpr std::chrono::seconds kTransferTimeout{30};

}  // namespace

PresetController::PresetController(QObject* parent) : QObject(parent) {}

PresetController::~PresetController() {
  cancel();
}

void PresetController::download(const QUrl& url, DownloadDone done, qint64 limit) {
  cancel();
  cancel_fetch_ =
      fetchBytesWithLimit(url, limit, kTransferTimeout, this, [done = std::move(done)](FetchedBytes fetched) {
        done(std::move(fetched.bytes), std::move(fetched.final_url), std::move(fetched.error));
      });
}

void PresetController::downloadToFile(
    const QUrl& url, const QString& path, qint64 limit, FileDownloadDone done, FetchProgress progress,
    const QString& pinned_sha256) {
  cancel();
#ifdef PJ_TARGET_WASM
  const quint32 id =
      browserFetchToFile(url, path, limit, kTransferTimeout, std::move(done), std::move(progress), pinned_sha256);
  cancel_fetch_ = [id]() { cancelBrowserFetch(id); };
#else
  Q_UNUSED(pinned_sha256);
  cancel_fetch_ = fetchBytesWithLimit(
      url, limit, kTransferTimeout, this,
      [path, done = std::move(done)](FetchedBytes fetched) {
        FileDownload file;
        file.error = std::move(fetched.error);
        file.limit_exceeded = fetched.limit_exceeded;
        if (file.error.isEmpty()) {
          QFile output(path);
          const QByteArray& bytes = fetched.bytes;
          if (output.open(QIODevice::WriteOnly | QIODevice::Truncate) && output.write(bytes) == bytes.size() &&
              output.flush()) {
            file.size = bytes.size();
            file.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
          } else {
            file.error = tr("could not write %1: %2").arg(path, output.errorString());
          }
        }
        done(std::move(file));
      },
      nullptr, std::move(progress));
#endif
}

void PresetController::start(const QUrl& page_url) {
  // A relative path reference keeps the base's directory and drops its query and fragment.
  const QUrl index_url = page_url.resolved(QUrl(u"presets/index.json"_s));
  const QStringList deep_links = QUrlQuery(page_url).allQueryItemValues(u"preset"_s, QUrl::FullyDecoded);
  download(index_url, [this, index_url, deep_links](const QByteArray& bytes, const QUrl&, const QString& error) {
    QList<PresetEntry> entries;
    QString catalog_error;
    if (error.isEmpty()) {
      entries = parsePresetCatalog(bytes, index_url, &catalog_error);
      if (!catalog_error.isEmpty()) {
        qWarning(
            "PJ_PRESET_INDEX_INVALID url=%s reason=%s", qPrintable(index_url.toString()), qPrintable(catalog_error));
      }
    }
    QString deep_link;
    QString deep_link_error;
    if (deep_links.size() > 1) {
      deep_link_error = tr("This page link names more than one preset (%1).").arg(deep_links.join(u", "_s));
    } else if (deep_links.size() == 1) {
      const QString& id = deep_links.front();
      if (findPreset(entries, id) != nullptr) {
        deep_link = id;
      } else if (!catalog_error.isEmpty()) {
        deep_link_error = tr("Preset '%1' cannot be opened: this page's preset list is invalid.").arg(id);
      } else {
        deep_link_error = tr("This page has no preset named '%1'.").arg(id);
      }
    }
    emit discovered(entries, deep_link, deep_link_error);
  });
}

void PresetController::cancel() {
  if (const CancelFetch cancel_fetch = std::exchange(cancel_fetch_, {})) {
    cancel_fetch();
  }
}

}  // namespace PJ
