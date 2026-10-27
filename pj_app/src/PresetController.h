#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>

#include "pj_runtime/BrowserFetch.h"
#include "pj_runtime/HttpGet.h"
#include "pj_runtime/PresetCatalog.h"

namespace PJ {

/**
 * @brief Fetches the files of a browser preset (index, layout, thumbnails, data), one at a time,
 * and discovers the presets a page publishes.
 *
 * Every download is capped: at `kMaxPresetDownloadBytes` (PresetCatalog.h) unless the caller passes a
 * limit (a recording takes `kMaxPresetDataBytes`). Files go through `pj_runtime`'s fetchBytesWithLimit() (HttpGet.h);
 * a browser recording streams to its file through BrowserFetch.h.
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
  /// Cancels the download in flight: a browser downloadToFile() is not tied to the controller's lifetime.
  ~PresetController() override;

  /// Starts fetching `url` (http/https only; redirects follow, never to a less secure scheme).
  /// A download already in flight is cancelled first, so its completion is dropped. `done` may
  /// call `download()` again to chain the next file.
  void download(const QUrl& url, DownloadDone done, qint64 limit = kMaxPresetDownloadBytes);

  /// What downloadToFile() wrote (its size and SHA-256), or why it did not.
  using FileDownload = BrowserFetchResult;
  using FileDownloadDone = std::function<void(FileDownload file)>;

  /// Writes `url`'s body into the file `path`, created or truncated. The browser streams it there, so a
  /// recording never passes through the wasm heap; the desktop writes it once downloaded. The same
  /// cancellation and http(s) rules as download(); over `limit` sets `limit_exceeded`. The caller owns
  /// `path`, also after a failure or a cancel. `progress` reports the bytes received against the declared size (-1:
  /// unknown), throttled to about ten calls a second plus the last, always before `done` and never after a cancel.
  /// `pinned_sha256`, the SHA-256 the caller will require of the file, lets the browser keep it across visits
  /// (browserFetchToFile()); the desktop ignores it.
  void downloadToFile(
      const QUrl& url, const QString& path, qint64 limit, FileDownloadDone done, FetchProgress progress = {},
      const QString& pinned_sha256 = {});

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
  /// Drops the transfer in flight, so its completion never runs; empty when none was started.
  CancelFetch cancel_fetch_;
};

}  // namespace PJ
