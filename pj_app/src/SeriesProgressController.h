#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QHash>
#include <QObject>
#include <cstdint>

#include "pj_widgets/CurveTreeView.h"

namespace PJ {

class CurveListPanel;
class SessionManager;

/// Bridges the replay progress of the on-demand series (`SeriesRunner::State`) to CurveTreeView's plain-typed
/// progress state, the way IngestProgressController does for dataset loads. Entries are keyed by the series' engine
/// topic id.
///
/// A replay (`replaying && !complete`) is a kLoading entry (determinate once its clock is counted, cancellable, not
/// discardable: stopping keeps the rows already computed). Its end is kCompleted or kCancelled for a short linger,
/// then the entry goes. A series that vanishes (removed, retired) drops its entry at once. A live append of a complete
/// series never shows a bar.
class SeriesProgressController : public QObject {
  Q_OBJECT

 public:
  explicit SeriesProgressController(QObject* parent = nullptr);

  /// Reads the series states from `session` on every `SessionManager::seriesStateChanged`. Call once.
  void setSessionManager(SessionManager* session);

  /// How long a terminal state stays on its row. Tests shorten it.
  void setLingerMs(int milliseconds) {
    linger_ms_ = milliseconds;
  }

  /// Stops the series whose engine topic is `topic` (what a row's stop click means), keeping its rows. A stale topic,
  /// or a replay already over, is a no-op.
  void stopSeriesOfTopic(quint64 topic);

  /// Re-reads every series and emits `progressUpdated` when the set changed.
  void refresh();

 signals:
  /// The full, current progress set keyed by series engine topic id.
  void progressUpdated(QHash<quint64, PJ::CurveTreeView::DatasetProgress> by_topic);

 private:
  struct Entry {
    CurveTreeView::DatasetProgress progress;
    uint64_t linger_seq = 0;  ///< a linger timer carrying an older value is stale
  };

  void startLinger(quint64 topic, Entry& entry);
  void emitProgress();

  SessionManager* session_ = nullptr;
  QHash<quint64, Entry> entries_;
  int linger_ms_ = 1000;
  uint64_t next_seq_ = 0;
};

/// Connects the series progress chain: SessionManager -> SeriesProgressController -> the Custom Topics rows of
/// `panel`, plus the row's stop click back to the controller. `context` scopes the stop connection to the caller.
void wireSeriesProgress(
    SessionManager& session, SeriesProgressController& controller, CurveListPanel& panel, QObject* context);

}  // namespace PJ
