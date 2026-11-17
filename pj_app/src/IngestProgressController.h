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

/// Turns SessionManager's ingest signals into CurveTreeView progress rows, one per ingest, keyed by IngestId (the row
/// key the view reports clicks with). Terminals: completed lingers, then retires; failed flashes, lingers, retires;
/// cancelled and unknown retire at once. An ingest whose dataset has no catalog row yet shows on a ghost row until
/// it gets one. Unknown tokens are ignored; a new ingest on a dataset retires that dataset's old row.
class IngestProgressController : public QObject {
  Q_OBJECT

 public:
  /// `timer_factory`: a test seam to drive linger and flash timers; null creates plain QTimers.
  explicit IngestProgressController(QObject* parent = nullptr, std::function<QTimer*()> timer_factory = nullptr);
  ~IngestProgressController() override;

  IngestProgressController(const IngestProgressController&) = delete;
  IngestProgressController& operator=(const IngestProgressController&) = delete;

  void setSessionManager(SessionManager* session);

  /// A dataset's tree path (its catalog label); empty while it has no row.
  using DatasetPathResolver = std::function<QString(DatasetId)>;

  /// Whatever the resolver captures must outlive the controller.
  void setDatasetPathResolver(DatasetPathResolver resolver);

  /// Re-resolves every ghost row; call it whenever the catalog gains items, or a ghost shadows the dataset's own row
  /// for the rest of the load.
  void refreshDatasetPaths();

  /// The ingest a row belongs to; nullopt once its entry retired. The only way to map a clicked row to a load:
  /// several run in parallel.
  [[nodiscard]] std::optional<IngestToken> tokenForRowKey(quint64 row_key) const;

 signals:
  /// The full current set, after every change.
  void progressUpdated(QHash<quint64, PJ::CurveTreeView::DatasetProgress> by_row_key);

  /// A row's tree path changed (empty: ghost). Sent before the ingest's first progressUpdated, so the row is named
  /// after its dataset from the start.
  void rowKeyChanged(QString tree_path, quint64 row_key);

 private slots:
  void onIngestBegan(PJ::IngestToken token, QString label, quint64 total, bool cancellable, bool discardable);
  void onIngestProgressed(PJ::IngestToken token, quint64 current, quint64 total);
  void onIngestStopping(PJ::IngestToken token, bool keep_partial);
  void onIngestEnded(PJ::IngestToken token, PJ::IngestOutcome outcome);
  void onLingerOrFlashTimerTimeout();

 private:
  struct IngestEntry {
    IngestToken token;           // token.id is the row key
    QString resolved_tree_path;  // last emitted; empty while a ghost
    CurveTreeView::DatasetProgress state;
    QTimer* linger_or_flash_timer = nullptr;  // one timer: repeating while flashing, single-shot while lingering
    int flash_phase = 0;                      // toggles done; flashing ends at 2 * kFlashCycles
  };

  void updateAndEmit();

  /// Emits rowKeyChanged when the path differs from the last one emitted.
  void tryResolveDatasetPath(IngestEntry& entry);

  void startLingerTimer(IngestEntry& entry, int linger_ms);
  void startFlashTimer(IngestEntry& entry);
  void removeEntry(IngestId id);

  SessionManager* session_ = nullptr;
  QHash<IngestId, IngestEntry> ingests_;
  std::function<QTimer*()> timer_factory_;
  DatasetPathResolver dataset_path_resolver_;
};

}  // namespace PJ
