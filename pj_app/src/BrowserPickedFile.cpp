// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "BrowserPickedFile.h"

#include <emscripten/emscripten.h>

#include <QLoggingCategory>
#include <cstdlib>
#include <unordered_map>
#include <utility>

// Implemented in browser_picked_file.js.
extern "C" {
void pj_browser_pick_file(unsigned id, const char* accept_utf8);
int pj_browser_pick_mount(unsigned pick_id, const char* path_utf8);
void pj_browser_pick_forget(unsigned pick_id);
void pj_browser_pick_hash(unsigned request_id, const char* path_utf8);
}

namespace PJ::browser_picked_file {
namespace {

Q_LOGGING_CATEGORY(lcBrowserPick, "pj.app.browserpick")

// Picks whose <input> has not settled yet, by id.
std::unordered_map<quint32, PickCallback>& pendingPicks() {
  static std::unordered_map<quint32, PickCallback> instance;
  return instance;
}

// Hashes still running in the hasher Worker, by request id.
std::unordered_map<quint32, std::function<void(QString)>>& pendingHashes() {
  static std::unordered_map<quint32, std::function<void(QString)>> instance;
  return instance;
}

// The one setReadFailureHandler() handler; empty when none is set.
std::function<void(const QString&)>& readFailureHandler() {
  static std::function<void(const QString&)> instance;
  return instance;
}

}  // namespace

void pick(const QString& accept, PickCallback callback) {
  static quint32 last_id = 0;
  const quint32 id = ++last_id;
  pendingPicks()[id] = std::move(callback);
  pj_browser_pick_file(id, accept.toUtf8().constData());
}

bool mount(quint32 pick_id, const QString& path) {
  return pj_browser_pick_mount(pick_id, path.toUtf8().constData()) != 0;
}

void forget(quint32 pick_id) {
  pj_browser_pick_forget(pick_id);
}

void hash(const QString& path, std::function<void(QString sha256)> done) {
  static quint32 last_id = 0;
  const quint32 id = ++last_id;
  pendingHashes()[id] = std::move(done);
  pj_browser_pick_hash(id, path.toUtf8().constData());
}

void setReadFailureHandler(std::function<void(const QString& path)> handler) {
  readFailureHandler() = std::move(handler);
}

}  // namespace PJ::browser_picked_file

/// The <input> of pick `id` settled; `name` is malloc'ed by the page and freed here.
extern "C" EMSCRIPTEN_KEEPALIVE void pj_browser_pick_done(quint32 id, char* name) {
  const QString browser_name = QString::fromUtf8(name);
  free(name);
  auto node = PJ::browser_picked_file::pendingPicks().extract(id);
  if (node.empty()) {
    PJ::browser_picked_file::forget(id);
    return;
  }
  node.mapped()(id, browser_name);
}

/// The first read of the mounted file `path` failed; `path` is malloc'ed by the page and freed here.
extern "C" EMSCRIPTEN_KEEPALIVE void pj_browser_pick_read_failed(char* path) {
  const QString mounted_path = QString::fromUtf8(path);
  free(path);
  qCWarning(PJ::browser_picked_file::lcBrowserPick).noquote() << "PJ_BROWSER_PICK_READ_FAILED" << mounted_path;
  if (const auto& handler = PJ::browser_picked_file::readFailureHandler()) {
    handler(mounted_path);
  }
}

/// Hash `id` finished; `hex` is malloc'ed by the page and freed here.
extern "C" EMSCRIPTEN_KEEPALIVE void pj_browser_pick_hash_done(quint32 id, char* hex) {
  QString sha256 = QString::fromLatin1(hex);
  free(hex);
  auto node = PJ::browser_picked_file::pendingHashes().extract(id);
  if (!node.empty()) {
    node.mapped()(std::move(sha256));
  }
}
