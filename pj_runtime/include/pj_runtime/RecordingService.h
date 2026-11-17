// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QLatin1StringView>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_runtime/RecordTap.h"
#include "pj_runtime/Recorder.h"
#include "pj_runtime/RecordingSink.h"

namespace PJ {

/// The exclusive bucket one recorded source lands in, by precedence: a sink that could not finalize (kIncomplete) beats
/// data a failure cut short (kTruncated), which beats a complete file with holes (kLossy). A capture's per-bucket
/// counts sum to its source count. A classification, not a Status.
enum class RecordingOutcome { kClean, kLossy, kTruncated, kIncomplete };

/// The outcome as the word the log and the toast show; code compares the enum, never this.
[[nodiscard]] constexpr QLatin1StringView outcomeName(RecordingOutcome outcome) noexcept {
  switch (outcome) {
    case RecordingOutcome::kClean:
      return QLatin1StringView("clean");
    case RecordingOutcome::kLossy:
      return QLatin1StringView("lossy");
    case RecordingOutcome::kTruncated:
      return QLatin1StringView("truncated");
    case RecordingOutcome::kIncomplete:
      return QLatin1StringView("incomplete");
  }
  return QLatin1StringView("unknown");
}

/// One recordable source, described by its owner (pj_app's streaming manager) without exposing the runtime host.
/// Both callbacks run on the GUI thread.
struct RecordingTarget {
  /// Opaque, unique within one Record press; the owner assigns it (pj_app uses the DatasetId) and addresses the source
  /// by it, so a display name is never an identity.
  uint64_t target_key = 0;
  std::string source_id;  ///< display name; presentation and file naming only
  std::string plugin_id;  ///< manifest id of the plugin behind the source
  /// Installs the tap on the source, or detaches it when handed nullptr. Returns false when the source has vanished
  /// (fails the start); a throwing attach counts as false.
  std::function<bool(std::shared_ptr<RecordTap>)> attach;
  /// The source's pure-lazy push counter, which the recorder cannot see. May be unset (counts as 0).
  std::function<uint64_t()> skipped_lazy;
};

/// One source's file, as start() opened it.
struct SourceRecordingStart {
  quint64 target_key = 0;
  QString source;
  QString plugin_id;
  QString path;
};

/// One source's file, as its recording ended.
struct SourceRecordingResult {
  quint64 target_key = 0;
  QString source;
  QString plugin_id;
  QString path;
  RecordingOutcome outcome = RecordingOutcome::kClean;
  QString terminal_cause;  ///< stopped | source_ended | truncated | shutdown
  /// Why the outcome is not clean: the sink's error, the reason the source ended, or what the losses were. Empty
  /// exactly when the outcome is clean.
  QString reason;
  quint64 messages = 0;
  quint64 payload_bytes = 0;  ///< message payloads written, NOT the file size
  quint64 dropped_messages = 0;
  /// Pure-lazy pushes this recording could not see: the source's counter delta while it ran. Distinct from
  /// dropped_messages, which reached the recorder and were evicted by its byte budget.
  quint64 skipped_messages = 0;
};

/// What one Record press produced: one folder holding one file per source.
struct CaptureResult {
  QString capture_id;
  QString directory;
  QVector<SourceRecordingResult> sources;  ///< in the order start() opened them
  quint64 total_messages = 0;
  quint64 total_dropped = 0;
  quint64 total_skipped = 0;
  // Exclusive buckets: the four counts sum to sources.size().
  int clean_count = 0;
  int lossy_count = 0;
  int truncated_count = 0;
  int incomplete_count = 0;
};

/// GUI-thread owner of at most one active capture: ONE Record press opens one batch folder (pj_<UTC yyyyMMdd_HHmmss>)
/// holding ONE MCAP file per recorded source, each with its own Recorder, writer thread and queue budget. It names the
/// files, attaches the taps, polls the recorders for progress and truncation, and finalizes each file exactly once. A
/// recording never stopped cleanly carries no summary section; readers scan it linearly and `mcap recover` rebuilds
/// its index.
///
/// Finalization is SYNCHRONOUS: stop(), a source ending, a poll-detected truncation and the destructor all drain and
/// close their recorders on the calling (GUI) thread, one after the other. The wait is bounded by what is queued (at
/// most the per-source budget times the sources still recording, over the sinks' write throughput) plus one serial
/// fsync per file, which on a busy or slow disk can dominate. Drop-largest keeps the queued part small in practice.
///
/// The service is portable; only its default sink is not, so ask isSupported() before offering a Record affordance.
///
/// The service itself is portable; only the sink it defaults to is not, so ask
/// isSupported() before offering the user a Record affordance.
class RecordingService : public QObject {
  Q_OBJECT
 public:
  struct Settings {
    QString directory;  ///< empty = the default recordings folder (see defaultDirectory)
    /// Per SOURCE, not per capture: N recorded sources may queue N times this much (no service-wide cap).
    int queue_budget_mib = 64;
  };

  explicit RecordingService(QObject* parent = nullptr);
  /// Finalizes a capture still in flight, and emits nothing.
  ~RecordingService() override;

  /// Whether this build has a sink to record into: true on desktop, false in the browser. The shell gates its Record
  /// UI on this alone. start() refuses on a build without one, unless a test injected a sink through
  /// setSinkFactoryForTest.
  [[nodiscard]] static bool isSupported() noexcept;

  /// `<QStandardPaths::AppLocalDataLocation>/recordings`.
  [[nodiscard]] static QString defaultDirectory();
  /// QSettings keys: Preferences::recording_directory, Preferences::recording_queue_budget_mib. The budget is clamped
  /// to the Preferences range (8..4096 MiB), so a hand-edited .ini cannot arm a degenerate recording.
  [[nodiscard]] static Settings loadSettings();
  static void saveSettings(const Settings& settings);
  /// Takes effect on the next start(); a running capture keeps its options.
  void setSettings(Settings settings);

  /// A source display name reduced to one filesystem-safe file name: NFC normalised, path separators and
  /// Windows-reserved characters replaced by `_`, `_`/whitespace runs collapsed, trailing dots and spaces trimmed, cut
  /// to 80 characters; empty input yields "source". Distinct names can reduce to the same one, so the caller appends
  /// an ordinal. Public because it is the naming contract the tests pin.
  [[nodiscard]] static QString sanitizeSourceName(const QString& source);

  /// Opens one file per target under a new batch folder and attaches every tap; returns the folder. Transactional: any
  /// failure (a returned error, or an exception from a sink, a recorder or a target callback) detaches every target
  /// touched, closes and deletes every file created, removes the folder if empty, and emits no started(). Fails when
  /// the build has no sink (see isSupported) and no test sink was injected, when a capture is already running, when
  /// `targets` is empty, holds a duplicate target_key or a target without `attach`, when the folder cannot be created,
  /// or when a recorder cannot open its file. Never throws for any of those.
  ///
  /// Targets are sorted by target_key first, so file names, capture ordinals and every reported order are
  /// deterministic. Taps attach one after the other, so sources start with a small skew.
  [[nodiscard]] Expected<QString> start(std::vector<RecordingTarget> targets, const std::string& app_version);

  /// Ends every source still recording with the cause `stopped` and emits stopped() before returning. Idempotent and
  /// blocking (see the class comment).
  void stop();

  /// One source went away: detaches its tap and finalizes ITS file with the cause `source_ended`, leaving the rest of
  /// the capture running; the batch ends when the last source does. `reason` is kept in that source's result and makes
  /// its outcome non-clean, so pass an empty reason for an orderly end. Unknown or already-finalized keys are ignored.
  void onSourceEnded(quint64 target_key, const QString& reason);

  /// True from start() until stopped() has been emitted.
  [[nodiscard]] bool isRecording() const noexcept;

  /// Replaces what the next start() builds its recorders on, one call per recorder in target order. Tests inject a sink
  /// that fails on demand; an injected sink also makes start() work where isSupported() is false.
  void setSinkFactoryForTest(std::function<std::unique_ptr<RecordingSink>()> factory);

 signals:
  /// The capture opened: every file exists and every tap is attached.
  void started(QString capture_id, QString directory, QVector<PJ::SourceRecordingStart> sources);
  /// Emitted exactly once per Record press, once every source has its result.
  void stopped(PJ::CaptureResult result);
  /// Live totals over the whole capture: live recorders plus the frozen results of finished ones, so they never
  /// decrease. Per-source events (one source truncating while the others go on) are logged only.
  void progress(quint64 messages, quint64 payload_bytes, quint64 dropped);

 private:
  /// One recorded source for the life of one capture.
  struct SourceEntry {
    RecordingTarget target;
    std::shared_ptr<Recorder> recorder;  ///< shared: every tap co-owns it; released once finalized
    SourceRecordingStart start;
    /// The source's pure-lazy counter at start(): counters are per-host and never reset, so only the delta belongs to
    /// this capture.
    uint64_t skipped_baseline = 0;
    QString ended_reason;  ///< what onSourceEnded reported, verbatim
    /// Set once its file is closed; also what makes a second stop of the same recorder impossible.
    std::optional<SourceRecordingResult> result;
  };

  /// Publishes progress and finalizes any recorder that truncated itself (the Recorder contract leaves that to its
  /// owner).
  void onPoll();
  /// Freezes the target's skipped-lazy delta, detaches it and closes its file. No-op for a source that has a result.
  void finalizeSource(SourceEntry& entry, std::string cause);
  /// Turns one finished recorder into its result: outcome bucket, the reason behind it, counters frozen into the
  /// capture totals. `skipped` is the source's pure-lazy delta, unseen by the recorder but enough to make the outcome
  /// non-clean.
  void applyResult(SourceEntry& entry, const RecordingSummary& summary, uint64_t skipped);
  /// Emits stopped() once every source has its result, clearing the capture BEFORE the signal so a slot may start the
  /// next one.
  void publishCaptureIfComplete();
  [[nodiscard]] SourceEntry* entryFor(quint64 target_key);
  [[nodiscard]] CaptureResult buildCaptureResult() const;
  void resetCapture();
  /// Undoes a partial start(): detaches everything attached, closes every recorder opened, deletes every file created
  /// and the folder if it empties. What it cannot remove is logged, not returned: the caller's error is about the
  /// source that failed.
  void rollbackStart(std::vector<SourceEntry>& entries, const QString& directory);

  Settings settings_;
  std::function<std::unique_ptr<RecordingSink>()> sink_factory_;  ///< unset = the build's default sink
  QString capture_id_;
  QString capture_directory_;
  /// The capture's sources in started order (sorted by target_key). Empty exactly when no capture is running.
  std::vector<SourceEntry> sources_;
  /// Totals of the sources already finalized, so the aggregates a poll publishes never decrease as recorders retire.
  quint64 frozen_messages_ = 0;
  quint64 frozen_payload_bytes_ = 0;
  quint64 frozen_dropped_ = 0;
  QTimer poll_timer_;
};

}  // namespace PJ

Q_DECLARE_METATYPE(PJ::SourceRecordingStart)
Q_DECLARE_METATYPE(PJ::SourceRecordingResult)
Q_DECLARE_METATYPE(PJ::CaptureResult)
