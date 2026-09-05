// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/RecordingService.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>
#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#ifndef PJ_TARGET_WASM
#include "pj_runtime/McapRecordingWriter.h"
#endif
#include "pj_runtime/RecordingFormat.h"

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

Q_LOGGING_CATEGORY(lcRecording, "pj.runtime.recording")

constexpr auto kDirectoryKey = "Preferences::recording_directory";
constexpr auto kQueueBudgetKey = "Preferences::recording_queue_budget_mib";
constexpr int kPollIntervalMs = 500;
/// How many `_N` variants of one second's batch folder are tried before giving up.
constexpr int kMaxDirectoryAttempts = 100;
/// Accepted range of the queue budget, matching the Preferences scrubber: a
/// hand-edited .ini must not be able to arm a 1 TiB queue.
constexpr int kMinQueueBudgetMiB = 8;
constexpr int kMaxQueueBudgetMiB = 4096;
/// Where a source name is cut. Long enough for any readable name, short enough
/// to leave room under the path length limits Windows still enforces on some
/// volumes.
constexpr int kMaxSourceNameChars = 80;

/// The batch folder of one Record press, created ATOMICALLY: mkdir on an
/// existing name fails, so creating the folder IS claiming it — there is no
/// window between a check and the create for a second press to slip into.
Expected<QString> createCaptureDirectory(const QString& base) {
  const QString stamp = QDateTime::currentDateTimeUtc().toString(u"yyyyMMdd_HHmmss"_s);
  QDir dir(base);
  for (int attempt = 1; attempt <= kMaxDirectoryAttempts; ++attempt) {
    const QString name = attempt == 1 ? u"pj_"_s + stamp : u"pj_"_s + stamp + u"_"_s + QString::number(attempt);
    if (dir.mkdir(name)) {
      return dir.absoluteFilePath(name);
    }
  }
  return unexpected("no free recording folder for " + stamp.toStdString() + " in " + base.toStdString());
}

/// The sink a production start() records into. Null on a build with none
/// compiled in, where start() refuses long before reaching here.
std::unique_ptr<RecordingSink> makeDefaultSink() {
#ifdef PJ_TARGET_WASM
  return nullptr;
#else
  return std::make_unique<McapRecordingWriter>();
#endif
}

std::filesystem::path toNativePath(const QString& path) {
#ifdef _WIN32
  return std::filesystem::path(path.toStdWString());
#else
  return std::filesystem::path(path.toStdString());
#endif
}

/// Installs (or, with a null tap, removes) one source's tap. A plugin-side
/// throw is a source that cannot take the tap, not a reason to unwind through
/// Qt, so it reads as a plain false.
bool attachTap(const RecordingTarget& target, std::shared_ptr<RecordTap> tap) {
  try {
    return target.attach(std::move(tap));
  } catch (...) {
    return false;
  }
}

/// The source's pure-lazy counter, or 0 for a target that has none. A throwing
/// counter is an owner bug; it must not unwind through start() or a finalize,
/// so it reads as 0 and costs only this diagnostic count.
uint64_t readSkippedLazy(const RecordingTarget& target) noexcept {
  if (!target.skipped_lazy) {
    return 0;
  }
  try {
    return target.skipped_lazy();
  } catch (...) {
    return 0;
  }
}

/// `sanitized`, or the first `_2`, `_3`… variant no sibling has taken. Compared
/// case-folded, so names differing only in case — or only in characters
/// sanitization erased — still get their own file on a case-insensitive volume.
QString uniqueSourceFileName(const QString& sanitized, QSet<QString>& taken) {
  QString candidate = sanitized;
  int ordinal = 2;
  while (taken.contains(candidate.toCaseFolded())) {
    candidate = sanitized + u"_"_s + QString::number(ordinal++);
  }
  taken.insert(candidate.toCaseFolded());
  return candidate;
}

}  // namespace

RecordingService::RecordingService(QObject* parent) : QObject(parent) {
  // Registered here rather than at static-init time: this service is the only
  // emitter of these types, and QSignalSpy needs them before the first emit.
  qRegisterMetaType<SourceRecordingStart>();
  qRegisterMetaType<QVector<SourceRecordingStart>>();
  qRegisterMetaType<SourceRecordingResult>();
  qRegisterMetaType<QVector<SourceRecordingResult>>();
  qRegisterMetaType<CaptureResult>();
  poll_timer_.setInterval(kPollIntervalMs);
  connect(&poll_timer_, &QTimer::timeout, this, &RecordingService::onPoll);
}

RecordingService::~RecordingService() {
  poll_timer_.stop();
  // Every file is closed here rather than left to the recorders' own
  // destructors, so each one still gets its detach and its terminal cause. No
  // signal follows: a destructor publishes nothing.
  for (SourceEntry& entry : sources_) {
    finalizeSource(entry, std::string(kTerminalCauseShutdown));
  }
}

bool RecordingService::isSupported() noexcept {
#ifdef PJ_TARGET_WASM
  return false;
#else
  return true;
#endif
}

QString RecordingService::defaultDirectory() {
  const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  return QDir(base).absoluteFilePath(u"recordings"_s);
}

RecordingService::Settings RecordingService::loadSettings() {
  QSettings settings;
  Settings loaded;
  loaded.directory = settings.value(QString::fromLatin1(kDirectoryKey)).toString();
  loaded.queue_budget_mib = std::clamp(
      settings.value(QString::fromLatin1(kQueueBudgetKey), loaded.queue_budget_mib).toInt(), kMinQueueBudgetMiB,
      kMaxQueueBudgetMiB);
  return loaded;
}

void RecordingService::saveSettings(const Settings& settings) {
  QSettings store;
  store.setValue(QString::fromLatin1(kDirectoryKey), settings.directory);
  store.setValue(QString::fromLatin1(kQueueBudgetKey), settings.queue_budget_mib);
}

void RecordingService::setSettings(Settings settings) {
  settings_ = std::move(settings);
}

void RecordingService::setSinkFactoryForTest(std::function<std::unique_ptr<RecordingSink>()> factory) {
  sink_factory_ = std::move(factory);
}

QString RecordingService::sanitizeSourceName(const QString& source) {
  // Control characters, the characters no mainstream filesystem accepts (both
  // separators included), whitespace and `_` are one class, so any run of them
  // becomes a single `_`. \p{Z} (every Unicode separator: spaces plus the line
  // and paragraph ones) and U+0085 are spelled out because \s covers only ASCII
  // whitespace without UseUnicodePropertiesOption.
  static const QRegularExpression separators(uR"([\x{00}-\x{1F}\x{7F}/\\:*?"<>|_\s\p{Z}\x{85}]+)"_s);
  // Normalised first: two spellings of the same accented name would otherwise
  // produce two different files, and only one of them would round-trip.
  QString collapsed = source.normalized(QString::NormalizationForm_C).replace(separators, u"_"_s);
  // Windows silently strips these, which would turn a unique name into a
  // collision with the one it strips down to.
  while (!collapsed.isEmpty() && (collapsed.endsWith(u'.') || collapsed.endsWith(u' '))) {
    collapsed.chop(1);
  }
  if (collapsed.isEmpty()) {
    return u"source"_s;
  }
  if (collapsed.size() <= kMaxSourceNameChars) {
    return collapsed;
  }
  qsizetype keep = kMaxSourceNameChars;
  if (collapsed.at(keep - 1).isHighSurrogate()) {
    --keep;  // never cut a surrogate pair in half
  }
  return collapsed.left(keep);
}

Expected<QString> RecordingService::start(std::vector<RecordingTarget> targets, const std::string& app_version) {
  if (!sources_.empty()) {
    return unexpected("a recording is already running");
  }
  // Refuse before touching the filesystem on a build with no sink. A test that
  // injected its own sink is exempt: it brings the sink the build lacks.
  if (!sink_factory_ && !isSupported()) {
    return unexpected("recording is not available in this build");
  }
  if (targets.empty()) {
    return unexpected("there is nothing to record");
  }
  // Sorted first, so file names, capture ordinals and every reported order come
  // out the same for the same set of sources.
  std::sort(targets.begin(), targets.end(), [](const RecordingTarget& lhs, const RecordingTarget& rhs) {
    return lhs.target_key < rhs.target_key;
  });
  for (std::size_t index = 0; index < targets.size(); ++index) {
    if (!targets[index].attach) {
      return unexpected("recording target '" + targets[index].source_id + "' has no attach");
    }
    if (index > 0 && targets[index].target_key == targets[index - 1].target_key) {
      return unexpected(
          "two recording targets share the key " + std::to_string(targets[index].target_key) + " ('" +
          targets[index].source_id + "')");
    }
  }

  const QString base = settings_.directory.isEmpty() ? defaultDirectory() : settings_.directory;
  if (!QDir().mkpath(base)) {
    return unexpected("cannot create the recordings folder: " + base.toStdString());
  }
  const auto directory = createCaptureDirectory(base);
  if (!directory) {
    return unexpected(directory.error());
  }
  const QString capture_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const uint64_t budget_bytes = static_cast<uint64_t>(std::max(settings_.queue_budget_mib, 1)) << 20;

  // Everything is opened before anything is attached, and the whole batch rolls
  // back as one: a half-armed capture would record some sources and silently
  // miss others.
  std::vector<SourceEntry> entries;
  entries.reserve(targets.size());
  QSet<QString> taken_names;
  std::string failure;
  for (std::size_t index = 0; index < targets.size() && failure.empty(); ++index) {
    SourceEntry entry;
    entry.target = std::move(targets[index]);
    entry.start.target_key = entry.target.target_key;
    entry.start.source = QString::fromStdString(entry.target.source_id);
    entry.start.plugin_id = QString::fromStdString(entry.target.plugin_id);
    const QString file_name = uniqueSourceFileName(sanitizeSourceName(entry.start.source), taken_names);
    entry.start.path = QDir(*directory).absoluteFilePath(file_name + u".mcap"_s);

    RecorderOptions options;
    options.path = toNativePath(entry.start.path);
    options.queue_budget_bytes = budget_bytes;
    options.info.app_version = app_version;
    options.info.capture_id = capture_id.toStdString();
    options.info.source_display_name = entry.target.source_id;
    options.info.source_plugin_id = entry.target.plugin_id;
    options.info.capture_ordinal = static_cast<uint32_t>(index + 1);

    // A sink or a recorder that throws instead of returning an error is still
    // one failed source, and the rollback below must run for it: an exception
    // escaping here would leave every file opened so far, and the folder, on
    // disk. The entry is kept either way — a recorder whose start threw may
    // already have created its file.
    try {
      std::unique_ptr<RecordingSink> sink = sink_factory_ ? sink_factory_() : makeDefaultSink();
      if (sink == nullptr) {
        failure = "the recording sink factory produced no sink for '" + entry.target.source_id + "'";
      } else {
        entry.recorder = std::make_shared<Recorder>(std::move(sink), std::move(options));
        if (auto status = entry.recorder->start(); !status) {
          failure = status.error();
        }
      }
    } catch (const std::exception& error) {
      failure = "recording of '" + entry.target.source_id + "' could not start: " + error.what();
    } catch (...) {
      failure = "recording of '" + entry.target.source_id + "' could not start";
    }
    entries.push_back(std::move(entry));
  }

  if (failure.empty()) {
    for (SourceEntry& entry : entries) {
      // A source's pure-lazy counter is per-host and never resets, so only what
      // it gains between here and the finish belongs to this capture.
      entry.skipped_baseline = readSkippedLazy(entry.target);
      if (!attachTap(entry.target, entry.recorder->tapFor())) {
        failure = "source '" + entry.target.source_id + "' could not be recorded: it is no longer live";
        break;
      }
    }
  }
  if (!failure.empty()) {
    rollbackStart(entries, *directory);
    return unexpected(failure);
  }

  capture_id_ = capture_id;
  capture_directory_ = *directory;
  sources_ = std::move(entries);
  frozen_messages_ = 0;
  frozen_payload_bytes_ = 0;
  frozen_dropped_ = 0;
  poll_timer_.start();

  QVector<SourceRecordingStart> published;
  published.reserve(static_cast<qsizetype>(sources_.size()));
  for (const SourceEntry& entry : sources_) {
    published.push_back(entry.start);
  }
  const QString capture_directory = capture_directory_;
  emit started(capture_id_, capture_directory, published);
  return capture_directory;
}

void RecordingService::rollbackStart(std::vector<SourceEntry>& entries, const QString& directory) {
  // Every target, not only the ones whose attach was reached: the failing one
  // may have installed its tap before reporting false, and detaching a source
  // that never got one is a no-op.
  for (SourceEntry& entry : entries) {
    (void)attachTap(entry.target, nullptr);
  }
  for (SourceEntry& entry : entries) {
    if (entry.recorder != nullptr) {
      (void)entry.recorder->stop(std::string(kTerminalCauseShutdown));  // closes the file so it can be removed
      entry.recorder.reset();
    }
    if (entry.start.path.isEmpty() || !QFileInfo::exists(entry.start.path)) {
      continue;
    }
    if (!QFile::remove(entry.start.path)) {
      qCWarning(lcRecording) << "could not remove the abandoned recording" << entry.start.path;
    }
  }
  // Never recursive: rmdir refuses a folder still holding anything, which is
  // exactly the file the warning above named.
  if (!QDir().rmdir(directory)) {
    qCWarning(lcRecording) << "could not remove the abandoned recording folder" << directory;
  }
}

void RecordingService::stop() {
  if (sources_.empty()) {
    return;  // never started, or already finished: a second stop changes nothing
  }
  for (SourceEntry& entry : sources_) {
    finalizeSource(entry, std::string(kTerminalCauseStopped));
  }
  publishCaptureIfComplete();
}

void RecordingService::onSourceEnded(quint64 target_key, const QString& reason) {
  SourceEntry* entry = entryFor(target_key);
  if (entry == nullptr || entry->result.has_value()) {
    return;
  }
  entry->ended_reason = reason;
  finalizeSource(*entry, std::string(kTerminalCauseSourceEnded));
  // The batch ends with its last live source.
  publishCaptureIfComplete();
}

bool RecordingService::isRecording() const noexcept {
  return !sources_.empty();
}

RecordingService::SourceEntry* RecordingService::entryFor(quint64 target_key) {
  const auto found = std::find_if(sources_.begin(), sources_.end(), [target_key](const SourceEntry& entry) {
    return entry.start.target_key == target_key;
  });
  return found == sources_.end() ? nullptr : &*found;
}

void RecordingService::finalizeSource(SourceEntry& entry, std::string cause) {
  if (entry.result.has_value() || entry.recorder == nullptr) {
    return;  // its file is already closed: never stop a recorder twice
  }
  // Read BEFORE the detach: a pure-lazy push racing the detach then lands on
  // the next capture's baseline, where it is invisible, rather than on this
  // count. It is ±1 at either boundary regardless — the host decides whether a
  // push counts from a relaxed tap-installed hint, and a lock on its push path
  // would cost every message to make a diagnostic counter exact.
  const uint64_t current = readSkippedLazy(entry.target);
  (void)attachTap(entry.target, nullptr);  // a null tap is what detaches
  // A source whose session ended reports a counter BELOW its baseline (its
  // owner has nothing left to read): that contributes zero, not a wrap.
  const uint64_t skipped = current > entry.skipped_baseline ? current - entry.skipped_baseline : 0;
  const RecordingSummary summary = entry.recorder->stop(std::move(cause));
  applyResult(entry, summary, skipped);
}

void RecordingService::applyResult(SourceEntry& entry, const RecordingSummary& summary, uint64_t skipped) {
  SourceRecordingResult result;
  result.target_key = entry.start.target_key;
  result.source = entry.start.source;
  result.plugin_id = entry.start.plugin_id;
  result.path = entry.start.path;
  result.terminal_cause = QString::fromStdString(summary.terminal_cause);
  result.messages = summary.messages;
  result.payload_bytes = summary.payload_bytes;
  result.dropped_messages = summary.dropped_messages;
  result.skipped_messages = skipped;

  const QString truncated_reason = QString::fromStdString(summary.truncated_reason);
  if (summary.file_incomplete) {
    result.outcome = RecordingOutcome::kIncomplete;
    result.reason = truncated_reason.isEmpty() ? u"the file could not be finalized"_s : truncated_reason;
  } else if (summary.truncated) {
    result.outcome = RecordingOutcome::kTruncated;
    result.reason = truncated_reason.isEmpty() ? u"the recording ended early"_s : truncated_reason;
  } else if (!entry.ended_reason.isEmpty() || result.dropped_messages > 0 || result.skipped_messages > 0) {
    result.outcome = RecordingOutcome::kLossy;
    QStringList notes;
    if (!entry.ended_reason.isEmpty()) {
      notes << u"source ended: "_s + entry.ended_reason;
    }
    if (result.dropped_messages > 0) {
      notes << u"the write queue dropped %1 message(s)"_s.arg(result.dropped_messages);
    }
    if (result.skipped_messages > 0) {
      notes << u"%1 message(s) never reached the recorder"_s.arg(result.skipped_messages);
    }
    result.reason = notes.join(u"; "_s);
  } else {
    result.outcome = RecordingOutcome::kClean;
  }
  if (result.outcome != RecordingOutcome::kClean) {
    // The only place a per-source problem is reported while the rest of the
    // capture keeps going: the signals carry capture totals, not per-source
    // events.
    qCWarning(lcRecording) << "recording of" << result.source << "ended as" << outcomeName(result.outcome) << ":"
                           << result.reason;
  }

  // Frozen into the aggregates before the recorder goes, so the totals a poll
  // publishes never step backwards as sources retire.
  frozen_messages_ += result.messages;
  frozen_payload_bytes_ += result.payload_bytes;
  frozen_dropped_ += result.dropped_messages;
  entry.recorder.reset();
  entry.result = std::move(result);
}

void RecordingService::publishCaptureIfComplete() {
  if (sources_.empty()) {
    return;
  }
  const bool complete =
      std::all_of(sources_.begin(), sources_.end(), [](const SourceEntry& entry) { return entry.result.has_value(); });
  if (!complete) {
    return;
  }
  const CaptureResult capture = buildCaptureResult();
  // Cleared before the signal: a slot may start the next capture straight away.
  resetCapture();
  emit stopped(capture);
}

CaptureResult RecordingService::buildCaptureResult() const {
  CaptureResult capture;
  capture.capture_id = capture_id_;
  capture.directory = capture_directory_;
  capture.sources.reserve(static_cast<qsizetype>(sources_.size()));
  for (const SourceEntry& entry : sources_) {
    if (!entry.result.has_value()) {
      continue;
    }
    const SourceRecordingResult& result = *entry.result;
    capture.total_messages += result.messages;
    capture.total_dropped += result.dropped_messages;
    capture.total_skipped += result.skipped_messages;
    switch (result.outcome) {
      case RecordingOutcome::kIncomplete:
        ++capture.incomplete_count;
        break;
      case RecordingOutcome::kTruncated:
        ++capture.truncated_count;
        break;
      case RecordingOutcome::kLossy:
        ++capture.lossy_count;
        break;
      case RecordingOutcome::kClean:
        ++capture.clean_count;
        break;
    }
    capture.sources.push_back(result);
  }
  return capture;
}

void RecordingService::resetCapture() {
  poll_timer_.stop();
  sources_.clear();
  capture_id_.clear();
  capture_directory_.clear();
  frozen_messages_ = 0;
  frozen_payload_bytes_ = 0;
  frozen_dropped_ = 0;
}

void RecordingService::onPoll() {
  if (sources_.empty()) {
    return;
  }
  quint64 messages = frozen_messages_;
  quint64 payload_bytes = frozen_payload_bytes_;
  quint64 dropped = frozen_dropped_;
  std::vector<quint64> truncated_keys;
  for (const SourceEntry& entry : sources_) {
    if (entry.recorder == nullptr) {
      continue;  // already finalized: its frozen result is in the totals above
    }
    const RecordingStats live = entry.recorder->stats();
    messages += live.messages;
    payload_bytes += live.payload_bytes;
    dropped += live.dropped_messages;
    if (entry.recorder->state() == Recorder::State::kTruncated) {
      truncated_keys.push_back(entry.start.target_key);
    }
  }
  // Slots run synchronously: one may stop this capture, and its stopped() slot
  // may start the NEXT one over the same target keys. The truncated set is
  // therefore collected before the emit and re-resolved after it, against the
  // capture id that was polled — a poll must never finalize a capture it did
  // not read.
  const QString polled_capture = capture_id_;
  emit progress(messages, payload_bytes, dropped);
  if (capture_id_ != polled_capture) {
    return;
  }
  for (const quint64 target_key : truncated_keys) {
    if (SourceEntry* entry = entryFor(target_key); entry != nullptr) {
      // The Recorder contract leaves a truncation's cleanup to its owner.
      finalizeSource(*entry, std::string(kTerminalCauseTruncated));
    }
  }
  if (!truncated_keys.empty()) {
    publishCaptureIfComplete();
  }
}

}  // namespace PJ
