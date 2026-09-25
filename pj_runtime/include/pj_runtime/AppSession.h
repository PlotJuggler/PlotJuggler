#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QList>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/types.hpp"  // PJ::DatasetId, PJ::Range, PJ::Timestamp
#include "pj_datastore/merge_result.hpp"
#include "pj_runtime/Time.h"  // PJ::DisplaySeconds (recomputeRange return)

namespace PJ {

class CatalogModel;
class CurveColorRegistry;
class ExtensionCatalogService;
class PlaybackEngine;
class SessionManager;
class TopicDemandTracker;
struct StaticPluginSet;

struct ObjectMergeConflict {
  std::string topic_name;
  DatasetId source_dataset_id = 0;
  sdk::BuiltinObjectType anchor_type{};
  sdk::BuiltinObjectType source_type{};
  // objectPayloadFormatFromMetadata of each side ("" = canonical objects). Same
  // type but different formats is a conflict too: one parser decodes the merge.
  std::string anchor_format;
  std::string source_format;
};

// Central runtime object for PlotJuggler 4. Owns long-lived application
// services; pj_app instantiates one of these at startup and wires the
// shell's widgets against the services it exposes.
class AppSession : public QObject {
  Q_OBJECT
 public:
  // Creates a session using the default extension directory.
  explicit AppSession(QObject* parent = nullptr);

  // Creates a session using an explicit extension directory.
  explicit AppSession(QString extensions_dir, QObject* parent = nullptr);

  // Creates a session with an explicit extension directory and diagnostics.
  AppSession(QString extensions_dir, DiagnosticSink sink, QObject* parent = nullptr);

  // Creates a session with application-composed statically linked plugins.
  // The vtable/dialog pointers inside the set must have static storage
  // duration — the catalog retains them for its lifetime.
  AppSession(QString extensions_dir, DiagnosticSink sink, StaticPluginSet static_plugins, QObject* parent = nullptr);

  // Releases all long-lived application services.
  ~AppSession() override;

  // AppSession owns stateful services and cannot be copied.
  AppSession(const AppSession&) = delete;

  // AppSession owns stateful services and cannot be assigned.
  AppSession& operator=(const AppSession&) = delete;

  // Returns the session manager owned by this session.
  SessionManager& sessionManager() const {
    return *session_manager_;
  }

  // Returns the playback engine owned by this session.
  PlaybackEngine& playbackEngine() const {
    return *playback_engine_;
  }

  // Returns the catalog model owned by this session.
  CatalogModel& catalogModel() const {
    return *catalog_model_;
  }

  // Returns the extension catalog owned by this session.
  ExtensionCatalogService& extensionCatalog() const {
    return *extension_catalog_;
  }

  // Returns the demand tracker owned by this session: computes, per streaming
  // dataset, the topics currently displayed (union always-active infrastructure),
  // so a demand-capable streaming source can subscribe to exactly that set and
  // leave the rest paused. See TopicDemandTracker for the ref-counting contract.
  TopicDemandTracker& topicDemandTracker() const {
    return *topic_demand_tracker_;
  }

  // Returns the session's curve-color registry. It is owned by SessionManager
  // (so plot widgets can reach it through the SessionManager pointer they hold
  // without it being threaded through their constructors); this is a convenience
  // delegate. Remembers each curve's color so it stays consistent across plots
  // (issue #68); cleared whenever the catalog empties.
  [[nodiscard]] CurveColorRegistry& curveColorRegistry() const;

  // Recomputes the playback range from the time bounds of the CATALOG-VISIBLE
  // topics + object topics (per-topic granularity, not per-dataset).
  // Visibility matters: the engine keeps removed/trashed topics' data
  // (append-only tombstones), and those must not stretch the timeline.
  // Recomputed from scratch each call, so the range also shrinks (remove or
  // trash data, reload a shorter file). Caller contract: rebuild the catalog
  // first, as the load/ingest paths already do.
  //
  // First seed after the session starts (or after the catalog emptied): also
  // snaps currentTime to the new minimum so the user lands at the start of the
  // data. Later seeds preserve the scrub position (setRange re-clamps it).
  //
  // Returns true if any visible topic with data was found and the engine was
  // updated; false when there is none.
  bool seedPlaybackFromSession();

  // FOCUS playback on specific datasets (a toolbox bulk import, e.g. a cloud
  // fetch): set the range to THEIR time bounds and snap currentTime to their
  // start — unlike seedPlaybackFromSession this intentionally REPLACES the
  // range, so a 10s snippet presents a 10s timeline even when older datasets
  // span hours. Scalar series bound the range; object topics are consulted
  // only when the datasets carry no scalar data at all (a 3D-only import) —
  // latched/static objects (tf_static, stale markers) must not stretch it.
  //
  // Returns true if any data was found and the engine was updated (callers
  // fall back to seedPlaybackFromSession otherwise).
  bool focusPlaybackOnDatasets(const std::vector<DatasetId>& datasets);

  // RAW [min,max] absolute-ns time bounds across a dataset's catalog-visible
  // topics (scalar + object), or nullopt if the dataset has no time-bearing
  // data. Pre-offset (the caller applies displayOffset). Shared by the Timeline
  // bars and the playback-range computation.
  [[nodiscard]] std::optional<PJ::Range<PJ::Timestamp>> datasetRawTimeRange(PJ::DatasetId dataset_id) const;

  // Recompute the playback RANGE from the offset-adjusted union of visible
  // topics; when data remains this touches only the range (never currentTime,
  // never the first-seed snap), for the live Timeline drag path — call
  // throttled. Returns the union's display-min (the value seedPlaybackFromSession
  // snaps the first-load playhead to). Returns nullopt when no topic has
  // time-bearing data; in that case it collapses the transport to the empty
  // state (resetPlaybackToEmpty: paused, [0,0] range, cursor 0) ONLY if the
  // catalog is genuinely empty — a transient mid-reload (topics still visible
  // but their chunks momentarily detached) leaves the current range untouched.
  std::optional<PJ::DisplaySeconds> recomputeRange();

  // Tell the session which dataset is the active live stream (0 = none). While set
  // AND playback is playing, recomputeRange() scopes the playback range to this
  // dataset's tip only (not the union of all visible data), so a far-away file
  // dataset can't widen range_max past the live edge and jolt the tip-pinned cursor.
  void setActiveStreamingDataset(PJ::DatasetId id) {
    active_streaming_dataset_id_ = id;
  }
  [[nodiscard]] PJ::DatasetId activeStreamingDataset() const {
    return active_streaming_dataset_id_;
  }

  // DESTRUCTIVELY merge the selected datasets into one (the OR/union), honoring
  // their Source-Timeline arrangement. The anchor is the dataset whose displayed
  // start is earliest (leftmost = min(raw_min - displayOffset)); its raw clock is
  // kept and the others are shifted into it by their relative display offset, so
  // the merged dataset starts at the anchor's absolute time and the data lands
  // where the user arranged it. The anchor's topics keep their ids (curve keys
  // survive); the other selected datasets are consumed and removed from the
  // catalog, and the anchor is relabelled "<name>_merged". Object topics are
  // folded by shared name or reparented when source-only. Not undoable.
  // Returns the surviving anchor's DatasetId (the merged dataset), or 0 on a
  // no-op (fewer than two of the selected datasets carry time-bearing data).
  PJ::DatasetId mergeDatasets(const std::vector<PJ::DatasetId>& selected);

  // Returns shared-name canonical object type conflicts for the merge plan that
  // mergeDatasets() would use. Pure preflight; does not mutate stores/catalog.
  [[nodiscard]] std::vector<ObjectMergeConflict> objectMergeConflicts(const std::vector<PJ::DatasetId>& selected) const;

 signals:
  void datasetsMerged(PJ::DatasetId anchor, QList<PJ::DatasetId> consumed);

 private:
  struct MergePlan {
    DatasetId anchor = 0;
    std::vector<DatasetMergeSource> sources;
  };

  // Per-visible-time-bearing-topic raw bounds, surfaced to a callback. Walks the
  // catalog-visible items once, deduping multi-field scalar topics by topic_id
  // and object topics by ObjectTopicId, skipping items with no time-bearing
  // data. The single scan shared by seedPlaybackFromSession / recomputeRange /
  // datasetRawTimeRange so the visibility + dedup rules live in one place. The
  // callback receives the item's DatasetId and the topic's RAW [min,max] ns.
  void forEachVisibleRawRange(
      const std::function<void(PJ::DatasetId dataset_id, PJ::Timestamp raw_min, PJ::Timestamp raw_max)>& visit) const;

  [[nodiscard]] std::optional<MergePlan> planMerge(const std::vector<DatasetId>& selected) const;

  // Collapse the transport to the no-data state: pause, clear live hold, reset
  // the range to [0,0] and the cursor to 0, and re-arm the first-seed snap.
  // Called whenever the last visible data disappears (recomputeRange's empty
  // branch and the CatalogModel::cleared() hook). Idempotent.
  void resetPlaybackToEmpty();

  // The active live-streaming dataset (0 = none); see setActiveStreamingDataset.
  PJ::DatasetId active_streaming_dataset_id_ = 0;

  std::unique_ptr<SessionManager> session_manager_;
  std::unique_ptr<PlaybackEngine> playback_engine_;
  std::unique_ptr<CatalogModel> catalog_model_;
  std::unique_ptr<TopicDemandTracker> topic_demand_tracker_;
  // Declared last: its ctor hits disk (scan + load) and must run after the
  // other services are alive. The destructor resets services explicitly so
  // session-owned plugin handles die before loaded plugin libraries unload.
  std::unique_ptr<ExtensionCatalogService> extension_catalog_;

  // Flips to true on the first successful seedPlaybackFromSession(); re-armed
  // (set false) by the CatalogModel::cleared() hook when the catalog empties.
  // Used to distinguish "first load" (snap currentTime) from "additional load"
  // (preserve currentTime).
  bool playback_seeded_ = false;
};

}  // namespace PJ
