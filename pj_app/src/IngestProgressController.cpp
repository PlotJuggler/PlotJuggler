// SPDX-License-Identifier: MPL-2.0

#include "IngestProgressController.h"

#include <QLoggingCategory>

namespace {
// Every state transition; off by default, enable with QT_LOGGING_RULES="pj.app.ingestui*=true".
Q_LOGGING_CATEGORY(lcIngestUi, "pj.app.ingestui", QtWarningMsg)
}  // namespace

#include <QTimer>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace PJ {

namespace {
constexpr int kLingerMs = 1000;
constexpr int kFlashCycleMs = 180;  // per toggle
constexpr int kFlashCycles = 3;     // on/off pairs
}  // namespace

IngestProgressController::IngestProgressController(QObject* parent, std::function<QTimer*()> timer_factory)
    : QObject(parent), timer_factory_(std::move(timer_factory)) {}

IngestProgressController::~IngestProgressController() {
  for (auto& entry : ingests_) {
    if (entry.linger_or_flash_timer) {
      entry.linger_or_flash_timer->stop();
    }
  }
}

std::optional<IngestToken> IngestProgressController::tokenForRowKey(quint64 row_key) const {
  const auto it = ingests_.constFind(static_cast<IngestId>(row_key));
  return it != ingests_.cend() ? std::optional<IngestToken>(it->token) : std::nullopt;
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
    return;
  }

  // A live ingest already ended (the runtime ends one a restart supersedes): this catches a row still lingering
  // after its own terminal, which must not sit beside the new one.
  std::vector<IngestId> superseded;
  for (auto it = ingests_.cbegin(); it != ingests_.cend(); ++it) {
    if (it->token.dataset_id == token.dataset_id) {
      superseded.push_back(it.key());
    }
  }
  for (const IngestId id : superseded) {
    removeEntry(id);
  }

  IngestEntry entry;
  entry.token = token;
  entry.resolved_tree_path.clear();
  entry.state.display_name = label;
  entry.state.indeterminate = (total == 0);
  entry.state.fraction = 0.0;
  entry.state.state = CurveTreeView::DatasetProgress::State::kLoading;
  entry.state.flash_on = true;
  entry.state.cancellable = cancellable;
  entry.state.discardable = discardable;

  qCInfo(lcIngestUi) << "[ctl] BEGAN token=" << token.id << "total=" << total
                     << "indeterminate=" << entry.state.indeterminate;
  ingests_[token.id] = std::move(entry);

  // Before the first publication, so the row never flashes the producer's progress title first.
  tryResolveDatasetPath(ingests_[token.id]);

  updateAndEmit();
}

void IngestProgressController::onIngestProgressed(PJ::IngestToken token, quint64 current, quint64 total) {
  auto it = ingests_.find(token.id);
  if (it == ingests_.end()) {
    return;
  }

  IngestEntry& entry = *it;
  using State = CurveTreeView::DatasetProgress::State;
  // On every tick, before the state gate: the catalog can relabel a dataset mid-ingest (a sibling's removal turns
  // "foo (2)" back into "foo"), which moves its row.
  tryResolveDatasetPath(entry);

  // A stopping row stays frozen (the producer keeps delivering until it notices the stop) and a finished row keeps
  // its final bar while it lingers.
  if (entry.state.state != State::kLoading) {
    return;
  }
  entry.state.indeterminate = (total == 0);
  entry.state.fraction = (total > 0) ? std::min(1.0, static_cast<double>(current) / static_cast<double>(total)) : 0.0;
  qCInfo(lcIngestUi) << "[ctl] PROGRESS token=" << token.id << "cur=" << current << "total=" << total
                     << "fraction=" << entry.state.fraction;

  updateAndEmit();
}

void IngestProgressController::onIngestStopping(PJ::IngestToken token, bool /*keep_partial*/) {
  auto it = ingests_.find(token.id);
  if (it == ingests_.end()) {
    return;
  }
  // Presentation only: onIngestEnded still decides the final state.
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
    updateAndEmit();
  }
}

void IngestProgressController::onIngestEnded(PJ::IngestToken token, PJ::IngestOutcome outcome) {
  auto it = ingests_.find(token.id);
  if (it == ingests_.end()) {
    return;
  }

  IngestEntry& entry = *it;
  // A committed load has its row by now, even with no tick since it appeared: linger there, not on a ghost.
  if (entry.resolved_tree_path.isEmpty()) {
    tryResolveDatasetPath(entry);
  }
  qCInfo(lcIngestUi) << "[ctl] ENDED token=" << token.id << "outcome=" << static_cast<int>(outcome)
                     << "fraction_at_end=" << entry.state.fraction;

  switch (outcome) {
    case IngestOutcome::kCompleted:
      entry.state.state = CurveTreeView::DatasetProgress::State::kCompleted;
      entry.state.fraction = 1.0;
      entry.state.indeterminate = false;
      entry.flash_phase = 0;
      startLingerTimer(entry, kLingerMs);
      break;

    case IngestOutcome::kCancelled:
      // The user already knows, and a discard deleted the dataset the row would describe.
      removeEntry(token.id);
      updateAndEmit();
      return;

    case IngestOutcome::kFailed:
      entry.state.state = CurveTreeView::DatasetProgress::State::kFailed;
      // The fraction stays where the load died: the caption reports it; the view draws the bar full-width anyway.
      entry.state.indeterminate = false;
      entry.flash_phase = 0;
      startFlashTimer(entry);
      break;

    case IngestOutcome::kUnknown:
      removeEntry(token.id);
      updateAndEmit();
      return;
  }

  updateAndEmit();
}

void IngestProgressController::onLingerOrFlashTimerTimeout() {
  for (auto it = ingests_.begin(); it != ingests_.end(); ++it) {
    IngestEntry& entry = *it;
    if (!entry.linger_or_flash_timer) {
      continue;
    }

    QObject* sender = entry.linger_or_flash_timer;
    if (QObject::sender() != sender) {
      continue;
    }

    // By the phase counter, not the state alone: a failed entry's linger firing would re-enter the flash branch and
    // never retire.
    const bool still_flashing =
        entry.state.state == CurveTreeView::DatasetProgress::State::kFailed && entry.flash_phase < kFlashCycles * 2;
    if (still_flashing) {
      ++entry.flash_phase;
      if (entry.flash_phase >= kFlashCycles * 2) {
        startLingerTimer(entry, kLingerMs);
      } else {
        entry.state.flash_on = (entry.flash_phase % 2 == 0);
        entry.linger_or_flash_timer->start(kFlashCycleMs);
      }
      updateAndEmit();
    } else {
      removeEntry(it.key());
      updateAndEmit();
    }
    break;
  }
}

void IngestProgressController::updateAndEmit() {
  QHash<quint64, CurveTreeView::DatasetProgress> result;
  for (const auto& entry : ingests_) {
    result[entry.token.id] = entry.state;
  }
  emit progressUpdated(result);
}

void IngestProgressController::tryResolveDatasetPath(IngestEntry& entry) {
  if (!dataset_path_resolver_) {
    return;
  }

  const QString resolved_path = dataset_path_resolver_(entry.token.dataset_id);
  if (resolved_path.isEmpty() && !entry.resolved_tree_path.isEmpty()) {
    // The dataset vanished (a discard): the row is about to retire; moving it to a ghost would read as a new row.
    return;
  }

  // The dataset's name replaces the producer's progress title ("Importing MCAP") and is kept once adopted.
  if (!resolved_path.isEmpty() && entry.state.display_name != resolved_path) {
    entry.state.display_name = resolved_path;
  }

  if (resolved_path != entry.resolved_tree_path) {
    entry.resolved_tree_path = resolved_path;
    emit rowKeyChanged(resolved_path, entry.token.id);
  }
}

void IngestProgressController::startLingerTimer(IngestEntry& entry, int linger_ms) {
  if (!entry.linger_or_flash_timer) {
    entry.linger_or_flash_timer = timer_factory_ ? timer_factory_() : new QTimer();
    // Also a factory timer, which nobody else owns.
    entry.linger_or_flash_timer->setParent(this);
    connect(
        entry.linger_or_flash_timer, &QTimer::timeout, this, &IngestProgressController::onLingerOrFlashTimerTimeout);
  }
  // The timer may still be repeating from the flash phase.
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
