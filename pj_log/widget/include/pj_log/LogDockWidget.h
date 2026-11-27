#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_runtime/IDataWidget.h"
#include "pj_runtime/SavedSeriesPath.h"
#include "pj_widgets/CoalescingTrigger.h"
#include "pj_widgets/TimeFormat.h"

namespace PJ {

class CatalogModel;
class LogView;
class PlaybackEngine;
class SessionManager;

/// The Log panel: a LogView over the log topics (`sdk::BuiltinObjectType::kLog`) dropped on it, merged by time, as the
/// `IDataWidget` the dock system hosts. Kind string and XML payload tag: `log`.
///
/// Data: the panel holds no copy of a topic. It indexes the topic's rows in the session's ObjectStore
/// (`ObjectStore::logView`) and reads the ones it shows or filters through a LogRowSource that takes a view per read.
/// Entries landing on the session (dataPublished, ingestEnded, derivedEntriesPublished) bring an update at once, then
/// at most one per kEntriesLandedInterval; an update drops the rows the store evicted and indexes the new ones. A late
/// entry (`contentIdentity().rewrite` moved) re-indexes the topic from the oldest late entry on; a removal (a reload, a
/// merge) or a display-offset change re-indexes the whole topic: a row's time is display-axis seconds
/// (`rawToDisplaySecondsAtOrAfter`, so a seek to a row reveals it). Only canonical kLog topics (stored as LogEntries)
/// are accepted. GUI thread only.
///
/// Persistence: one `<topic>` child per topic with the scene-dock identity attributes (`dataset_id`,
/// `dataset_source`, `dataset_path`, `topic`), so LayoutXml's dataset relocation applies, plus the filters, the
/// enabled levels and the future-rows choice. A topic keeps the identity it was added with, so one whose dataset left
/// still names it. A topic that does not resolve (its dataset is not loaded) is kept, re-serialized and retried
/// whenever the catalog or a dataset's source path changes; `finishRestore` makes the last attempt.
class LogDockWidget : public QWidget, public IDataWidget {
  Q_OBJECT
 public:
  /// Entries landing bring an update at once, then at most one more per interval: a progressive load commits many
  /// times a second.
  static constexpr int kEntriesLandedIntervalMs = 250;

  /// A topic the panel waits for: its name and the dataset its identity resolves to now, when exactly one does.
  struct PendingDemand {
    QString topic_name;
    std::optional<DatasetId> preferred_dataset;
    bool operator==(const PendingDemand&) const = default;
  };

  /// @p session, @p catalog and @p playback are non-null and outlive the panel.
  LogDockWidget(SessionManager* session, CatalogModel* catalog, PlaybackEngine* playback, QWidget* parent = nullptr);
  ~LogDockWidget() override;

  // --- IDataWidget ---
  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double time) override;
  /// Accepts a kLog topic it does not show yet; it merges with the topics already shown.
  bool tryAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;
  [[nodiscard]] bool canAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type) const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;
  [[nodiscard]] bool xmlValidateState(const QDomElement& element) const override;
  void finishRestore() override;
  /// The topic names of the pending topics.
  [[nodiscard]] QStringList unresolvedRestores() const override;

  /// The app-wide time format of the Time column (the playback readout's).
  void setTimeLabelFormat(TimeLabelFormat format);
  /// An advertised topic with no storage yet (a placeholder drop): kept as pending until it materializes.
  void addPendingTopic(DatasetId dataset_id, const QString& topic_name);
  /// The user deleted a topic, or a whole dataset: shown or pending, it leaves the panel instead of waiting to return.
  /// Called before the store drops it.
  void discardTopic(DatasetId dataset_id, const QString& topic_name);
  void discardDataset(DatasetId dataset_id);

  /// (dataset, topic name) of every topic shown, for demand tracking.
  [[nodiscard]] std::vector<std::pair<DatasetId, QString>> displayedTopics() const;
  /// (dataset, object topic) of every topic shown.
  [[nodiscard]] std::vector<std::pair<DatasetId, ObjectTopicId>> displayedObjectTopics() const;
  [[nodiscard]] std::vector<PendingDemand> pendingDemands() const;
  [[nodiscard]] bool hasPendingTopic(const QString& topic_name) const;

  [[nodiscard]] LogView* view() const noexcept {
    return view_;
  }

 signals:
  /// A user gesture changed the persisted state (a filter, a level, the Topics menu). The shell debounces it into a
  /// workspace-undo snapshot, as for the scene docks.
  void workspaceChanged();
  /// The shown or pending topics changed.
  void topicsChanged();

 private:
  struct Topic {
    ObjectTopicId id;
    SavedSeriesPath saved;  ///< its layout identity (no field), taken while its dataset is loaded
    /// The newest UID indexed: entries after it are the ones a late insert added.
    SequentialUID cursor;
    OrderedEntries::ContentIdentity identity;
    uint64_t end = 0;  ///< absolute number past the last row indexed
  };

  [[nodiscard]] bool shows(ObjectTopicId topic_id) const;
  [[nodiscard]] SavedSeriesPath savedIdentity(DatasetId dataset_id, const QString& topic_name) const;
  /// The dataset @p saved binds to, as tryResolve() binds it.
  [[nodiscard]] std::optional<DatasetId> resolveDataset(const SavedSeriesPath& saved) const;
  /// Adds `topic_id` and reads its history; false when it is not a kLog topic or already shown. Emits nothing: the
  /// caller emits topicsChanged.
  bool addTopic(ObjectTopicId topic_id);
  /// Brings one topic's index up to date: the new rows, the rows from a late entry on, or everything when `full` or a
  /// row it indexed was removed.
  void loadTopic(Topic& topic, bool full);
  /// Follows the store: drops topics that vanished (back to pending), loads new entries, everything on `reload_all_`.
  void refresh();
  /// Binds @p pending, adding its topic; true when it resolves, to a topic shown already too.
  bool tryResolve(const SavedSeriesPath& pending);
  void retryPending();
  void discardIf(const std::function<bool(const SavedSeriesPath&)>& matches);

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  PlaybackEngine* playback_ = nullptr;
  LogView* view_ = nullptr;
  std::vector<Topic> topics_;
  /// Saved but unresolved, written back verbatim.
  std::vector<SavedSeriesPath> pending_;
  bool reload_all_ = false;
  CoalescingTrigger entries_landed_;
};

}  // namespace PJ
