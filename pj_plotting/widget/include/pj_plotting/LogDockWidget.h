#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <optional>
#include <utility>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_runtime/IDataWidget.h"
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
/// Data: each topic is read from the session's ObjectStore and decoded once per entry (`decodeStoredObject`). Entries
/// landing on the session (dataPublished, ingestEnded, derivedEntriesPublished) bring an update at once, then at most
/// one per kEntriesLandedInterval; an update appends what arrived after the topic's cursor, and re-reads the topic
/// only when its content was rewritten (a late insert, a reload, a merge). A front eviction by retention leaves the
/// rows shown. A display-offset change re-reads every topic: a row's time is display-axis seconds
/// (`rawToDisplaySecondsAtOrAfter`, so a seek to a row reveals it). GUI thread only.
///
/// Persistence: one `<topic>` child per topic with the scene-dock identity attributes (`dataset_id`,
/// `dataset_source`, `dataset_path`, `topic`), so LayoutXml's dataset relocation applies, plus the filters, the
/// enabled levels and the future-rows choice. A topic that does not resolve (its dataset is not loaded) is kept,
/// re-serialized and retried whenever the catalog or a dataset's source path changes; `finishRestore` makes the last
/// attempt.
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
  void finishRestore() override;

  /// The app-wide time format of the Time column (the playback readout's).
  void setTimeLabelFormat(TimeLabelFormat format);
  /// An advertised topic with no storage yet (a placeholder drop): kept as pending until it materializes.
  void addPendingTopic(DatasetId dataset_id, const QString& topic_name);

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
    DatasetId dataset = 0;
    QString name;
    SequentialUID cursor;  ///< drainNewSince position: every entry up to it is in the view
    OrderedEntries::ContentIdentity identity;
  };
  /// A saved-but-unresolved topic: the raw identity attributes, preserved verbatim across save/load.
  struct PendingTopic {
    qint64 dataset_id = 0;
    QString dataset_source;
    QString dataset_path;
    QString topic;
  };

  /// Adds `topic_id` and reads its history; false when it is not a kLog topic or already shown.
  bool addTopic(ObjectTopicId topic_id);
  /// Brings one topic's rows up to date: appended entries, or everything when `full` or rewritten.
  void loadTopic(Topic& topic, bool full);
  /// Follows the store: drops topics that vanished (back to pending), loads new entries, everything on `reload_all_`.
  void refresh();
  bool tryResolve(const PendingTopic& pending);
  void retryPending();

  SessionManager* session_ = nullptr;
  QPointer<CatalogModel> catalog_;
  QPointer<PlaybackEngine> playback_;
  LogView* view_ = nullptr;
  std::vector<Topic> topics_;
  std::vector<PendingTopic> pending_;
  bool reload_all_ = false;
  CoalescingTrigger entries_landed_;
};

}  // namespace PJ
