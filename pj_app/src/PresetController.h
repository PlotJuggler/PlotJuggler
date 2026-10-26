#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <functional>

#include "pj_runtime/PresetCatalog.h"

class QNetworkReply;

namespace PJ {

/**
 * @brief Fetches the files of a browser preset (index, layout, thumbnails, data), one at a time,
 * and discovers the presets a page publishes.
 *
 * Every download is buffered whole and capped: at `kMaxPresetDownloadBytes` (PresetCatalog.h) unless
 * the caller passes a limit (a recording takes `kMaxPresetDataBytes`).
 * GUI thread only; asynchronous, never nests an event loop. The completion is never called from
 * inside `download()`, after `cancel()`, or after the controller is destroyed.
 */
class PresetController : public QObject {
  Q_OBJECT

 public:
  /// Bytes, final URL, error (empty on success; bytes are empty on failure). Browser caveat:
  /// after a redirect the WASM build still reports the REQUESTED URL as final, where native
  /// builds report the redirected one. A layout's relative references resolve against it, so a
  /// preset must not rely on a redirect to another directory.
  using DownloadDone = std::function<void(QByteArray bytes, QUrl final_url, QString error)>;

  explicit PresetController(QObject* parent = nullptr);
  /// Cancels the download in flight while the controller is whole: `network_` deletes its replies
  /// first, and a browser reply deleted while running would deliver its completion.
  ~PresetController() override;

  /// Starts fetching `url` (http/https only; redirects follow, never to a less secure scheme).
  /// A download already in flight is cancelled first, so its completion is dropped. `done` may
  /// call `download()` again to chain the next file.
  void download(const QUrl& url, DownloadDone done, qint64 limit = kMaxPresetDownloadBytes);

  /// Aborts the download in flight, if any, without calling its completion. Idempotent.
  void cancel();

  /// Discovers the presets published beside `page_url`: downloads `presets/index.json` from the
  /// page's directory (its query and fragment play no part) and emits `discovered` once, later.
  /// Supersedes a download in flight, like download().
  void start(const QUrl& page_url);

 signals:
  /// start()'s outcome. `entries` is empty when the page publishes no index (any failed download
  /// counts as none), an empty one, or a malformed one; only the last is worth a warning, which
  /// start() logs once. `deep_link` is the entry id the page's one `preset` query value names;
  /// `deep_link_error` instead says why a `preset` value cannot be honoured (an unknown id, the
  /// key repeated, no usable index). Both are empty when the page names no preset.
  void discovered(const QList<PJ::PresetEntry>& entries, const QString& deep_link, const QString& deep_link_error);

 private:
  QNetworkAccessManager network_;
  QPointer<QNetworkReply> reply_;
  /// Bumped by every download() and cancel(); a completion delivers only for the generation it
  /// was started under.
  quint64 generation_ = 0;
};

}  // namespace PJ
