// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "SeriesProgressController.h"

#include <QSet>
#include <QTimer>
#include <algorithm>
#include <optional>
#include <string>

#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/SeriesRunner.h"
#include "pj_runtime/SessionManager.h"
#include "ui/CurveListPanel.h"

namespace PJ {

namespace {

using Progress = CurveTreeView::DatasetProgress;

bool sameProgress(const Progress& lhs, const Progress& rhs) {
  return lhs.state == rhs.state && lhs.indeterminate == rhs.indeterminate && lhs.fraction == rhs.fraction;
}

}  // namespace

SeriesProgressController::SeriesProgressController(QObject* parent) : QObject(parent) {}

void SeriesProgressController::setSessionManager(SessionManager* session) {
  if (session_ != nullptr) {
    disconnect(session_, nullptr, this, nullptr);
  }
  session_ = session;
  if (session_ != nullptr) {
    connect(session_, &SessionManager::seriesStateChanged, this, &SeriesProgressController::refresh);
  }
}

void SeriesProgressController::stopSeriesOfTopic(quint64 topic) {
  if (session_ == nullptr) {
    return;
  }
  SeriesRunner& series = session_->onDemandService().series();
  for (const std::string& key : session_->onDemandService().graph().keys()) {
    if (const std::optional<SeriesRunner::State> state = series.state(key); state && state->topic == topic) {
      (void)series.stop(key);
      return;
    }
  }
}

void SeriesProgressController::refresh() {
  if (session_ == nullptr) {
    return;
  }
  const OnDemandService& service = session_->onDemandService();
  bool changed = false;
  QSet<quint64> alive;
  for (const std::string& key : service.graph().keys()) {
    const std::optional<SeriesRunner::State> status = service.series().state(key);
    if (!status.has_value()) {
      continue;
    }
    const quint64 topic = status->topic;
    alive.insert(topic);
    const auto existing = entries_.find(topic);
    if (status->replaying && !status->complete) {
      Progress progress;
      progress.state = Progress::State::kLoading;
      progress.indeterminate = status->total == 0;
      progress.fraction = progress.indeterminate
                              ? 0.0
                              : std::min(1.0, static_cast<double>(status->done) / static_cast<double>(status->total));
      progress.cancellable = true;
      progress.discardable = false;
      if (existing == entries_.end() || !sameProgress(existing->progress, progress)) {
        Entry entry;
        entry.progress = progress;
        entry.linger_seq = ++next_seq_;  // also voids a linger of the replay this one replaces
        entries_.insert(topic, entry);
        changed = true;
      }
      continue;
    }
    if (existing == entries_.end() || existing->progress.state != Progress::State::kLoading) {
      continue;  // never shown, or already in its terminal linger
    }
    if (status->complete || status->stopped) {
      existing->progress.state = status->complete ? Progress::State::kCompleted : Progress::State::kCancelled;
      existing->progress.fraction = 1.0;
      existing->progress.indeterminate = false;
      startLinger(topic, *existing);
    } else {
      entries_.erase(existing);  // it ended without a verdict (no longer wanted, a chunk that could not run)
    }
    changed = true;
  }
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (alive.contains(it.key())) {
      ++it;
    } else {
      it = entries_.erase(it);
      changed = true;
    }
  }
  if (changed) {
    emitProgress();
  }
}

void SeriesProgressController::startLinger(quint64 topic, Entry& entry) {
  entry.linger_seq = ++next_seq_;
  const uint64_t seq = entry.linger_seq;
  QTimer::singleShot(linger_ms_, this, [this, topic, seq] {
    const auto it = entries_.find(topic);
    if (it == entries_.end() || it->linger_seq != seq) {
      return;
    }
    entries_.erase(it);
    emitProgress();
  });
}

void SeriesProgressController::emitProgress() {
  QHash<quint64, Progress> by_topic;
  for (auto it = entries_.cbegin(); it != entries_.cend(); ++it) {
    by_topic.insert(it.key(), it->progress);
  }
  emit progressUpdated(by_topic);
}

void wireSeriesProgress(
    SessionManager& session, SeriesProgressController& controller, CurveListPanel& panel, QObject* context) {
  controller.setSessionManager(&session);
  QObject::connect(&controller, &SeriesProgressController::progressUpdated, &panel, &CurveListPanel::setSeriesProgress);
  QObject::connect(&panel, &CurveListPanel::seriesStopRequested, context, [&controller](quint64 topic) {
    controller.stopSeriesOfTopic(topic);
  });
}

}  // namespace PJ
