#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <QHash>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

#include "pj_runtime/SessionManager.h"
#include "pj_widgets/CurveTreeView.h"

class QTimer;

namespace PJ {

/// Bridges SessionManager's progressive ingest signals to CurveTreeView's
/// plain-typed progress state. Maintains the lifecycle of each ingest from
/// beginIngest through terminal states, applying linger delays and flash
/// animations according to the outcome.
///
/// Key responsibilities:
/// - Keys each row by its IngestId: the token id IS the opaque row key the view
///   reports clicks against, so a click resolves back to its ingest by lookup
///   and the two identities can never drift apart
/// - Subscribes to ingestBegan/ingestProgressed/ingestEnded and maintains a
///   QHash<IngestId, DatasetProgress>
/// - Implements state transitions: kLoading → terminal → linger → removed
/// - Terminal animations: 1s linger for completed/cancelled, 3x flash at ~180ms
///   for failed, immediate clear for kUnknown
/// - Ghost rows: when no catalog node exists for an ingest, renders a top-level
///   progress-only row from the entry's display name
/// - Stale tokens silently dropped (a restarted ingest supersedes the old id)
/// - Timer factory seam for headless tests to advance linger/flash
///
/// TERMINAL STATE SEQUENCE (for reference):
/// - kCompleted/kCancelled: enter state → startLingerTimer (single-shot 1s) →
///   timeout fires once → removeEntry → row removed
/// - kFailed: enter state → startFlashTimer (repeating ~180ms) → 6 toggles of
///   flash_on → startLingerTimer (single-shot 1s) → timeout fires once →
///   removeEntry → row removed
/// - kUnknown: removeEntry immediately, no animation
///
/// The flash/linger share a single QTimer; the single-shot mode is explicitly
/// set before each start() to prevent a repeating flash timer from accidentally
/// remaining repeating during the linger phase.
class IngestProgressController : public QObject {
  Q_OBJECT

 public:
  /// Constructs the controller. Optional `timer_factory` is a callable that
  /// returns a new QTimer on demand; when nullptr, the controller creates
  /// timers normally. The factory seam exists so headless tests can inject a
  /// mock timer and advance linger/flash deterministically.
  explicit IngestProgressController(QObject* parent = nullptr, std::function<QTimer*()> timer_factory = nullptr);
  ~IngestProgressController() override;

  IngestProgressController(const IngestProgressController&) = delete;
  IngestProgressController& operator=(const IngestProgressController&) = delete;

  /// Attaches the controller to a SessionManager so it can subscribe to the
  /// ingest signals. Called only once at setup.
  void setSessionManager(SessionManager* session);

  /// Resolves a dataset id to its tree path (its catalog display label for the
  /// tree). Returns an empty string when the dataset has no row yet — this is
  /// legitimate and simply leaves the entry on a ghost row until it resolves.
  /// Called on ingest begin and re-tried on progress ticks for unresolved
  /// entries.
  using DatasetPathResolver = std::function<QString(DatasetId)>;

  /// Injects the dataset-path resolver. It returns the dataset's tree path
  /// (its catalog display label) or an empty string when the dataset has no
  /// row yet. The resolver is retained by reference; capture only data valid
  /// for the controller's lifetime (typically the CatalogModel owned by the
  /// session).
  void setDatasetPathResolver(DatasetPathResolver resolver);

  /// Re-resolves every entry still on a ghost row. A dataset's catalog row can
  /// appear at any point during (or after) its load, and until it does the
  /// entry has nowhere real to live; call this whenever the catalog changes so
  /// a ghost converts to the dataset's own row as soon as one exists, instead
  /// of a phantom row sitting above it for the rest of the load.
  void refreshDatasetPaths();

  /// The ingest a tree row belongs to, or nullopt once that ingest has
  /// ended (its entry is dropped after the terminal linger) or the key is
  /// unknown.
  ///
  /// This is the ONLY correct way to answer "which load did the user click
  /// the cancel button of": the controller alone holds the row-key<->token
  /// mapping, and several ingests run in parallel, so scanning for "some
  /// cancellable ingest" would cancel an arbitrary one of them.
  [[nodiscard]] std::optional<IngestToken> tokenForRowKey(quint64 row_key) const;

 signals:
  /// Emitted whenever the progress hash changes (a state transition, entry
  /// added, or entry removed). Carries the full, current progress set keyed by
  /// row key.
  void progressUpdated(QHash<quint64, PJ::CurveTreeView::DatasetProgress> by_row_key);

  /// Emitted when a dataset row is created for an ingest, mapping the catalog
  /// tree path to the opaque row key used in progressUpdated. Sent before that
  /// ingest's first progressUpdated, so the row is already named after its
  /// dataset when its first progress state arrives.
  void rowKeyChanged(QString tree_path, quint64 row_key);

 private slots:
  void onIngestBegan(PJ::IngestToken token, QString label, quint64 total, bool cancellable, bool discardable);
  void onIngestProgressed(PJ::IngestToken token, quint64 current, quint64 total);
  void onIngestStopping(PJ::IngestToken token, bool keep_partial);
  void onIngestEnded(PJ::IngestToken token, PJ::IngestOutcome outcome);
  void onLingerOrFlashTimerTimeout();

 private:
  struct IngestEntry {
    IngestToken token;           // token.id doubles as the view-facing row key
    QString resolved_tree_path;  // Last emitted tree path; empty if unresolved
    CurveTreeView::DatasetProgress state;
    QTimer* linger_or_flash_timer = nullptr;
    int flash_phase = 0;  // 0-5: 3 on/off cycles; 6+ = done flashing
  };

  /// Records or updates an ingest entry and emits progressUpdated. Should be
  /// called whenever state changes.
  void updateAndEmit();

  /// Attempts to resolve the dataset path for an entry. If the path changes
  /// from what was last emitted, emits rowKeyChanged with the new path.
  void tryResolveDatasetPath(IngestEntry& entry);

  /// Starts the linger timer for a terminal state. On timeout, the entry is
  /// removed and progressUpdated is emitted. Always sets the timer to single-shot
  /// mode before starting, even when reusing a timer from the flash phase.
  void startLingerTimer(IngestEntry& entry, int linger_ms);

  /// Starts the flash timer for a failed state. Cycles flash_on 3 times,
  /// then transitions to linger. The timer is repeating; when 6 transitions
  /// are complete, startLingerTimer is called to switch to single-shot mode
  /// and begin the linger phase.
  void startFlashTimer(IngestEntry& entry);

  /// Cancels any pending timers and removes the entry.
  void removeEntry(IngestId id);

  SessionManager* session_ = nullptr;
  QHash<IngestId, IngestEntry> ingests_;
  std::function<QTimer*()> timer_factory_;
  DatasetPathResolver dataset_path_resolver_;
};

}  // namespace PJ
