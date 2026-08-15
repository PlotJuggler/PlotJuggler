// SPDX-License-Identifier: MPL-2.0

#include "IngestProgressController.h"

#include <QLoggingCategory>

namespace {
// Ingest-UI diagnostics: every controller state transition (began / progress /
// ended) with row keys and fractions. Off by default (QtWarningMsg floor);
// enable with QT_LOGGING_RULES="pj.app.ingestui*=true".
Q_LOGGING_CATEGORY(lcIngestUi, "pj.app.ingestui", QtWarningMsg)
}  // namespace

#include <QTimer>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace PJ {

namespace {
// Timing constants
constexpr int kLingerMs = 1000;     // 1 second
constexpr int kFlashCycleMs = 180;  // ~180ms per on/off cycle
constexpr int kFlashCycles = 3;     // 3 on/off pairs = 6 transitions total
}  // namespace

IngestProgressController::IngestProgressController(QObject* parent, std::function<QTimer*()> timer_factory)
    : QObject(parent), timer_factory_(std::move(timer_factory)) {}

IngestProgressController::~IngestProgressController() {
  // Clean up all timers
  for (auto& entry : ingests_) {
    if (entry.linger_or_flash_timer) {
      entry.linger_or_flash_timer->stop();
    }
  }
}

std::optional<IngestToken> IngestProgressController::tokenForRowKey(quint64 row_key) const {
  for (auto it = ingests_.cbegin(); it != ingests_.cend(); ++it) {
    if (it->row_key == row_key) {
      return it->token;
    }
  }
  return std::nullopt;
}

void IngestProgressController::setSessionManager(SessionManager* session) {
  if (session_) {
    disconnect(session_, nullptr, this, nullptr);
  }

  session_ = session;

  if (session_) {
    connect(session_, &SessionManager::ingestBegan, this, &IngestProgressController::onIngestBegan);
    connect(session_, &SessionManager::ingestProgressed, this, &IngestProgressController::onIngestProgressed);
    connect(session_, &SessionManager::ingestEnded, this, &IngestProgressController::onIngestEnded);
    connect(session_, &SessionManager::ingestStopping, this, &IngestProgressController::onIngestStopping);
  }
}

void IngestProgressController::setDatasetPathResolver(DatasetPathResolver resolver) {
  dataset_path_resolver_ = std::move(resolver);
}

void IngestProgressController::onIngestBegan(
    PJ::IngestToken token, QString label, quint64 total, bool cancellable, bool discardable) {
  if (!token.id) {
    return;  // Stale token
  }

  // Retire any row this dataset still shows. A LIVE ingest is already gone by
  // now — the runtime reports a terminal when a restart supersedes one, and a
  // cancelled row retires without lingering — so what this catches is a row
  // still lingering after its OWN terminal (a finished load restarted inside
  // the linger), which must not sit beside the new one.
  std::vector<IngestId> superseded;
  for (auto it = ingests_.cbegin(); it != ingests_.cend(); ++it) {
    if (it->token.dataset_id == token.dataset_id) {
      superseded.push_back(it.key());
    }
  }
  for (const IngestId id : superseded) {
    removeEntry(id);
  }

  // Create entry for this ingest
  IngestEntry entry;
  entry.token = token;
  entry.row_key = mintRowKey();
  entry.resolved_tree_path.clear();  // Initially unresolved
  entry.state.display_name = label;
  entry.state.indeterminate = (total == 0);
  entry.state.fraction = 0.0;
  entry.state.state = CurveTreeView::DatasetProgress::State::kLoading;
  entry.state.flash_on = true;
  entry.state.cancellable = cancellable;
  entry.state.discardable = discardable;

  qCInfo(lcIngestUi) << "[ctl] BEGAN token=" << token.id << "row_key=" << entry.row_key << "total=" << total
                     << "indeterminate=" << entry.state.indeterminate;
  ingests_[token.id] = std::move(entry);

  // Resolve BEFORE the first publication: the row must appear under its
  // dataset's own name, never flash the producer's progress title first.
  // A still-unresolved dataset emits an empty path (ghost row).
  tryResolveDatasetPath(ingests_[token.id]);

  updateAndEmit();
}

void IngestProgressController::onIngestProgressed(PJ::IngestToken token, quint64 current, quint64 total) {
  auto it = ingests_.find(token.id);
  if (it == ingests_.end()) {
    return;  // Stale or unknown token
  }

  IngestEntry& entry = *it;
  using State = CurveTreeView::DatasetProgress::State;
  // Resolution first, and on EVERY tick: the catalog can relabel a dataset
  // mid-ingest (a sibling's removal turns "foo (2)" back into "foo"), which
  // moves the tree row out from under this entry. Re-resolving only while the
  // path was still empty left the row filed under a path that no longer exists
  // — a ghost for the rest of the load. Runs above the state gates below so a
  // stopping or finished row keeps tracking its dataset while it lingers.
  tryResolveDatasetPath(entry);

  // A stopping row is FROZEN, and freezing it is this layer's job: the runtime
  // keeps reporting what the producer really delivered (it goes on delivering
  // until it notices the stop), so the tick is absorbed here. Advancing the bar
  // after the click, or worse dropping the row back to kLoading, would
  // un-acknowledge a stop the user already sees acknowledged.
  //
  // A row past its terminal is equally untouchable: the entry outlives it for
  // the completion linger / error flash, and a tick landing in that window
  // would replace the finished 100% bar with whatever fraction it carried.
  if (entry.state.state != State::kLoading) {
    return;
  }
  entry.state.indeterminate = (total == 0);
  entry.state.fraction = (total > 0) ? std::min(1.0, static_cast<double>(current) / static_cast<double>(total)) : 0.0;
  qCInfo(lcIngestUi) << "[ctl] PROGRESS token=" << token.id << "row_key=" << entry.row_key << "cur=" << current
                     << "total=" << total << "fraction=" << entry.state.fraction;

  updateAndEmit();
}

void IngestProgressController::onIngestStopping(PJ::IngestToken token, bool /*keep_partial*/) {
  auto it = ingests_.find(token.id);
  if (it == ingests_.end()) {
    return;  // stale or already terminated
  }
  // Presentation only: the row acknowledges the click now and freezes. The
  // producer's real terminal still arrives via onIngestEnded and decides the
  // final state.
  it->state.state = CurveTreeView::DatasetProgress::State::kStopping;
  updateAndEmit();
}

void IngestProgressController::refreshDatasetPaths() {
  bool changed = false;
  for (auto it = ingests_.begin(); it != ingests_.end(); ++it) {
    if (it->resolved_tree_path.isEmpty()) {
      tryResolveDatasetPath(*it);
      changed = changed || !it->resolved_tree_path.isEmpty();
    }
  }
  if (changed) {
    updateAndEmit();  // the row moved from a ghost to its dataset's own row
  }
}

void IngestProgressController::onIngestEnded(PJ::IngestToken token, PJ::IngestOutcome outcome) {
  auto it = ingests_.find(token.id);
  if (it == ingests_.end()) {
    return;  // Stale or unknown token
  }

  IngestEntry& entry = *it;
  // A load that committed has a catalog row by now even if no progress tick
  // arrived after it appeared. Resolve before the terminal state is applied so
  // the completion lingers on the dataset's own row rather than on a ghost
  // sitting above it.
  if (entry.resolved_tree_path.isEmpty()) {
    tryResolveDatasetPath(entry);
  }
  qCInfo(lcIngestUi) << "[ctl] ENDED token=" << token.id << "row_key=" << entry.row_key
                     << "outcome=" << static_cast<int>(outcome) << "fraction_at_end=" << entry.state.fraction;

  // Map outcome to state
  switch (outcome) {
    case IngestOutcome::kCompleted:
      entry.state.state = CurveTreeView::DatasetProgress::State::kCompleted;
      entry.state.fraction = 1.0;
      entry.state.indeterminate = false;
      entry.flash_phase = 0;
      startLingerTimer(entry, kLingerMs);
      break;

    case IngestOutcome::kCancelled:
      // The user stopped this load, so they already know the outcome — and a
      // discard has deleted the dataset, leaving a lingering row describing
      // something that no longer exists. Retiring at once IS the confirmation.
      removeEntry(token.id);
      updateAndEmit();
      return;

    case IngestOutcome::kFailed:
      entry.state.state = CurveTreeView::DatasetProgress::State::kFailed;
      // The fraction is left at where the load actually died: the view spans a
      // failed bar full-width from the state alone, and the caption reports
      // this number, so overwriting it would make the row claim 100%.
      entry.state.indeterminate = false;
      entry.flash_phase = 0;
      startFlashTimer(entry);
      break;

    case IngestOutcome::kUnknown:
      // Unknown outcome clears immediately, no animation
      removeEntry(token.id);
      updateAndEmit();
      return;
  }

  updateAndEmit();
}

void IngestProgressController::onLingerOrFlashTimerTimeout() {
  // Find which entry's timer fired
  for (auto it = ingests_.begin(); it != ingests_.end(); ++it) {
    IngestEntry& entry = *it;
    if (!entry.linger_or_flash_timer) {
      continue;
    }

    // QTimer::sender() returns the sender if called from a slot connected to
    // the timer's timeout signal
    QObject* sender = entry.linger_or_flash_timer;
    if (QObject::sender() != sender) {
      continue;
    }

    // A failed entry flashes FIRST and lingers SECOND, so "still flashing" is a
    // function of the phase counter, not of the state: gating on the state alone
    // lets the linger's own firing re-enter the flash branch, and the entry then
    // never retires. Every other firing means a linger elapsed.
    const bool still_flashing =
        entry.state.state == CurveTreeView::DatasetProgress::State::kFailed && entry.flash_phase < kFlashCycles * 2;
    if (still_flashing) {
      ++entry.flash_phase;
      if (entry.flash_phase >= kFlashCycles * 2) {
        startLingerTimer(entry, kLingerMs);  // last toggle done: hold, then retire
      } else {
        entry.state.flash_on = (entry.flash_phase % 2 == 0);
        entry.linger_or_flash_timer->start(kFlashCycleMs);
      }
      updateAndEmit();
    } else {
      // Linger elapsed for a completed, cancelled, or finished-flashing entry.
      removeEntry(it.key());
      updateAndEmit();
    }
    break;
  }
}

quint64 IngestProgressController::mintRowKey() {
  return next_row_key_++;
}

void IngestProgressController::updateAndEmit() {
  QHash<quint64, CurveTreeView::DatasetProgress> result;
  for (const auto& entry : ingests_) {
    result[entry.row_key] = entry.state;
  }
  emit progressUpdated(result);
}

void IngestProgressController::tryResolveDatasetPath(IngestEntry& entry) {
  if (!dataset_path_resolver_) {
    return;  // No resolver set yet
  }

  const QString resolved_path = dataset_path_resolver_(entry.token.dataset_id);
  if (resolved_path.isEmpty() && !entry.resolved_tree_path.isEmpty()) {
    // The dataset vanished under a live row (a discard deletes it). Keep the
    // row where it is rather than demoting it to a ghost mid-flight: it is
    // about to retire, and a row that moves and renames itself on the way out
    // reads as a different row entirely.
    return;
  }

  // Adopt the dataset's own name as the row label, and KEEP it once adopted.
  // The fallback is the producer's progress title ("Importing MCAP"), which
  // names an activity rather than a thing — fine as a last resort, wrong as an
  // identity. Stickiness matters because the dataset can vanish (a discard
  // deletes it) while the row is still on screen: without it the row would
  // rename itself mid-flight, which reads as a different row entirely.
  if (!resolved_path.isEmpty() && entry.state.display_name != resolved_path) {
    entry.state.display_name = resolved_path;
  }

  // Emit rowKeyChanged only if the path actually changed
  if (resolved_path != entry.resolved_tree_path) {
    entry.resolved_tree_path = resolved_path;
    emit rowKeyChanged(resolved_path, entry.row_key);
  }
}

void IngestProgressController::startLingerTimer(IngestEntry& entry, int linger_ms) {
  if (!entry.linger_or_flash_timer) {
    entry.linger_or_flash_timer = timer_factory_ ? timer_factory_() : new QTimer();
    // Parent it whatever its origin: a factory-supplied timer (the test seam) is
    // otherwise owned by nobody, and an entry can retire before it ever fires.
    entry.linger_or_flash_timer->setParent(this);
    connect(
        entry.linger_or_flash_timer, &QTimer::timeout, this, &IngestProgressController::onLingerOrFlashTimerTimeout);
  }
  // Always set single-shot mode before starting. This is critical when
  // transitioning from flash to linger: the flash timer is repeating, so we must
  // explicitly switch to single-shot here to ensure the linger fires exactly
  // once.
  entry.linger_or_flash_timer->setSingleShot(true);
  entry.linger_or_flash_timer->start(linger_ms);
}

void IngestProgressController::startFlashTimer(IngestEntry& entry) {
  if (!entry.linger_or_flash_timer) {
    entry.linger_or_flash_timer = timer_factory_ ? timer_factory_() : new QTimer();
    entry.linger_or_flash_timer->setParent(this);
    connect(
        entry.linger_or_flash_timer, &QTimer::timeout, this, &IngestProgressController::onLingerOrFlashTimerTimeout);
  }
  entry.flash_phase = 0;
  entry.state.flash_on = true;
  // Flash uses a repeating timer for the ~180ms on/off transitions. After 6
  // transitions (3 on/off cycles), the handler will transition to linger.
  entry.linger_or_flash_timer->setSingleShot(false);
  entry.linger_or_flash_timer->start(kFlashCycleMs);
}

void IngestProgressController::removeEntry(IngestId id) {
  auto it = ingests_.find(id);
  if (it != ingests_.end()) {
    if (it->linger_or_flash_timer) {
      it->linger_or_flash_timer->stop();
      it->linger_or_flash_timer->deleteLater();
    }
    ingests_.erase(it);
  }
}

}  // namespace PJ
