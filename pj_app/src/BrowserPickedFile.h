// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Browser-only (WebAssembly). Lazily read browser picks: a picked File is never copied, neither into the wasm heap
// nor into MEMFS. Its staged path becomes a read-only MEMFS node that reads straight from the File, so a pick can be
// larger than the wasm address space and costs no memory while it is open. The page side is browser_picked_file.js.
//
// Costs, all on the browser main thread (every pthread's file syscalls are proxied there): a read that misses the
// node's 1 MiB read-ahead block blocks that thread for one Worker round trip, about 0.1-0.2 ms plus the copy.
// A File changed or deleted on disk after the pick makes its reads fail with EIO. A Worker that errors, or a read it
// leaves unanswered for 30 s, retires both Workers: every mounted pick then fails its reads (and pending hashes)
// until the next pick starts new ones.
//
// All functions run on the browser main (GUI) thread; every callback runs later, from its event loop.

#include <QString>
#include <QtGlobal>
#include <functional>

namespace PJ::browser_picked_file {

/// Reports a pick: `pick_id` names the File kept page-side for mount() or forget(); `browser_name` is the File's
/// name, empty when the user cancelled.
using PickCallback = std::function<void(quint32 pick_id, QString browser_name)>;

/// Shows the browser's file picker; `accept` is an <input type=file> accept list (empty: any file). Must run inside a
/// user gesture. A picked File is kept page-side until mount() or forget() takes it.
void pick(const QString& accept, PickCallback callback);

/// Makes `path` (absent; its directory exists) a read-only file served from the File of `pick_id`, which mount()
/// takes over. Removing the path releases the File. False when `pick_id` holds no File.
[[nodiscard]] bool mount(quint32 pick_id, const QString& path);

/// Drops the File of a pick that will not be mounted. Unknown ids are ignored.
void forget(quint32 pick_id);

/// Computes the lowercase hex SHA-256 of the File mounted at `path`, in a Worker, never on the main thread:
/// `done` gets an empty string when the File cannot be read whole (changed or removed since the pick). The hash
/// runs at about 100 MB/s; reads of the mounted file never wait for it.
void hash(const QString& path, std::function<void(QString sha256)> done);

/// Runs `handler(path)` once per mounted file whose read first fails (its File changed or was removed on disk since
/// the pick; every later read fails too), from the event loop, after the failed read returned. One handler for the
/// process; a later call replaces it, an empty one removes it.
void setReadFailureHandler(std::function<void(const QString& path)> handler);

}  // namespace PJ::browser_picked_file
