#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include "pj_base/types.hpp"

namespace PJ {

// Computes, per streaming dataset, the set of topics currently "in demand" —
// displayed by at least one widget, or pinned as always-active infrastructure
// (TF / CameraInfo) — so a demand-capable streaming source can subscribe to
// exactly that set and leave the rest paused.
//
// Identity is (DatasetId, topic_name): the string a streaming source
// advertises and binds by. A displayed widget resolves to a topic name and
// calls addReference/removeReference; N curves/fields backed by one topic
// count once (reference-counted) — callers must balance every addReference
// with a matching removeReference. A *pending* curve binding (e.g. a
// drag-and-drop still resolving, or a catalog placeholder awaiting real data)
// counts as a reference too: register it at pend time and release it on
// cancel or hand-off to the real binding, or the topic never subscribes.
//
// Threading: GUI-thread only — all methods must be called from the thread
// that owns this object (widget signals, catalog callbacks).
//
// No debounce/hysteresis by design: every mutator recomputes and emits
// immediately. A consumer that wants to avoid thrashing a real subscription
// on rapid add/remove churn (e.g. dragging a curve in and out of a plot)
// applies the emitted set at its own poll cadence, which naturally absorbs
// flapping — this class does not need to know about that policy.
class TopicDemandTracker : public QObject {
  Q_OBJECT
 public:
  explicit TopicDemandTracker(QObject* parent = nullptr);

  TopicDemandTracker(const TopicDemandTracker&) = delete;
  TopicDemandTracker& operator=(const TopicDemandTracker&) = delete;

  // A consumer began (addReference) / stopped (removeReference) displaying a
  // series backed by this topic. removeReference is defensive: an unknown
  // dataset, an unknown topic, or a count already at zero is silently
  // ignored (unbalanced calls must never crash or emit).
  void addReference(DatasetId dataset_id, const QString& topic_name);
  void removeReference(DatasetId dataset_id, const QString& topic_name);

  // Declarative replacement of the per-dataset infrastructure tier: topics
  // kept active regardless of display. Replaces the previous set wholesale;
  // an identical set is a no-op (no emission).
  void setInfrastructureTopics(DatasetId dataset_id, const std::vector<QString>& topic_names);

  // The derived-input tier: the raw topics a displayed derived topic reads
  // (AppSession sets it from the demanded on-demand recipes), so they stream
  // while only the derived topic is displayed. Replaced wholesale, like the
  // infrastructure tier; it is not a display reference (hasAnyReference).
  void setDerivedInputTopics(DatasetId dataset_id, const std::vector<QString>& topic_names);

  // User-driven "force topic streaming" tier: a forced topic streams (and keeps
  // accumulating history) regardless of display — the only way to collect data
  // for a topic BEFORE it is first shown. Forcing is a tier, never an override:
  // stopping forced streaming cannot pause a topic that is still displayed or
  // infra-pinned. Idempotent; emits only on an actual active-set change.
  void setTopicForced(DatasetId dataset_id, const QString& topic_name, bool forced);
  [[nodiscard]] bool isTopicForced(DatasetId dataset_id, const QString& topic_name) const;
  // The dataset's forced set (sorted), for UI that badges forced rows.
  [[nodiscard]] std::vector<QString> forcedTopics(DatasetId dataset_id) const;

  // Forgets all references and infrastructure for a dataset (source
  // stopped/removed). Emits an empty active set only if it wasn't already.
  void clearDataset(DatasetId dataset_id);

  // Current active set for a dataset: referenced (count > 0) union infra,
  // sorted and de-duplicated. Empty vector for an unknown dataset.
  [[nodiscard]] std::vector<QString> activeTopics(DatasetId dataset_id) const;

  // True while a widget displays any of the dataset's topics. Unlike activeTopics,
  // the infrastructure and forced tiers do not count.
  [[nodiscard]] bool hasAnyReference(DatasetId dataset_id) const;

 signals:
  // Emitted whenever a dataset's active-topic set changes. `active_topics` is
  // the complete current set (sorted, de-duplicated), not a delta. A slot may
  // synchronously mutate the tracker: nested emissions are flattened so every
  // slot's LAST delivery for a dataset carries the newest state (a slot may
  // transiently observe a superseded set, and reverting churn inside a slot
  // can produce one duplicate delivery — consumers apply the set idempotently).
  void activeTopicsChanged(DatasetId dataset_id, const std::vector<QString>& active_topics);
  // Emitted whenever a dataset's FORCED set changes (setTopicForced toggles, or
  // clearDataset drops a non-empty forced set). Distinct from
  // activeTopicsChanged, which stays silent when forcing a topic that was
  // already active through a display reference — UI badging must hear about
  // that case too.
  void forcedTopicsChanged(DatasetId dataset_id);
  // Emitted whenever hasAnyReference(dataset_id) flips. Distinct from
  // activeTopicsChanged, which stays silent when a topic already streaming
  // through the infrastructure or forced tier gets displayed: the batch
  // engine's demand gate must hear about that case too.
  void referenceDemandChanged(DatasetId dataset_id);

 private:
  struct DatasetState {
    std::map<QString, int> ref_counts;  // topic name -> live display references (> 0)
    std::set<QString> infra;            // always-active while streaming
    std::set<QString> derived_inputs;   // read by a displayed derived topic
    std::set<QString> forced;           // user-forced streaming (context menu)
    std::vector<QString> last_emitted;  // last active set handed out (sorted, unique)
  };

  [[nodiscard]] static std::vector<QString> computeActive(const DatasetState& state);
  void recomputeAndEmit(DatasetId dataset_id, DatasetState& state);
  // Replaces one tier (`infra`, `derived_inputs`) of the dataset's state.
  void replaceTier(DatasetId dataset_id, std::set<QString> DatasetState::* tier, const std::vector<QString>& names);
  // Queues one (possibly overwriting) delivery per dataset and drains the
  // queue iteratively, so a mutation made by a connected slot never delivers
  // an older set after a newer one.
  void deliver(DatasetId dataset_id, std::vector<QString> active);

  std::unordered_map<DatasetId, DatasetState> datasets_;
  std::map<DatasetId, std::vector<QString>> pending_emits_;
  bool emitting_ = false;
};

}  // namespace PJ
