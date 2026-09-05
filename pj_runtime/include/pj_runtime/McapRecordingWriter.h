// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

#include "pj_runtime/RecordingSink.h"

namespace mcap {
class McapReader;
struct Metadata;
}  // namespace mcap

namespace PJ {

/// Scan an open reader (summary already read) for every metadata record named
/// `name`, invoking `on_record` per match in file order. Returns an error only
/// for an unreadable/unparseable record — zero matches is a successful no-op.
/// The one shared implementation of the index→ReadRecord→ParseMetadata walk.
Status forEachMcapMetadata(
    mcap::McapReader& reader, std::string_view name, const std::function<void(const mcap::Metadata&)>& on_record);

/// RecordingSink over mcap::McapWriter. One MCAP channel per binding, one
/// schema per distinct (type_name, encoding, schema_bytes); 1 MiB zstd chunks.
///
/// Chunking policy: small messages share those 1 MiB chunks, while a message of
/// 200 KiB or more gets a chunk to itself. A lazy reader decompresses a whole
/// chunk to reach one message, so co-locating a large payload with many small
/// samples makes each pay for the other; isolating it keeps scalar fetches cheap
/// and lets the large message be fetched, and compressed, on its own.
///
/// The file-level `pj.recording` metadata record is written at open() (start
/// facts) and again at close() (final facts, incl. `truncated`); readers take
/// the last one, so a file abandoned without close() carries no `truncated` or
/// `stopped_utc`. Those are the only two points at which metadata may be
/// written: `McapWriter::write(Metadata)` closes the open chunk, so a
/// mid-recording record would fragment the file.
///
/// Bytes go through a stdio sink that opens the file exclusive-create (an
/// existing path is an error, never truncated) and latches the first
/// write/flush/sync/close failure, so a full disk surfaces as an error instead
/// of a silently truncated recording. close() fsyncs before it fcloses, so an
/// ok Status means the bytes are on stable storage by the time the caller is
/// told the recording ended.
///
/// mcap emits schema and channel records lazily, on the first message that
/// references them, so a binding that never carried a message leaves no trace
/// in the file — replay must not expect an empty channel per binding.
///
/// Threading is the RecordingSink contract and nothing more, so this class
/// holds no locks of its own. Desktop-only: the browser build has no filesystem
/// to record into and does not compile this.
class McapRecordingWriter final : public RecordingSink {
 public:
  McapRecordingWriter();
  /// Closes a file still open as a best effort and never throws; a file it
  /// could not finalize is left footer-less, for `mcap recover`. Every normal
  /// path has already run close(summary) by the time this runs.
  ~McapRecordingWriter() override;

  /// A failed open() — an error returned or an exception thrown past the
  /// exclusive create — leaves no file behind, except when the removal itself
  /// fails, which a returned error then names. Reusable: each open() starts
  /// from a clean writer, so one instance can record many files.
  [[nodiscard]] Status open(const std::filesystem::path& path, const RecordingInfo& info) override;
  [[nodiscard]] Expected<uint16_t> addChannel(const RecordedBinding& binding) override;
  /// Rejects a negative `log_time_ns`: MCAP timestamps are unsigned, and
  /// silently rewriting one to epoch 0 would bury an upstream clock bug.
  [[nodiscard]] Status write(uint16_t channel, int64_t log_time_ns, Span<const uint8_t> bytes) override;
  /// Writes the final metadata and the footer, then fsyncs and closes the
  /// stream. The writer counts as closed from the moment this is entered, so
  /// an exception out of the footer write (an allocation, in practice) is
  /// rethrown with the stream already closed and nothing left for the
  /// destructor to do; the file is then footer-less, exactly as if close()
  /// had returned an error.
  [[nodiscard]] Status close(const RecordingSummary& summary) override;

 private:
  /// Holds the file sink and mcap::McapWriter (keeping <mcap/writer.hpp> out
  /// of this header) plus every piece of per-recording state, so open() can
  /// reset the lot by replacing it.
  struct Impl;
  /// Scope-failure guard for open(): removes the file on any exit but a commit.
  class OpenGuard;
  std::unique_ptr<Impl> impl_;
};

}  // namespace PJ
