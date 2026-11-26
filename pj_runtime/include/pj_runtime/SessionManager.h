#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/resident_payload_pool.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_runtime/CurveColorRegistry.h"
#include "pj_runtime/ObjectIngestTap.h"
#include "pj_runtime/Time.h"

namespace pj::tf {
class TransformSnapshot;
}  // namespace pj::tf

namespace PJ {

class MessageParserPluginBase;
class DataProcessorService;
class BatchEngine;
class EnginePool;
class InFlightBudget;
class OnDemandService;
class RefillGuard;
class TransformService;
namespace scripting {
class IImageDecoder;
class IVideoFrameSource;
}  // namespace scripting

/// A persisted dataset identity resolved against the live session. `id == nullopt` with `ambiguous == false` means
/// "not loaded yet" (a deferred restore waits); `ambiguous` means several candidates (the restore rejects instead of
/// choosing by load order).
struct DatasetIdentityResolution {
  std::optional<DatasetId> id;
  bool ambiguous = false;
};

/// How a provider plugin can re-obtain a dataset: the plugin (`provider_id`), its durable key for the source
/// (`source_identity`, a fast-path key only) and the canonical descriptor JSON. Keyed by dataset, not by
/// LoadedSource: one file or fetch fans out to several datasets, each with its own record.
struct SourceRecord {
  QString provider_id;
  QString source_identity;
  QString descriptor_json;
};

/// Facts a loader read out of the loaded artifact, attributed to the emitting plugin. The host renders it
/// generically and never interprets keys; unlike SourceRecord it does not say how to re-obtain the data.
struct DatasetMetadata {
  QString plugin_id;
  QString json;  ///< Validated JSON object document, stored verbatim.
};

/// 0 = invalid/stale.
using IngestId = quint64;

/// One ingest run on a dataset. A new beginIngest on the dataset supersedes the token: later calls carrying the old
/// one are silent no-ops, so a late terminal of an old run cannot erase the new run's entry.
struct IngestToken {
  IngestId id = 0;  // monotonic, 0 = invalid/stale
  DatasetId dataset_id = 0;
};

/// Outcome of a completed ingest operation.
enum class IngestOutcome {
  kCompleted,
  kCancelled,  // requestCancel was accepted, whatever the producer reported
  kFailed,
  kUnknown,  // the plugin C-ABI's on_ingest_finished carries no outcome
};

/// How a producer stops itself once requestCancel is accepted. A producer that cannot honour `keep_partial` ignores it.
using IngestCancelFn = std::function<void(bool keep_partial)>;

/// Whether a producer can discard on stop. The toolbox ingest ABI has no host-side rollback, so a toolbox stop always
/// keeps; declaring that lets the UI disable the discard affordance instead of silently turning it into a keep.
enum class IngestStop {
  kKeepOrDiscard,  ///< keep_partial is honoured either way
  kStopOnly,       ///< stopping always keeps; a discard request must be refused
};

// Owns the datastore of the app session. Call it on the GUI thread, so plot adapters never see a mutation during
// paint. Exception: the object-topic parser registry is written from streaming worker threads (a plugin discovers a
// topic mid-stream) and read from the GUI thread every render tick, under object_parsers_mutex_.
class SessionManager : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::shared_ptr<SessionManager>;

  explicit SessionManager(QObject* parent = nullptr);
  ~SessionManager() override;

  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;

  [[nodiscard]] DataEngine& dataEngine() noexcept {
    return data_engine_;
  }
  [[nodiscard]] ObjectStore& objectStore() noexcept {
    return object_store_;
  }

  /// Session-wide object ingest taps shared by every runtime host.
  [[nodiscard]] ObjectIngestTapRegistry& ingestTaps() noexcept {
    return *ingest_taps_;
  }

  /// Shared ownership for runtime hosts that may outlive their construction scope.
  [[nodiscard]] std::shared_ptr<ObjectIngestTapRegistry> ingestTapsShared() const noexcept {
    return ingest_taps_;
  }
  // Plot markers live in the ObjectStore as PlotMarkers object topics (MarkerTopics.h). Call after pushing or clearing
  // one to repaint the plot overlays.
  void notifyMarkersChanged() {
    emit markersChanged();
  }
  /// Announces object topics registered or retired outside an ingest (a layout restore's on-demand step, the Custom
  /// Topics delete); the catalog relists on it.
  void notifyObjectTopicsChanged() {
    emit objectTopicsChanged();
  }
  // Ends the previews among the keys still present and still ephemeral, repainting the overlays once if a marker
  // preview went and announcing `objectTopicsChanged` once if on-demand topics retired. `DerivedRuntimeHosts` passes
  // the previews its bridges created when their plugin instance ends; persistent recipes are the user's data.
  void removeEphemerals(
      const std::unordered_set<std::string>& transform_keys, const std::unordered_set<std::string>& generator_ids,
      const std::unordered_set<std::string>& on_demand_keys);
  // A dataset was just listed by the catalog: marks every all-datasets marker rule stale, since the newcomer may
  // contribute. Wired by CatalogModel; the commit recompute runs before the catalog rebuild, so it never sees it.
  void publishSharedMarkersToNewDatasets();

  // The one session-wide byte budget for ingest-seeded object payloads; every dataset competes for it.
  [[nodiscard]] const std::shared_ptr<ResidentPayloadPool>& residentPayloadPool() const noexcept {
    return resident_payload_pool_;
  }

  // Remembers each curve's color across plots. AppSession clears it when the catalog empties.
  [[nodiscard]] CurveColorRegistry& curveColorRegistry() noexcept {
    return curve_color_registry_;
  }

  // Filters and transforms, as eager IncrementalEngine nodes over this session's DataEngine.
  [[nodiscard]] DataProcessorService& dataProcessorService() noexcept {
    return *processor_service_;
  }

  // Marker rules, published to the ObjectStore. AppSession injects its series resolver once the catalog exists.
  [[nodiscard]] BatchEngine& batchEngine() noexcept {
    return *batch_engine_;
  }

  // The on-demand engine. The dataset life cycle (reload, rollback, merge, removal, eviction) suspends and re-binds
  // its recipes here.
  [[nodiscard]] OnDemandService& onDemandService() noexcept {
    return *on_demand_;
  }

  /// The DataEngine topics the host derives (processor outputs and on-demand series), only the previews' with
  /// `ephemeral_only`: a reload does not treat them as the file's, nor an undo snapshot as raw data.
  [[nodiscard]] std::unordered_set<TopicId> derivedScalarTopics(bool ephemeral_only) const;
  // The workers the derived-data engines run on; `waitIdle` publishes everything submitted.
  [[nodiscard]] EnginePool& enginePool() noexcept {
    return *engine_pool_;
  }
  [[nodiscard]] const InFlightBudget& inFlightBudget() const noexcept {
    return *in_flight_budget_;
  }

  /// (topic, column) of the scalar series `name` (bare, the catalog's series naming) in a dataset.
  using ScalarSeriesLookup =
      std::function<std::optional<std::pair<TopicId, std::size_t>>(DatasetId dataset, const std::string& name)>;
  /// How on-demand scalar inputs resolve when they do not name a scalar topic (`scalarInput`). AppSession injects
  /// the catalog's lookup; until then no such input resolves. GUI thread.
  void setScalarSeriesLookup(ScalarSeriesLookup lookup);
  /// Where on-demand runs read TF from. The shell owns the service and may destroy it first (held as a QPointer).
  /// Without one, runs see no TF. GUI thread.
  void setTransformService(TransformService* service);
  /// Replaces the clock the TF snapshot brake reads; for tests.
  void setTfClock(std::function<std::chrono::steady_clock::time_point()> clock);
  /// The image decoder on-demand scripts use (pj_app's `SceneImageDecoder`, so the runtime links no codec). A run
  /// keeps the decoder it was submitted with; without one `decode` reports "no image decoder". GUI thread.
  void setImageDecoder(std::shared_ptr<const scripting::IImageDecoder> decoder) {
    image_decoder_ = std::move(decoder);
  }
  /// What `video:frame_at` decodes through (pj_app's `SceneVideoFrameSource`). `suspendOnDemand` retires a dataset's
  /// topics in it before their storage changes. Without one, `frame_at` reports "no video frame source". GUI thread.
  void setVideoFrameSource(std::shared_ptr<scripting::IVideoFrameSource> source) {
    video_source_ = std::move(source);
  }

  [[nodiscard]] DataReader createReader() const;

  /// The clock range a derived topic covers: the envelope of its node's raw inputs' ranges, upstream nodes followed
  /// to their raw leaves, never what was computed. Inverted (`{max, min}`) for a finding and when no input has
  /// data; nullopt when the topic is no on-demand output. GUI thread.
  [[nodiscard]] std::optional<std::pair<Timestamp, Timestamp>> derivedTopicInputRange(ObjectTopicId topic) const;

  /// The normalized full path FileLoader created `dataset_id` from (`normalizedSourcePath`; non-file URIs verbatim).
  /// `DatasetInfo::source_name` is only a display label and cannot tell same-named files apart. Empty `path`
  /// removes the association. GUI thread.
  void setDatasetSourcePath(DatasetId dataset_id, QString path);

  /// Empty for a streaming or test dataset.
  [[nodiscard]] QString datasetSourcePath(DatasetId dataset_id) const;

  /// Attaches or overwrites `dataset_id`'s provenance. GUI thread. Dropped by removeDataset, by a destructive merge
  /// (every contributor) and by a committed in-place refill (the content no longer comes from the recorded source;
  /// FileLoader's promotion listener re-attaches).
  void attachSourceRecord(DatasetId dataset_id, SourceRecord record);

  /// nullptr when none. Invalidated by the next attach, detach, removeDataset or merge: copy it out.
  [[nodiscard]] const SourceRecord* sourceRecord(DatasetId dataset_id) const;

  void detachSourceRecord(DatasetId dataset_id);

  /// Ties an opaque resource to `dataset_id`'s lifetime: released on removeDataset and when a destructive merge
  /// consumes the dataset. The source cache holds a read pin this way, so the artifact a cache-hit dataset reads is
  /// not evicted. GUI thread; resources accumulate.
  void pinDatasetResource(DatasetId dataset_id, std::shared_ptr<void> resource);

  /// Stores `dataset_id`'s loader-metadata document, replacing the previous one; an empty or `{}` document clears.
  /// GUI thread. Same lifecycle as the source record (a committed refill clears it). A refusal
  /// (validateDatasetMetadata) keeps the previous document and returns the reason; it never affects the ingest.
  Status setDatasetMetadata(DatasetId dataset_id, const QString& json, QString plugin_id);

  /// nullptr when none. Invalidated by the next set, clear, removeDataset or merge: copy it out.
  [[nodiscard]] const DatasetMetadata* datasetMetadata(DatasetId dataset_id) const;

  void clearDatasetMetadata(DatasetId dataset_id);

  /// A JSON object, at most 1 MiB serialized, nesting depth 32, 16384 values in total. Rejects, never truncates.
  [[nodiscard]] static Status validateDatasetMetadata(const QString& json);

  /// The normalization every stored source path goes through: canonicalFilePath when the file exists, else its
  /// canonical folder plus the file name, else the cleaned absolute path. On Windows even an absolute Unix-style
  /// input gains a drive prefix, so compare against this form.
  [[nodiscard]] static QString normalizedSourcePath(const QString& path);

  /// Resolves a persisted identity without guessing:
  ///  1. `saved_id`, trusted only while every supplied qualifier (source label, full path) still agrees, so a
  ///     same-session undo keeps its exact id amid duplicates;
  ///  2. exactly one dataset whose registered path (and source label) matches;
  ///  3. without a path, exactly one source-label match.
  /// Several candidates at 2 or 3 return `{nullopt, ambiguous=true}`. An id with neither source nor path is valid
  /// only while that exact id exists; otherwise it resolves to nothing (not ambiguous).
  [[nodiscard]] DatasetIdentityResolution resolveDatasetIdentity(
      DatasetId saved_id, const QString& saved_source, const QString& saved_path = {}) const;

  /// Adds the persisted SourceRecord as a qualifier and a tier between the id and the path: a unique attached record
  /// whose (provider_id, source_identity) matches AND whose descriptor_json is byte-equal (a matching identity with
  /// other descriptor bytes falls through). Several confirmed matches are ambiguous. An empty `saved_record` makes
  /// this the 3-argument form.
  [[nodiscard]] DatasetIdentityResolution resolveDatasetIdentity(
      DatasetId saved_id, const QString& saved_source, const QString& saved_path,
      const SourceRecord& saved_record) const;

  /// For a same-file single<->fan-out remint: when the path names several datasets, returns the one sibling owning
  /// an object topic named `object_topic_name`; still ambiguous if several do.
  [[nodiscard]] DatasetIdentityResolution resolveObjectDatasetIdentity(
      DatasetId saved_id, const QString& saved_source, const QString& saved_path,
      const QString& object_topic_name) const;

  /// The total display shift: the per-source alignment plus the global "Use time offset" reference, so
  /// display_time = raw_time - sourceDisplayOffset - globalTimeReference(). Summed on read, so toggling the global
  /// frame never disturbs the per-source alignment the Source Timeline owns.
  [[nodiscard]] DisplayOffset displayOffset(DatasetId dataset_id) const;

  /// nullopt when `dataset_id` is not loaded (a stale or invalid handle).
  [[nodiscard]] std::optional<DisplaySeconds> displayTimeForSource(DatasetId dataset_id, int64_t absolute_ns) const;

  /// The per-source alignment only (the dataset's TimeDomain offset, what the Source Timeline edits and positions its
  /// bars by). Use it wherever positions must not follow the global "Use time offset" toggle.
  [[nodiscard]] DisplayOffset sourceDisplayOffset(DatasetId dataset_id) const;

  /// The one origin (raw ns) subtracted from every displayed time when "Use time offset" is on: the earliest raw
  /// sample across loaded datasets (static reference geometry excluded), applied uniformly so cross-dataset gaps are
  /// kept; 0 when off. Memoized, invalidated on commit and ingest. The Source Timeline controller uses it to bridge
  /// the playback frame (raw - source - global) and the Timeline frame (raw - source).
  [[nodiscard]] Timestamp globalTimeReference() const;

  /// A dataset whose point clouds/meshes and scalar summaries all sit at timestamp zero: a static reference, outside
  /// the playback clock and the global origin.
  [[nodiscard]] bool isStaticReferenceDataset(DatasetId dataset_id) const;

  /// Writes a source's alignment shift (display_time = raw_time - offset) and emits displayOffsetChanged(DatasetId);
  /// unchanged offsets emit nothing. A warning and no-op for an unknown dataset or one bound to the default (id 0)
  /// time domain.
  void setDisplayOffset(DatasetId dataset_id, DisplayOffset offset);

  /// Where the plots showing `dataset_id`'s on-demand series start their view (display axis): a series computes from
  /// there to the end first, then backfills (`SeriesRunner::setVisibleStart`).
  void setSeriesVisibleStart(DatasetId dataset_id, DisplaySeconds start);

  /// One dataset's scalar and object bounds, in display seconds; nullopt when it holds no data. Every raw-to-display
  /// range conversion for playback goes through here, so no raw range reaches the playback axis offset-blind.
  [[nodiscard]] std::optional<DisplayRange> datasetDisplayRange(DatasetId dataset_id) const;

  /// True when any of the dataset's topics, scalar or object (off-clock ones included), holds a row or an entry. A
  /// file load that ends without any is a failed load (FileLoader); a streaming dataset starts empty and is never
  /// asked.
  [[nodiscard]] bool datasetHoldsData(DatasetId dataset_id) const;

  // "Use time offset" switches the displayed frame between absolute epoch seconds (off) and seconds since
  // globalTimeReference() (on). Only the boolean is state.

  /// Default off; the shell applies the user's preference.
  [[nodiscard]] bool useTimeOffset() const noexcept {
    return use_time_offset_;
  }

  /// Emits the no-arg displayOffsetChanged() on a change. Touches no per-source offset, so a Timeline alignment
  /// survives a toggle.
  void setUseTimeOffset(bool use);

  [[nodiscard]] std::vector<TopicId> commitChunks(std::vector<std::pair<TopicId, TopicChunk>> chunks);

  // Announces data written straight to the DataEngine (file and stream ingest bypass commitChunks); without it plot
  // adapter caches never invalidate. `live` reaches samplesIngested: follow-live consumers act only on it. A live
  // announcement first requests the on-demand recipes of its datasets at their live edge: the datasets of `ids`,
  // and `dataset`, so a tick of object topics only (no scalar ids) still reaches them.
  void notifyIngest(QVector<PJ::TopicId> ids, bool live = false, std::optional<DatasetId> dataset = std::nullopt);
  /// `dataset_id`'s stream ended: its on-demand series compute the tail past their live edge (`SeriesRunner::endLive`).
  void notifyStreamEnded(DatasetId dataset_id);
  /// A live announcement within the TF brake period (`kTfSnapshotPeriod` on `setTfClock`'s clock).
  [[nodiscard]] bool isLive(DatasetId dataset_id) const;

  // Progressive import lifecycle: which datasets are receiving a progressive import, for any observer; the
  // presentation stays in pj_app. GUI thread (ToolboxRuntimeHost marshals its callbacks here).

  /// Starts a progressive import and returns its token. A begin on a dataset with a live ingest supersedes it at
  /// once: the old run is reported as ingestEnded(old, kCancelled) before the new ingestBegan, and the old token goes
  /// stale. Triggers one structural catalog publication, so an object-only dataset gets its row at once.
  ///
  /// `cancellable` false refuses every requestCancel (no stop affordance). `on_cancel` runs only when requestCancel
  /// succeeds. A `kStopOnly` ingest refuses requestCancel(keep_partial=false).
  ///
  /// The supersede terminal is emitted synchronously, so an observer may begin another ingest on this dataset from
  /// inside it. That successor supersedes this call before it announced itself: the `ingestBegan` for the returned
  /// token is suppressed (a began never follows the began of its replacement) and the returned token is stale.
  [[nodiscard]] IngestToken beginIngest(
      DatasetId dataset_id, QString label, quint64 total, bool cancellable, IngestCancelFn on_cancel,
      IngestStop stop = IngestStop::kKeepOrDiscard);

  /// Stale tokens are ignored. Publishes the dataset's flushed topics through notifyIngest first, so data appears
  /// before the progress number announcing it.
  void updateIngest(IngestToken token, quint64 current, quint64 total);

  /// Stale tokens are ignored. Reports kCancelled whatever `outcome` is when a cancel was accepted.
  void endIngest(IngestToken token, IngestOutcome outcome);

  /// True when the ingest is stopping. False when refused: unknown or stale token, already ended, not cancellable,
  /// or a discard asked of a kStopOnly producer. The first accepted request runs the handler synchronously.
  ///
  /// The first intent wins: a request on an ingest already stopping returns true without re-running the handler
  /// (also from inside ingestStopping, which would otherwise recurse), so a second click with the opposite
  /// keep_partial cannot override the choice the producer is acting on.
  bool requestCancel(IngestToken token, bool keep_partial);

  [[nodiscard]] bool hasActiveIngests() const noexcept {
    return !active_ingests_.empty();
  }

  /// One live ingest as a display consumer sees it. Has no cancel handler: cancel through requestCancel(token), so
  /// the intent is recorded.
  struct ActiveIngest {
    IngestToken token;
    QString label;
    quint64 current = 0;
    quint64 total = 0;  // 0 = size unknown
    bool cancellable = false;
    bool discardable = true;  // false: stopping always keeps
  };

  /// Live ingests by dataset, at most one each. A snapshot per call: re-query instead of holding it across an
  /// event-loop turn.
  [[nodiscard]] std::unordered_map<DatasetId, ActiveIngest> activeIngests() const;

  [[nodiscard]] bool ingestActive(DatasetId dataset_id) const;

  // In-place reload swap: replaces `primary_id`'s scalar and object data with what was staged under `staged_id`,
  // keeping the primary DatasetId, TopicIds and ObjectTopicIds (every curve key and dock binding stays valid). Order:
  // datasetAboutToBeReplaced, the engine and store swaps, staged object parsers re-registered under the primary ids,
  // notifyIngest. Stage into a throwaway engine and store, and run no event loop between staging, this call and the
  // catalog rebuild that must follow it: a repaint in between would read freed chunks.
  void replaceDataset(
      DataEngine& staged_engine, ObjectStore& staged_store, DatasetId staged_id, DatasetId primary_id,
      std::vector<std::pair<ObjectTopicId, std::unique_ptr<MessageParserHandle>>> staged_object_parsers);

  // Begins a transactional in-place refill of `dataset_id` that keeps its ids, so bound plot adapters stay valid and
  // the refill writes back into the same ids. On return, on the GUI thread with no event loop, it has:
  //   1) emitted datasetAboutToBeReplaced (adapters drop their caches of the old values),
  //   2) detached (moved aside, not freed) the scalar chunks and object entries, sharing each prior object topic's
  //      parser slot,
  //   3) notifyIngest(<the dataset's topic ids>, live=false), so the UI shows it empty.
  // 1 before 2, or an adapter keeps serving the detached data. The caller commit()s the guard on success (or to keep a
  // partial load); otherwise its destructor rolls back to the exact prior data.
  [[nodiscard]] RefillGuard beginRefill(PJ::DatasetId dataset_id);
  /// True from beginRefill until the dataset's guard commits or rolls back. Meanwhile the dataset's earliest-sample pin
  /// is held (a static reference's is dropped instead), so emptying it does not move the time origin ("Use time
  /// offset"); the close re-pins from the data left.
  [[nodiscard]] bool refillOpen(DatasetId dataset_id) const;

  /// Marks stale the marker rules whose inputs are among `changed` (file commit, streaming tick); the batch engine
  /// re-runs them on the pool behind its brake. A topic whose stamp (newest timestamp, row count, retention floor,
  /// series generation) has not moved since the last call is dropped, since a streaming tick names every topic of
  /// its dataset. `scope` is the dataset whose data moved, bounding an `all_datasets` rule to it; 0 only when the
  /// change belongs to no single dataset.
  void recomputeMarkersForChanged(const std::vector<TopicId>& changed, DatasetId scope = 0);

  // Folds `sources` (each with a raw timestamp shift) into `anchor`. Order: datasetAboutToBeReplaced for the anchor
  // and every source, engine merge, object-store merge, notifyIngest of the anchor's changed topics. nullopt when the
  // engine rejected the merge (the store is untouched, so the caller must not remove the sources), unlike a merge
  // that folds no topics (an empty report). The caller picks the anchor and shifts and, on success only, updates the
  // catalog. Runs no event loop; rebuild the catalog after it returns.
  std::optional<DatasetMergeReport> mergeDatasets(DatasetId anchor, const std::vector<DatasetMergeSource>& sources);

  // Registers or replaces one object topic's parser; called from streaming worker threads. A replaced slot is
  // destroyed after object_parsers_mutex_ is released (its destructor may run plugin teardown and dlclose), and a
  // ParserBinding taken earlier keeps the old parser alive for its holder.
  void registerObjectTopicParser(ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser);
  struct ParserBinding {
    MessageParserPluginBase* parser = nullptr;
    std::shared_ptr<std::mutex> mutex;
    std::shared_ptr<void> keepalive;

    [[nodiscard]] explicit operator bool() const noexcept {
      return parser != nullptr && keepalive != nullptr;
    }
  };

  /// The parser, its per-topic mutex and its DSO keepalive, copied out under object_parsers_mutex_ (shared). Empty
  /// when no valid parser is registered. Take a binding per use and hold its keepalive across parseObject; a cached
  /// raw `parser` pointer dangles once the slot is replaced.
  [[nodiscard]] ParserBinding parserBindingForObjectTopic(ObjectTopicId id) const;
  [[nodiscard]] MessageParserPluginBase* parserForObjectTopic(ObjectTopicId id) const;

  // MessageParser plugins are not thread-safe (stateful scratch), so every caller of parseObject on this topic's
  // parser holds this mutex. nullptr when no parser is registered.
  [[nodiscard]] std::shared_ptr<std::mutex> parserMutexForObjectTopic(ObjectTopicId id) const;

  // Holding it keeps the parser instance and its plugin DSO mapped. A decode worker calling parseObject must hold
  // it: at shutdown the extension catalog and the DSO can go before the worker is joined. nullptr when no parser
  // is registered.
  [[nodiscard]] std::shared_ptr<void> parserKeepaliveForObjectTopic(ObjectTopicId id) const;

  struct LoadedSource {
    QString path;
    QString prefix;
    QString plugin_id;           // empty when the loader recorded no plugin
    QString plugin_config_json;  // the plugin's saveConfig() JSON at load time
    // The plugin's manifest "id", persisted in layouts beside the display name; empty when not recorded.
    QString plugin_manifest_id;
  };

  // Every data file loaded, in load order, deduped by normalized path; a layout saves one <fileInfo> per entry.
  [[nodiscard]] const std::vector<LoadedSource>& loadedSources() const noexcept {
    return loaded_sources_;
  }
  [[nodiscard]] std::optional<LoadedSource> lastLoadedSource() const noexcept {
    if (loaded_sources_.empty()) {
      return std::nullopt;
    }
    return loaded_sources_.back();
  }
  // `path` is normalized (normalizedSourcePath): an alias of a tracked file updates its entry in place, keeping its
  // position; a new file is appended.
  void recordLoadedSource(
      QString path, QString prefix, QString plugin_id = {}, QString plugin_config_json = {},
      QString plugin_manifest_id = {});
  void clearLoadedSource() noexcept {
    loaded_sources_.clear();
  }

  // Dataset removal keeps DataEngine scalars (a catalog tombstone) but frees ObjectStore objects.
  void evictDatasetObjects(DatasetId dataset_id);
  void evictObjectTopics(const std::vector<ObjectTopicId>& topic_ids);
  void clearAllObjects();

  // Removes the dataset's scalar storage from the engine and invalidates its time-origin caches, so
  // globalTimeReference() re-bases to the survivors. Use it, not dataEngine().removeDataset(), which leaves the
  // memoized origin stale. Tear down readers and catalog items and evict the dataset's objects first.
  //
  // With `remove_dependent_processors`, every filter and transform reading the dataset goes first, cascading, without
  // asking; a shell that wants the user's consent asks before calling, counting datasetTopicIds(). True when that
  // removed one: an output can live in a surviving dataset, so the caller rebuilds the catalog. Without it the
  // processors stay bound to the erased topics, for a caller that restores them right after (a fan-out reload).
  bool removeDataset(DatasetId dataset_id, bool remove_dependent_processors = true);

  // Every topic removeDataset() erases, retired ones included: a merge or a reload retires topics a processor may
  // still read, and listTopics() hides them. Empty for an unknown dataset.
  [[nodiscard]] std::vector<TopicId> datasetTopicIds(DatasetId dataset_id) const;

  // Recomputes `dataset_id`'s earliest-sample pin from its current data and emits the global reframe if the origin
  // moved. FileLoader calls it when a load completes: plugin ingest commits through the C-ABI write host, bypassing
  // commitChunks/notifyIngest, so an earlier file would otherwise leave adapters on a stale offset. No-op when
  // "Use time offset" is off.
  void refreshDatasetTimeReference(DatasetId dataset_id);

 signals:
  // New samples on `ids`. `live` is true only for follow-live writers (streaming).
  void samplesIngested(QVector<PJ::TopicId> ids, bool live);

  // Emitted by every notifyIngest, before samplesIngested and also when that is suppressed (no scalar ids, not
  // live): object entries may have landed with no scalar topic to name them. It fires before a live ingest is
  // stamped live (isLive) and before the on-demand recipes follow the live edge: a listener needing either defers.
  void dataPublished();

  // Emitted before a reload swaps a dataset's chunks in place. Bound adapters drop cached chunk pointers
  // synchronously; the DatasetId and TopicIds stay valid.
  void datasetAboutToBeReplaced(PJ::DatasetId dataset_id);

  // A refill of the dataset begins: emitted before datasetAboutToBeReplaced, while the dataset still holds its prior
  // data and before refillOpen turns true. A listener reads now what it must keep for the refill's duration.
  void refillStarting(PJ::DatasetId dataset_id);
  // The dataset's refill committed or rolled back, after the time origin was re-pinned from the data left. The
  // playback range is recomputed on it: a rollback that restores only object topics notifies no ingest.
  void refillClosed(PJ::DatasetId dataset_id);

  // Plot markers changed. No payload (an all-datasets rule touches every dataset): listeners re-derive their rows
  // through publishedMarkerScopes (MarkerTopics.h).
  void markersChanged();

  // The global display frame changed (toggle, or a load moved the earliest sample). No topic changed, so plots drop
  // every curve adapter's cached offset and replot. Connect with qOverload<>(...).
  void displayOffsetChanged();

  // One dataset's display offset changed (setDisplayOffset); no data moved. Connect with qOverload<PJ::DatasetId>.
  void displayOffsetChanged(PJ::DatasetId dataset_id);

  // GUI thread. ingestProgressed fires after the tick's samplesIngested, so data precedes the number announcing it.
  void ingestBegan(PJ::IngestToken token, QString label, quint64 total, bool cancellable, bool discardable);
  /// A stop was accepted; emitted before the producer's handler runs, so the UI acknowledges the click before a
  /// cooperative stop completes. The terminal still arrives as ingestEnded.
  void ingestStopping(PJ::IngestToken token, bool keep_partial);
  void ingestProgressed(PJ::IngestToken token, quint64 current, quint64 total);
  void ingestEnded(PJ::IngestToken token, PJ::IngestOutcome outcome);

  // The on-demand engine's reports, on the GUI thread; `topics` are the node's output topics.
  /// The ring took a result of `key`: what its topics show may have changed.
  void derivedEntriesPublished(QString key, std::vector<PJ::ObjectTopicId> topics);
  /// The node's state (status, detail, lag) changed.
  void derivedStateChanged(QString key, std::vector<PJ::ObjectTopicId> topics);
  /// A new revision of the recipe was installed (a create, an edit, a merge re-binding it).
  void onDemandRecipeInstalled(QString key);
  /// `key`'s series state (`SeriesRunner::state`) changed, or the series is gone.
  void seriesStateChanged(QString key);
  /// Object topics were retired (an on-demand install or removal): the catalog re-lists them.
  void objectTopicsChanged();
  /// The source path registered for `dataset_id` changed (setDatasetSourcePath). Nothing else announces it, and
  /// path-comparing identity resolution (a saved object wire) gives a new answer: an asynchronous load registers the
  /// path only when it commits.
  void datasetSourcePathChanged(PJ::DatasetId dataset_id);

 private:
  friend class RefillGuard;
  // A Qt signal is emitted only from the object's own members.
  void notifyDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);
  // RefillGuard's steps, in order. announceRefill, before the detach, pins the dataset's earliest sample (drops a
  // static reference's) and emits refillStarting and datasetAboutToBeReplaced. openRefill, after every step that can
  // throw, counts the guard, so a constructor that throws leaves no refill open. closeRefill, when the dataset's last
  // guard ends, re-pins the origin and emits refillClosed.
  void announceRefill(PJ::DatasetId dataset_id);
  void openRefill(PJ::DatasetId dataset_id);
  void closeRefill(PJ::DatasetId dataset_id);

  // Datasets owning `topic_ids` (dataset 0 skipped). Take it before removing the topics, whose descriptors are needed.
  [[nodiscard]] std::unordered_set<DatasetId> datasetsOwningObjectTopics(
      const std::vector<ObjectTopicId>& topic_ids) const;

  [[nodiscard]] std::optional<std::pair<Timestamp, Timestamp>> datasetRawBounds(DatasetId dataset_id) const;

  // Earliest raw timestamp of one dataset (0 when empty), pinned in dataset_min_cache_. Moves earlier with older
  // data, never later because retention evicted rows.
  [[nodiscard]] Timestamp datasetMinTimestamp(DatasetId dataset_id) const;
  [[nodiscard]] Timestamp datasetDomainDisplayOffset(DatasetId dataset_id) const;
  [[nodiscard]] Timestamp rememberDatasetMinTimestamp(DatasetId dataset_id, Timestamp observed_min) const;
  /// Returns the datasets of `ids`.
  std::unordered_set<DatasetId> refreshDatasetMinTimestampsForTopics(const QVector<TopicId>& ids) const;
  // Keeps the pin of a dataset whose refill is open (closeRefill re-pins it), resetting only the global memo.
  void invalidateDatasetMinTimestamp(DatasetId dataset_id) const;
  // Emits the no-arg displayOffsetChanged exactly when the global reference moved since the last emission.
  void notifyGlobalTimeReferenceIfChanged();

  struct ObjectParserSlot {
    // shared_ptrs so a decode worker or an in-flight parse keeps the parser, its DSO and its lock alive past topic
    // removal and app teardown.
    std::shared_ptr<MessageParserHandle> handle;
    std::shared_ptr<std::mutex> mutex;
  };

  // The slot for `id` iff it holds a live handle. The caller holds object_parsers_mutex_; the pointer is valid only
  // while it does.
  [[nodiscard]] const ObjectParserSlot* findValidParserSlotLocked(ObjectTopicId id) const;

  // An empty `slot` means no parser was registered; restoring it erases, or a rollback would leave a binding the
  // restored bytes were never written under.
  struct ObjectParserSnapshotEntry {
    uint32_t topic_id = 0;
    ObjectParserSlot slot;
  };

  // Shares the slots registered for `topic_ids`, so a pending restore keeps parser and DSO alive after the map entry
  // is replaced. Takes object_parsers_mutex_ shared.
  [[nodiscard]] std::vector<ObjectParserSnapshotEntry> snapshotObjectTopicParsers(
      const std::vector<ObjectTopicId>& topic_ids) const;

  // Reinstalls a snapshot. Displaced slots are destroyed after the lock is released (dlclose under
  // object_parsers_mutex_ would run plugin teardown under it).
  void restoreObjectTopicParsers(std::vector<ObjectParserSnapshotEntry> snapshot);

  struct ActiveIngestEntry {
    // Any other id names a superseded run.
    IngestId id = 0;
    QString label;
    quint64 current = 0;
    quint64 total = 0;  // 0 = size unknown
    bool cancellable = false;
    bool discardable = true;
    bool cancel_requested = false;
    // Wall-clock ms of the stop request, or 0; the pj.runtime.ingest.cancel diagnostic reports the delay until the
    // terminal (a cooperative stop is seen only at the producer's next chunk boundary).
    qint64 cancel_requested_ms = 0;
    IngestCancelFn on_cancel;
  };

  // The staleness rule: null when `token` is 0 or not the dataset's current id. updateIngest, endIngest and
  // requestCancel all ask here.
  [[nodiscard]] ActiveIngestEntry* liveEntry(IngestToken token);

  /// Before a meaning-changing change of the dataset's storage: a new epoch (a run in flight is never published),
  /// the on-demand nodes suspended (results dropped, runs cancelled, requests held), the dataset retired in the video
  /// source. Pair each with `resumeOnDemandLater`.
  void suspendOnDemand(DatasetId dataset_id);
  /// Ends one `suspendOnDemand` from the event loop, so the requests it serves see the storage, the catalog and
  /// the TF state their caller rebuilds after the change, with no event loop in between.
  void resumeOnDemandLater(DatasetId dataset_id);
  /// Re-installs the merged datasets' on-demand recipes (`shifts`: each dataset's raw shift, the anchor's 0),
  /// producers first, from one snapshot, so they bind on `anchor`; a finding's pinned time and a requested node's
  /// cursor move by the shift. One that no longer resolves is removed, with a warning.
  void rebindOnDemandAfterMerge(DatasetId anchor, const std::unordered_map<DatasetId, Timestamp>& shifts);
  /// The dataset's TF for an on-demand run. While it ingests (a live ingest, or a live announcement within
  /// `kTfSnapshotPeriod`: a stream never begins an ingest) the held snapshot, at most `kTfSnapshotPeriod` old, so TF
  /// writes do not copy the growing state per request. Null without a TransformService.
  [[nodiscard]] std::shared_ptr<const pj::tf::TransformSnapshot> onDemandTf(DatasetId dataset_id);
  /// The revision `onDemandTf` would answer now, taking no snapshot.
  [[nodiscard]] uint64_t onDemandTfRevision(DatasetId dataset_id);
  /// Whether `onDemandTf` brakes the dataset's snapshots now.
  [[nodiscard]] bool tfIngesting(DatasetId dataset_id) const;

  /// (topic, column) of an on-demand scalar input: the scalar topic named `name` (its first column), else the series
  /// the injected lookup names (`topic/field`). nullopt before the lookup is set.
  [[nodiscard]] std::optional<std::pair<TopicId, std::size_t>> scalarInput(
      DatasetId dataset, const std::string& name) const;

  DataEngine data_engine_;
  ObjectStore object_store_;
  std::shared_ptr<ObjectIngestTapRegistry> ingest_taps_;
  std::shared_ptr<ResidentPayloadPool> resident_payload_pool_;
  CurveColorRegistry curve_color_registry_;
  bool use_time_offset_ = false;
  // Per-dataset earliest-stamp pin for displayOffset(). Ingest only lowers it; replacement invalidates it.
  mutable std::unordered_map<DatasetId, Timestamp> dataset_min_cache_;
  // Earliest raw stamp across all datasets, for globalTimeReference(); nullopt = not computed or no data yet.
  // Independent of the toggle; reset on every commit and ingest.
  // The recompute trusts the per-dataset pins instead of rescanning, so every path that can raise a dataset's
  // earliest sample (removeDataset, evictObjectTopics, clearAllObjects, a refill's close, refreshDatasetTimeReference)
  // calls invalidateDatasetMinTimestamp, which drops the pin and this memo. Ingest only lowers pins
  // (rememberDatasetMinTimestamp never raises), so it only resets the memo. A missed reset leaves a stale-low origin.
  mutable std::optional<Timestamp> global_min_cache_;
  // globalTimeReference() at the last displayOffsetChanged(), to suppress a redundant one.
  Timestamp last_notified_global_reference_ = 0;
  // Built in the constructor body, after data_engine_ (its IncrementalEngine binds to it).
  std::unique_ptr<DataProcessorService> processor_service_;
  // Declared before the pool so it outlives it: a run still on the pool holds its reservation.
  std::unique_ptr<InFlightBudget> in_flight_budget_;
  // Declared before the engines so it is destroyed after them: each engine cancels its runs, then the pool joins.
  std::unique_ptr<EnginePool> engine_pool_;
  // After the pool and the budget: destroyed first, its scheduler cancelling the runs the pool then joins.
  std::unique_ptr<OnDemandService> on_demand_;
  /// On the display axis: an offset change moves the raw instant the series receive.
  std::map<DatasetId, DisplaySeconds> series_visible_start_;
  void pushSeriesVisibleStarts();
  // Moved by suspendOnDemand; a result staged under another epoch is never current.
  std::unordered_map<DatasetId, uint64_t> epochs_;
  ScalarSeriesLookup scalar_lookup_;
  QPointer<TransformService> transform_service_;
  std::shared_ptr<const scripting::IImageDecoder> image_decoder_;
  std::shared_ptr<scripting::IVideoFrameSource> video_source_;
  // The brake: how long one TF snapshot serves on-demand runs while its dataset ingests.
  static constexpr std::chrono::milliseconds kTfSnapshotPeriod{500};
  std::function<std::chrono::steady_clock::time_point()> tf_clock_ = [] { return std::chrono::steady_clock::now(); };
  struct HeldTf {
    std::shared_ptr<const pj::tf::TransformSnapshot> snapshot;
    std::chrono::steady_clock::time_point taken;
  };
  std::unordered_map<DatasetId, HeldTf> held_tf_;
  std::unordered_map<DatasetId, std::chrono::steady_clock::time_point> live_at_;
  // Built after object_store_ and data_engine_.
  std::unique_ptr<BatchEngine> batch_engine_;
  // What recomputeMarkersForChanged last saw of each topic.
  struct MarkerInputStamp {
    Timestamp time_range_max = 0;
    uint64_t total_row_count = 0;
    Timestamp retention_floor = 0;
    uint64_t series_generation = 0;
    bool operator==(const MarkerInputStamp&) const = default;
  };
  std::unordered_map<TopicId, MarkerInputStamp> marker_input_stamps_;
  // Written from streaming worker threads (a plugin discovers or replaces a topic mid-stream) and read on the GUI
  // thread every render tick: guarded by object_parsers_mutex_ (shared in the parser* accessors, exclusive in
  // register, evict and clear). A replaced slot is destroyed outside the lock (its destructor may dlclose).
  mutable std::shared_mutex object_parsers_mutex_;
  std::unordered_map<uint32_t, ObjectParserSlot> object_topic_parsers_;
  // The portable identity FileLoader registers; plots, processors and both scene families resolve against it.
  std::unordered_map<DatasetId, QString> dataset_source_paths_;
  // These three registries and dataset_resources_ share one lifecycle: erased on removeDataset, invalidated for
  // every contributor of a destructive merge (anchor included).
  std::unordered_map<DatasetId, SourceRecord> dataset_source_records_;
  std::unordered_map<DatasetId, DatasetMetadata> dataset_metadata_;
  std::unordered_map<DatasetId, std::vector<std::shared_ptr<void>>> dataset_resources_;
  // At most one live ingest per dataset, carrying the id of the token that owns it; a call from a superseded run is a
  // lookup miss.
  std::unordered_map<DatasetId, ActiveIngestEntry> active_ingests_;
  // Open RefillGuards per dataset: a replacing guard is built before the one it replaces rolls back.
  std::unordered_map<DatasetId, int> open_refills_;
  // Never 0.
  IngestId next_ingest_id_ = 1;
  std::vector<LoadedSource> loaded_sources_;
};

/// The transaction of a replacing reload (SessionManager::beginRefill). Construction emits datasetAboutToBeReplaced,
/// then detaches the dataset's prior data into a snapshot, leaving every TopicId/ObjectTopicId registered and empty
/// so a progressive refill writes back into the same ids. Destroyed without commit() it rolls back: the exact prior
/// scalar and object data, and the parser each prior object topic was bound to (only for topics the dataset still
/// owns: a topic removed mid-refill stays unbound, its raw id may be reused); topics the refill added are retired or
/// removed with their parsers, and the UI is re-notified. The parsers matter: a replacement load's parser lands in the
/// same slot, so without them a rolled-back topic would decode prior bytes with the replacement's schema.
///
/// GUI thread; no event loop between construction and commit/rollback. Does not touch the dataset's TF buffer:
/// FileLoader owns the TransformService and rebuilds TF after a rollback from the restored ObjectStore.
class RefillGuard {
 public:
  RefillGuard(SessionManager& session, DatasetId dataset_id);
  ~RefillGuard();
  RefillGuard(RefillGuard&&) noexcept;
  RefillGuard& operator=(RefillGuard&&) noexcept;
  RefillGuard(const RefillGuard&) = delete;
  RefillGuard& operator=(const RefillGuard&) = delete;

  /// Keeps the refilled data; the destructor then does nothing.
  void commit();
  /// Replays processor outputs after the final raw flush and before pruneVanishedTopics, then re-runs the dataset's
  /// marker rules once (the replay does not fire the outputs-changed sink). On failure the transaction stays live for
  /// the destructor's rollback.
  [[nodiscard]] Status recomputeProcessors();
  /// Retires prior topics the reloaded file no longer has: a prior topic still empty after the refill (every prior
  /// topic was emptied; the refill writes back only those present), including one declared with zero rows. In a
  /// refilled topic it retires the columns left without a value (TopicStorage::retireValuelessColumns). Call only
  /// after a complete refill (on a partial load an unreached topic has not vanished) and before commit(), which frees
  /// the snapshots this reads.
  void pruneVanishedTopics();
  /// Bytes held aside (scalar approximate, object exact); 0 when nothing was detached or after commit().
  [[nodiscard]] std::size_t snapshotBytes() const noexcept;

 private:
  void rollback();

  SessionManager* session_ = nullptr;  // null after a move, so the moved-from destructor does nothing
  DatasetId dataset_id_ = 0;
  DataEngine::DatasetChunkSnapshot scalar_snapshot_;
  ObjectStore::ObjectDatasetSnapshot object_snapshot_;
  std::vector<ObjectTopicId> prior_object_topic_ids_;
  // Keeps the parser of each prior object topic alive, so a replacement re-registering the id cannot destroy one this
  // transaction may reinstate. Dropped on commit().
  std::vector<SessionManager::ObjectParserSnapshotEntry> object_parser_snapshot_;
  std::vector<TopicId> replaced_source_topic_ids_;
  std::unordered_set<TopicId> processor_output_topic_ids_;
  bool committed_ = false;
};

}  // namespace PJ

// Registered so the ingest signals cross queued connections and QVariant (QSignalSpy).
Q_DECLARE_METATYPE(PJ::IngestToken)
Q_DECLARE_METATYPE(PJ::IngestOutcome)
