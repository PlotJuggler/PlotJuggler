// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/McapRecordingWriter.h"

// The single mcap implementation TU of every executable that links
// pj_runtime: mcap is header-only behind MCAP_IMPLEMENTATION, and the
// reader/writer .inl files share non-inline definitions in types.inl, so
// two TUs defining the macro collide. mcap.hpp (reader + writer) is
// included rather than writer.hpp alone so a consumer that also reads MCAP
// — the round-trip test — links against this one implementation.
#define MCAP_IMPLEMENTATION
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <mcap/mcap.hpp>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "pj_runtime/RecordingFormat.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace PJ {

namespace {

/// Target uncompressed chunk payload. A crash loses at most the open chunk, so
/// this trades a little compression ratio for a bounded loss window.
constexpr uint64_t kChunkSize = 1u << 20;

/// MCAP schema and channel ids are uint16_t and mcap::McapWriter hands them out
/// by incrementing, so past this many they would silently wrap onto live
/// records. Refusing the channel truncates with a real reason instead.
constexpr size_t kMaxRecordIds = 65535;

// Isolation threshold; policy in the header.
constexpr uint64_t kLargeMessageChunkBytes = 200 * 1024;

/// mcap sink over a stdio stream opened exclusive-create. Two reasons not to
/// use mcap's stock FileWriter: it drops short-write/flush/close failures
/// outside debug asserts, so a full disk would end a recording silently, and it
/// takes a narrow path, which on Windows mangles anything outside the active
/// code page. The first failure is latched for the caller to surface.
class CheckedFileSink final : public mcap::IWritable {
 public:
  ~CheckedFileSink() override {
    closeStream();
  }

  /// Exclusive create ('x'): an existing file is an error and is never
  /// truncated. Returns false with the reason latched in error().
  [[nodiscard]] bool open(const std::filesystem::path& path) {
    errno = 0;  // a stale errno would otherwise decorate the failure message
#if defined(_WIN32)
    if (::_wfopen_s(&stream_, path.c_str(), L"wbx") != 0) {
      stream_ = nullptr;
    }
#else
    stream_ = std::fopen(path.c_str(), "wbx");
#endif
    if (stream_ == nullptr) {
      latch("could not exclusively create '" + path.string() + "'");
      return false;
    }
    return true;
  }

  void handleWrite(const std::byte* data, uint64_t size) override {
    if (stream_ == nullptr) {
      errno = 0;  // no syscall failed here, so a stale errno would invent a cause
      latch("write after the recording file was closed");
    } else if (error_.empty()) {
      const size_t written = std::fwrite(data, 1, static_cast<size_t>(size), stream_);
      if (written != static_cast<size_t>(size)) {
        latch("filesystem write failed");
      }
    }
    // size_ keeps advancing after a latched error: McapWriter reads size() for
    // its offset bookkeeping, which must stay self-consistent while the file
    // is drained to close().
    size_ += size;
  }

  void end() override {
    closeStream();
  }

  void flush() override {
    if (stream_ == nullptr || !error_.empty()) {
      return;
    }
    if (std::fflush(stream_) != 0) {
      latch("filesystem flush failed");
    }
  }

  [[nodiscard]] uint64_t size() const override {
    return size_;
  }

  /// The first latched failure, empty while the file is healthy.
  [[nodiscard]] const std::string& error() const {
    return error_;
  }

 private:
  /// Records the FIRST failure only, decorated with errno while it is still
  /// fresh: "filesystem flush failed" alone does not say whether the disk filled
  /// up or the mount went read-only. Call it right after the failed call.
  void latch(std::string what) {
    if (!error_.empty()) {
      return;
    }
    error_ = std::move(what);
    if (errno != 0) {
      error_ += ": " + std::error_code(errno, std::generic_category()).message();
    }
  }

  void closeStream() {
    if (stream_ == nullptr) {
      return;
    }
    if (std::fflush(stream_) != 0) {
      latch("filesystem flush failed");
    }
    syncStream();
    if (std::fclose(stream_) != 0) {
      latch("filesystem close failed");
    }
    stream_ = nullptr;
  }

  /// Between the flush and the close: the user is told the recording is saved
  /// as soon as close() returns, so the bytes must be on stable storage first —
  /// otherwise a crash right after that leaves a finished-looking file with
  /// unwritten tail blocks.
  void syncStream() {
    errno = 0;  // a stale errno would otherwise decorate the failure message
#if defined(_WIN32)
    const int handle = ::_fileno(stream_);
    if (handle < 0 || ::_commit(handle) != 0) {
#else
    const int handle = ::fileno(stream_);
    if (handle < 0 || ::fsync(handle) != 0) {
#endif
      latch("filesystem sync failed");
    }
  }

  std::FILE* stream_ = nullptr;
  uint64_t size_ = 0;
  std::string error_;
};

mcap::Metadata recordingMetadata(const RecordingInfo& info, const RecordingSummary* summary) {
  mcap::Metadata metadata;
  metadata.name = kRecordingMetadataName;
  metadata.metadata["version"] = std::to_string(kRecordingFormatVersion);
  metadata.metadata["app_version"] = info.app_version;
  metadata.metadata["capture_id"] = info.capture_id;
  metadata.metadata["source_display_name"] = info.source_display_name;
  metadata.metadata["source_plugin_id"] = info.source_plugin_id;
  metadata.metadata["capture_ordinal"] = std::to_string(info.capture_ordinal);
  metadata.metadata["started_utc"] = info.started_utc;
  if (summary != nullptr) {
    metadata.metadata["stopped_utc"] = summary->stopped_utc;
    metadata.metadata["terminal_cause"] = summary->terminal_cause;
    metadata.metadata["truncated"] = summary->truncated ? "true" : "false";
    metadata.metadata["truncated_reason"] = summary->truncated_reason;
    metadata.metadata["messages"] = std::to_string(summary->messages);
    metadata.metadata["payload_bytes"] = std::to_string(summary->payload_bytes);
    metadata.metadata["dropped_messages"] = std::to_string(summary->dropped_messages);
  }
  return metadata;
}

}  // namespace

struct McapRecordingWriter::Impl {
  // Declared before the writer: ~McapWriter writes the footer through the
  // sink, so the sink must outlive it.
  CheckedFileSink sink;
  mcap::McapWriter writer;
  std::unordered_map<std::string, uint16_t> schema_ids;  ///< schema key → mcap schema id
  std::unordered_map<uint16_t, uint32_t> next_sequence;  ///< channel id → next message sequence
  size_t channels_added = 0;                             ///< guards the 16-bit channel id space
  RecordingInfo info;
  bool open = false;
};

McapRecordingWriter::McapRecordingWriter() : impl_(std::make_unique<Impl>()) {}

/// Undoes an open() that did not get as far as a file carrying its start
/// facts: terminates the writer (so ~McapWriter has no footer to write into a
/// closed stream), closes the stream and removes the file, so the exclusive
/// create of the next attempt is not blocked by an empty stray. Runs from the
/// destructor on any exit not marked committed, which is what makes the
/// allocations between the create and the commit exception-safe; discard()
/// is the explicit form for the returned-error path, where the caller wants
/// to know whether the removal succeeded.
class McapRecordingWriter::OpenGuard {
 public:
  OpenGuard(Impl& impl, const std::filesystem::path& path) noexcept : impl_(impl), path_(path) {}
  ~OpenGuard() {
    if (!done_) {
      (void)discard();
    }
  }
  OpenGuard(const OpenGuard&) = delete;
  OpenGuard& operator=(const OpenGuard&) = delete;

  void commit() noexcept {
    done_ = true;
  }

  /// Returns the removal's error, if any; every other step is best effort and
  /// cannot fail in a way that matters once the file is gone.
  [[nodiscard]] std::error_code discard() {
    done_ = true;
    impl_.writer.terminate();
    impl_.sink.end();
    std::error_code remove_error;
    std::filesystem::remove(path_, remove_error);
    return remove_error;
  }

 private:
  Impl& impl_;
  const std::filesystem::path& path_;
  bool done_ = false;
};

McapRecordingWriter::~McapRecordingWriter() {
  if (!impl_->open) {
    return;
  }
  // Best effort: a close(summary) already ran on every normal path, and a
  // destructor cannot report anything. A throw out of the footer write would
  // otherwise terminate the process; terminate() leaves the file footer-less,
  // which `mcap recover` handles, and makes ~McapWriter a no-op.
  impl_->open = false;
  try {
    impl_->writer.close();
  } catch (...) {
    impl_->writer.terminate();
  }
}

Status McapRecordingWriter::open(const std::filesystem::path& path, const RecordingInfo& info) {
  if (impl_->open) {
    return unexpected("recording writer already open");
  }
  // McapWriter::close() deliberately keeps its schema and channel tables ("can
  // be re-used between files"), which would carry the previous recording's
  // records into this one. Start every file on a freshly built writer.
  impl_ = std::make_unique<Impl>();

  if (!impl_->sink.open(path)) {
    return unexpected("cannot open recording file: " + impl_->sink.error());
  }
  // From here to the commit the file exists but carries nothing a reader can
  // use; the guard removes it on every exit but the commit, thrown ones
  // included, so a failed open() never blocks the next exclusive create.
  OpenGuard guard(*impl_, path);
  const auto abandon = [&guard, &path](std::string message) -> Status {
    if (const std::error_code remove_error = guard.discard(); remove_error) {
      // "No file left behind" would be a lie when the removal fails (read-only
      // remount, permission flip), so name the stray instead.
      message += " (a stray file remains at '" + path.string() + "': " + remove_error.message() + ")";
    }
    return unexpected(std::move(message));
  };

  mcap::McapWriterOptions options("");  // no profile: channels declare their own encodings
  options.compression = mcap::Compression::Zstd;
  options.chunkSize = kChunkSize;
  impl_->writer.open(impl_->sink, options);
  if (const auto written = impl_->writer.write(recordingMetadata(info, nullptr)); !written.ok()) {
    return abandon("cannot write recording metadata: " + written.message);
  }
  if (!impl_->sink.error().empty()) {
    return abandon("cannot write recording metadata: " + impl_->sink.error());
  }
  impl_->info = info;
  impl_->open = true;
  guard.commit();
  return okStatus();
}

Expected<uint16_t> McapRecordingWriter::addChannel(const RecordedBinding& binding) {
  if (!impl_->open) {
    return unexpected("recording writer not open");
  }
  if (impl_->channels_added >= kMaxRecordIds) {
    return unexpected("recording is out of MCAP channel ids (" + std::to_string(kMaxRecordIds) + ")");
  }
  // Length-prefixed rather than separator-joined: no field's content can forge
  // a boundary, so distinct signatures can never collide onto one schema.
  const auto sized = [](const std::string& field) { return std::to_string(field.size()) + ':' + field; };
  const std::string schema_key = sized(binding.type_name) + sized(binding.encoding) + sized(binding.schema_bytes);
  uint16_t schema_id = 0;
  if (const auto found = impl_->schema_ids.find(schema_key); found != impl_->schema_ids.end()) {
    schema_id = found->second;
  } else {
    if (impl_->schema_ids.size() >= kMaxRecordIds) {
      return unexpected("recording is out of MCAP schema ids (" + std::to_string(kMaxRecordIds) + ")");
    }
    // The binding's parser encoding is recorded as BOTH the schema encoding
    // and the message encoding, so the loader selects the same parser the
    // live session used.
    mcap::Schema schema(binding.type_name, binding.encoding, binding.schema_bytes);
    impl_->writer.addSchema(schema);
    schema_id = schema.id;
    impl_->schema_ids.emplace(schema_key, schema_id);
  }
  // No channel metadata: the file holds one source, so its identity is a
  // file-level fact in `pj.recording` rather than a per-channel repetition.
  mcap::Channel channel(binding.topic, binding.encoding, schema_id);
  impl_->writer.addChannel(channel);
  ++impl_->channels_added;
  return channel.id;
}

Status McapRecordingWriter::write(uint16_t channel, int64_t log_time_ns, Span<const uint8_t> bytes) {
  if (!impl_->open) {
    return unexpected("recording writer not open");
  }
  if (log_time_ns < 0) {
    return unexpected("negative log time on channel " + std::to_string(channel));
  }
  // Guarded like the id space above: mcap sequence numbers are uint32_t, and
  // wrapping would reuse numbers a reader treats as a gap or a duplicate.
  uint32_t& sequence = impl_->next_sequence[channel];
  if (sequence == std::numeric_limits<uint32_t>::max()) {
    return unexpected("message sequence exhausted on channel " + std::to_string(channel));
  }
  // Messages at or above the chunk size are isolated already, by the writer's
  // own overflow check; this brings the same treatment down to a fixed
  // threshold. Flushing a chunk is a real file write, so the sink's latch is
  // checked around it exactly as it is around the message itself.
  const bool own_chunk = bytes.size() >= kLargeMessageChunkBytes;
  if (own_chunk) {
    impl_->writer.closeLastChunk();  // whatever is buffered goes first
    if (!impl_->sink.error().empty()) {
      return unexpected("recording write failed: " + impl_->sink.error());
    }
  }
  mcap::Message message;
  message.channelId = channel;
  message.sequence = sequence++;
  message.logTime = static_cast<mcap::Timestamp>(log_time_ns);
  message.publishTime = message.logTime;  // publish time is not available at the ingest seam
  message.dataSize = bytes.size();
  message.data = reinterpret_cast<const std::byte*>(bytes.data());
  if (const auto status = impl_->writer.write(message); !status.ok()) {
    return unexpected("recording write failed: " + status.message);
  }
  if (own_chunk) {
    impl_->writer.closeLastChunk();  // and nothing that follows joins it
  }
  if (!impl_->sink.error().empty()) {
    return unexpected("recording write failed: " + impl_->sink.error());
  }
  return okStatus();
}

Status McapRecordingWriter::close(const RecordingSummary& summary) {
  if (!impl_->open) {
    return okStatus();
  }
  // Cleared first: whatever happens below, this writer is done with the file,
  // and neither a second close() nor the destructor may touch it again.
  impl_->open = false;
  mcap::Status written;
  try {
    written = impl_->writer.write(recordingMetadata(impl_->info, &summary));
    for (const auto& [name, json] : summary.extra_metadata) {
      if (!written.ok()) {
        break;
      }
      mcap::Metadata extra;
      extra.name = name;
      extra.metadata = {{"json", json}};
      written = impl_->writer.write(extra);
    }
    impl_->writer.close();
  } catch (...) {
    // The footer is lost either way; what matters is that ~McapWriter finds
    // nothing left to write into the stream and the stream is closed. The
    // caller decides what an exception out of close() means.
    impl_->writer.terminate();
    impl_->sink.end();
    throw;
  }
  if (!written.ok()) {
    return unexpected("cannot write final recording metadata: " + written.message);
  }
  if (!impl_->sink.error().empty()) {
    return unexpected("recording finalize failed: " + impl_->sink.error());
  }
  return okStatus();
}

Status forEachMcapMetadata(
    mcap::McapReader& reader, std::string_view name, const std::function<void(const mcap::Metadata&)>& on_record) {
  for (const auto& [record_name, index] : reader.metadataIndexes()) {
    if (std::string_view(record_name) != name) {
      continue;
    }
    mcap::Record record{};
    if (!mcap::McapReader::ReadRecord(*reader.dataSource(), index.offset, &record).ok()) {
      return unexpected(std::string("unreadable metadata record: ") + std::string(name));
    }
    mcap::Metadata metadata;
    if (!mcap::McapReader::ParseMetadata(record, &metadata).ok()) {
      return unexpected(std::string("unparseable metadata record: ") + std::string(name));
    }
    on_record(metadata);
  }
  return okStatus();
}

}  // namespace PJ
