#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QList>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
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
  // objectPayloadFormatFromMetadata of each side ("" = canonical objects); the same type with different formats still
  // conflicts, since one parser decodes the merge.
  std::string anchor_format;
  std::string source_format;
  // Same-named marker topics whose scope, target or owner differ: merging would keep
  // the anchor's description for both sets.
  bool marker_description_differs = false;
};

// Central runtime object: owns the long-lived application services. pj_app instantiates one at startup and wires the
// shell's widgets against the services it exposes.
class AppSession : public QObject {
  Q_OBJECT
 public:
  explicit AppSession(QObject* parent = nullptr);

  explicit AppSession(QString extensions_dir, QObject* parent = nullptr);

  AppSession(QString extensions_dir, DiagnosticSink sink, QObject* parent = nullptr);

  // Creates a session with application-composed statically linked plugins. The vtable/dialog pointers in the set must
  // have static storage duration: the catalog retains them.
  AppSession(QString extensions_dir, DiagnosticSink sink, StaticPluginSet static_plugins, QObject* parent = nullptr);

  ~AppSession() override;

  AppSession(const AppSession&) = delete;

  AppSession& operator=(const AppSession&) = delete;

  SessionManager& sessionManager() const {
    return *session_manager_;
  }

  PlaybackEngine& playbackEngine() const {
    return *playback_engine_;
  }

  CatalogModel& catalogModel() const {
    return *catalog_model_;
  }

  ExtensionCatalogService& extensionCatalog() const {
    return *extension_catalog_;
  }

  // Per streaming dataset, computes the topics currently displayed (plus always-active infrastructure) so a
  // demand-capable source subscribes to exactly that set. See TopicDemandTracker for the ref-counting contract.
  TopicDemandTracker& topicDemandTracker() const {
    return *topic_demand_tracker_;
  }

  // Delegates to SessionManager, which owns the registry so plot widgets reach it through the SessionManager they
  // already hold. Cleared whenever the catalog empties.
  [[nodiscard]] CurveColorRegistry& curveColorRegistry() const;

  // Recomputes the playback range from the time bounds of the CATALOG-VISIBLE topics and object topics (per topic, not
  // per dataset): the engine keeps removed/trashed topics' data as append-only tombstones, which must not stretch the
  // timeline. Recomputed from scratch, so the range also shrinks. Rebuild the catalog first (the load/ingest paths do).
  //
  // The playhead is seeded on the datasets present when it last snapped to the data minimum. A seed snaps currentTime
  // to the new minimum again once none of them is in the catalog (the first load, a cleared catalog, or data replaced
  // dataset by dataset: removed after the next data loaded); otherwise it keeps the scrub position (setRange re-clamps
  // it). A dataset under refill stays in the catalog, so a reload keeps the playhead. Returns false when no visible
  // topic has data.
  bool seedPlaybackFromSession();

  // FOCUSES playback on specific datasets (a toolbox bulk import, e.g. a cloud fetch): sets the range to THEIR bounds.
  // currentTime goes to their start on the first focus and when datasets not yet focused find it outside that range;
  // re-focusing the same growing import keeps it, clamped into the range. Unlike seedPlaybackFromSession it REPLACES
  // the range, so a 10 s snippet shows
  // a 10 s timeline even when older datasets span hours. Scalar series bound the range; object topics count only when
  // the datasets have no scalar data (a 3D-only import), since latched/static objects (tf_static, stale markers) must
  // not stretch it. Returns false when no data was found (callers fall back to seedPlaybackFromSession).
  bool focusPlaybackOnDatasets(const std::vector<DatasetId>& datasets);

  // RAW absolute-ns [min,max] across a dataset's catalog-visible topics (scalar + object), nullopt when none has
  // time-bearing data. Pre-offset (the caller applies displayOffset). Shared by the Timeline bars and the range
  // computation.
  [[nodiscard]] std::optional<PJ::Range<PJ::Timestamp>> datasetRawTimeRange(PJ::DatasetId dataset_id) const;

  // Recomputes the playback RANGE from the offset-adjusted union of visible topics, for the live Timeline drag path
  // (call throttled); never touches currentTime or the first-seed snap. Returns the union's display-min (what
  // seedPlaybackFromSession snaps the first-load playhead to). Returns nullopt when no topic has time-bearing data,
  // and then collapses the transport to the empty state (resetPlaybackToEmpty) ONLY if the catalog is genuinely empty:
  // a transient mid-reload (topics visible, chunks momentarily detached) leaves the range untouched. A dataset under
  // refill that the catalog still shows counts with the visible extent it had when the refill began, so the playhead
  // is not clamped to the other datasets; SessionManager::refillClosed drops it and recomputes.
  std::optional<PJ::DisplaySeconds> recomputeRange();

  // Sets the active live-stream dataset (0 = none). While set AND playing, recomputeRange() scopes the range to this
  // dataset's tip, so a far-away file dataset cannot widen range_max past the live edge and jolt the tip-pinned cursor.
  // A followed stream joins the playhead's seeding (live-follow drives the playhead, and the shell does not re-seed on
  // a removal while following), so removing the data seeded before it does not re-snap; a stream-only session stays
  // unseeded.
  void setActiveStreamingDataset(PJ::DatasetId id) {
    active_streaming_dataset_id_ = id;
    if (id != 0 && !seeded_datasets_.empty()) {
      seeded_datasets_.insert(id);
    }
  }
  [[nodiscard]] PJ::DatasetId activeStreamingDataset() const {
    return active_streaming_dataset_id_;
  }

  // DESTRUCTIVELY merges the selected datasets into one, honoring their Source-Timeline arrangement. The anchor is the
  // dataset whose displayed start is earliest (min(raw_min - displayOffset)); its raw clock is kept and the others are
  // shifted into it by their relative display offset. The anchor's topics keep their ids (curve keys survive); the
  // others are consumed and removed from the catalog; the anchor is relabelled "<name>_merged". Object topics fold by
  // shared name, or are reparented when source-only. Not undoable. Returns the anchor's DatasetId, or 0 on a no-op
  // (fewer than two selected datasets carry time-bearing data).
  PJ::DatasetId mergeDatasets(const std::vector<PJ::DatasetId>& selected);

  // Shared-name conflicts (canonical object type, payload format, marker description) in the merge plan
  // mergeDatasets() would use. Pure preflight; mutates nothing.
  [[nodiscard]] std::vector<ObjectMergeConflict> objectMergeConflicts(const std::vector<PJ::DatasetId>& selected) const;

 signals:
  void datasetsMerged(PJ::DatasetId anchor, QList<PJ::DatasetId> consumed);

 private:
  struct MergePlan {
    DatasetId anchor = 0;
    std::vector<DatasetMergeSource> sources;
  };

  // Raw [min,max] ns per visible time-bearing topic, passed to `visit` with the item's DatasetId. Walks the
  // catalog-visible items once, deduping multi-field scalar topics by topic_id and object topics by ObjectTopicId: the
  // one scan shared by seedPlaybackFromSession / recomputeRange / datasetRawTimeRange, so visibility and dedup rules
  // live in one place.
  void forEachVisibleRawRange(
      const std::function<void(PJ::DatasetId dataset_id, PJ::Timestamp raw_min, PJ::Timestamp raw_max)>& visit) const;

  [[nodiscard]] std::optional<MergePlan> planMerge(const std::vector<DatasetId>& selected) const;

  // Requests the on-demand recipe `key` at the playhead, converted to its dataset's raw time (a finding: its pin).
  void requestOnDemandAtPlayhead(const std::string& key);
  // Keeps the raw topics the demanded recipes of `dataset` read in the demand tracker's derived-input tier, so a
  // demand-capable stream sends them while only a derived topic is displayed.
  void demandOnDemandRawInputs(DatasetId dataset);
  // `demandOnDemandRawInputs` for every dataset whose tier is not empty, after topics were retired (a removal, a
  // re-binding).
  void refreshOnDemandRawInputs();

  // Collapses the transport to the no-data state: pauses, clears the live hold, resets the range to [0,0] and the
  // cursor to 0, and re-arms the first-seed snap. Called when the last visible data disappears (recomputeRange's empty
  // branch, CatalogModel::cleared()). Idempotent.
  void resetPlaybackToEmpty();

  // 0 = none; see setActiveStreamingDataset.
  PJ::DatasetId active_streaming_dataset_id_ = 0;
  // datasetRawTimeRange of each dataset when its refill began; counted by recomputeRange while the refill is open.
  std::unordered_map<PJ::DatasetId, PJ::Range<PJ::Timestamp>> refill_held_raw_ranges_;
  // The datasets whose derived-input tier holds topics (demandOnDemandRawInputs).
  std::set<PJ::DatasetId> raw_input_datasets_;

  std::unique_ptr<SessionManager> session_manager_;
  std::unique_ptr<PlaybackEngine> playback_engine_;
  std::unique_ptr<CatalogModel> catalog_model_;
  std::unique_ptr<TopicDemandTracker> topic_demand_tracker_;
  // Declared last: its ctor hits disk (scan + load) and must run after the
  // other services are alive. The destructor resets services explicitly so
  // session-owned plugin handles die before loaded plugin libraries unload.
  std::unique_ptr<ExtensionCatalogService> extension_catalog_;

  // The datasets the playhead was seeded on (seedPlaybackFromSession, or focusPlaybackOnDatasets, which adds the
  // focused ones and replaces the set when it snaps); empty = not seeded. Never grown by an added
  // file dataset, so data replaced without the catalog emptying re-seeds; a merge's anchor inherits its consumed
  // datasets, and a followed stream joins (setActiveStreamingDataset).
  std::set<PJ::DatasetId> seeded_datasets_;
};

}  // namespace PJ
