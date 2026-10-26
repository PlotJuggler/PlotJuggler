// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PresetController.h"

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QTimer>
#include <QUrlQuery>
#include <chrono>
#include <utility>

#include "pj_runtime/HttpGet.h"
#include "pj_runtime/HttpUrl.h"
#include "pj_runtime/PresetCatalog.h"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// An inactivity timeout, not wall clock, so it suits a body of up to the cap.
constexpr std::chrono::seconds kTransferTimeout{30};
constexpr int kMaxRedirects = 4;

}  // namespace

PresetController::PresetController(QObject* parent) : QObject(parent) {}

PresetController::~PresetController() {
  cancel();
}

void PresetController::download(const QUrl& url, DownloadDone done, qint64 limit) {
  cancel();
  get(url, limit, [done = std::move(done)](QByteArray bytes, QUrl final_url, QString error, bool) {
    done(std::move(bytes), std::move(final_url), std::move(error));
  });
}

void PresetController::downloadToFile(const QUrl& url, const QString& path, qint64 limit, FileDownloadDone done) {
  cancel();
#ifdef PJ_TARGET_WASM
  const quint64 generation = generation_;
  fetch_ = browserFetchToFile(
      url, path, limit, kTransferTimeout, [this, generation, done = std::move(done)](FileDownload file) {
        if (generation == generation_) {
          fetch_ = 0;
          done(std::move(file));
        }
      });
#else
  get(url, limit,
      [path, done = std::move(done)](const QByteArray& bytes, const QUrl&, QString error, bool limit_exceeded) {
        FileDownload file;
        file.error = std::move(error);
        file.limit_exceeded = limit_exceeded;
        if (file.error.isEmpty()) {
          QFile output(path);
          if (output.open(QIODevice::WriteOnly | QIODevice::Truncate) && output.write(bytes) == bytes.size() &&
              output.flush()) {
            file.size = bytes.size();
            file.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
          } else {
            file.error = tr("could not write %1: %2").arg(path, output.errorString());
          }
        }
        done(std::move(file));
      });
#endif
}

void PresetController::get(const QUrl& url, qint64 limit, Fetched done) {
  const quint64 generation = generation_;
  if (!isHttpUrl(url)) {
    // Native QNetworkAccessManager would read file:// URLs: a preset names its files, so refuse here.
    QTimer::singleShot(0, this, [this, generation, url, done = std::move(done)]() {
      if (generation == generation_) {
        done({}, url, tr("only http(s) preset files can be downloaded: %1").arg(url.toString()), false);
      }
    });
    return;
  }
#ifdef PJ_TARGET_WASM
  // The browser reports the requested URL as final (DownloadDone).
  fetch_ = browserFetchToBytes(
      url, limit, kTransferTimeout, this,
      [this, generation, url, done = std::move(done)](QByteArray bytes, QString error, bool limit_exceeded) {
        if (generation == generation_) {
          fetch_ = 0;
          done(std::move(bytes), url, std::move(error), limit_exceeded);
        }
      });
#else
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setMaximumRedirectsAllowed(kMaxRedirects);
  reply_ = httpGetWithLimit(
      network_, request, limit, kTransferTimeout, this,
      [this, generation, limit, done = std::move(done)](QNetworkReply& reply, bool limit_exceeded) {
        if (generation != generation_) {
          return;  // cancelled or superseded
        }
        reply_.clear();
        if (limit_exceeded) {
          done({}, reply.url(), tr("download exceeds the %1 MiB limit").arg(limit / (1024 * 1024)), true);
        } else if (reply.error() != QNetworkReply::NoError) {
          done({}, reply.url(), reply.errorString(), false);
        } else {
          done(reply.readAll(), reply.url(), QString(), false);
        }
      });
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
  ++generation_;
#ifdef PJ_TARGET_WASM
  cancelBrowserFetch(std::exchange(fetch_, 0));
#endif
  if (reply_) {
    reply_->abort();  // finished fires synchronously and sees the stale generation
  }
  reply_.clear();
}

}  // namespace PJ
