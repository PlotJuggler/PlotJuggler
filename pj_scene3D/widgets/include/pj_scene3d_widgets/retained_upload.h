#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <cstddef>
#include <span>

#include "pj_widgets/GraphicsApi.h"  // padForUpload

namespace pj::scene3d {

/// Rewrites a layer's retained vertex array (PointRenderable::vertexBytes())
/// with the records `pack(out, reused)` writes into `out` (the first
/// `max_bytes` bytes of the array; `pack` returns how many it wrote), then pads
/// it for upload (PJ::padForUpload()). The records go into
/// `retained`'s own allocation when nothing else shares it (the view's upload
/// batch lets go once its frame is submitted, if it carried over 1 MiB: QRhi
/// keeps a smaller batch's data) and it is at most twice the size needed: a
/// fresh array costs a page fault per 4 KiB on first write. `reused`
/// tells `pack` which it got. Throws std::bad_alloc when a fresh array cannot
/// be allocated, `retained` then unchanged.
template <typename Pack>
void packIntoRetained(QByteArray& retained, qsizetype max_bytes, Pack&& pack) {
  const qsizetype needed = PJ::paddedUploadSize(max_bytes);
  const bool reused = retained.isDetached() && retained.capacity() >= needed && retained.capacity() <= 2 * needed;
  if (reused) {
    retained.resize(needed);
  } else {
    retained = QByteArray(needed, Qt::Uninitialized);  // resize() would round the capacity up
  }
  const qsizetype used =
      pack(std::as_writable_bytes(std::span<char>(retained.data(), static_cast<std::size_t>(needed))), reused);
  PJ::padForUpload(retained, used);
}

}  // namespace pj::scene3d
