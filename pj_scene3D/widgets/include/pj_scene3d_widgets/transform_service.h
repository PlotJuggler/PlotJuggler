#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "pj_base/types.hpp"
#include "pj_datastore/sequential_uid.hpp"
#include "pj_runtime/ObjectIngestTap.h"

namespace PJ {
class SessionManager;
}  // namespace PJ

namespace pj::scene3d {

class TransformBuffer;

// Owns the per-dataset 3D transform buffers and the load-time TF ingest.
//
// This deliberately lives in the 3D widget family rather than in
// pj_runtime: SessionManager (the shared, domain-neutral runtime) must not
// depend on 3D-specific types like TransformBuffer. The service reaches the
// data it needs through SessionManager's neutral surface only: ingestTaps(),
// objectStore(), and parserBindingForObjectTopic().
//
// One active TransformBuffer per dataset is shared by every attached 3D dock;
// an in-place reload may also own one private staging generation. Per
// pj_scene3D REQUIREMENTS §9.
//
// THREADING: public methods run on the GUI thread. The registered ingest-tap
// callback is the sole worker-thread entry point; its buffer routing uses a
// private mutex, while TransformBuffer provides its own reader/writer lock.
class TransformService : public QObject {
  Q_OBJECT
 public:
  explicit TransformService(PJ::SessionManager& session, QObject* parent = nullptr);
  ~TransformService() override;

  TransformService(const TransformService&) = delete;
  TransformService& operator=(const TransformService&) = delete;

  /// Claims the FrameTransforms ingest tap. Call once during application wiring,
  /// after construction and before any runtime host is created.
  void activateIngestTap();

  // Returns the dataset's TransformBuffer, lazily creating it on first access.
  // The buffer is thread-safe for concurrent ingest writes + render reads.
  [[nodiscard]] std::shared_ptr<TransformBuffer> transformBuffer(PJ::DatasetId dataset_id);

  // Explicit ObjectStore rebuild used after a destructive dataset merge. With
  // the tap active it clears the active buffer and cursor state, then cold-drains
  // the whole history so payload_stamp_shift is applied. With no active tap it
  // uses the incremental UID cursor path. Always emits datasetTransformsReady.
  //
  // Threading: GUI-thread only, like every TransformService method (see the
  // class comment). Synchronous; blocks the calling thread for the whole TF
  // history.
  void ingestFrameTransformsForDataset(PJ::DatasetId dataset_id);

  /// Drains any store entries the tap did not feed (no-op with the tap active),
  /// then signals docks.
  void publishTransforms(PJ::DatasetId dataset_id);

  // Bound the dataset's TransformBuffer to a rolling cache window so a
  // live-streaming session does not retain every TF sample forever. Trims
  // per-edge to (newest stamp - `window`) but always keeps the last sample of
  // each edge, so static / once-published frames stay resolvable. File loads
  // never call this and keep the buffer's kKeepAll default (see transformBuffer).
  // Reconfiguring is cheap and can be re-issued whenever the retention budget
  // changes; GUI-thread only.
  void setLiveCacheWindow(PJ::DatasetId dataset_id, std::chrono::nanoseconds window);

  // Forgets a dataset's TF state so the next ingest re-reads the store: drops the
  // per-topic ingest cursors and non-TF classifications, and empties the existing
  // TransformBuffer IN PLACE (3D docks share that buffer by pointer, so they see
  // the reset rather than holding a stale orphan). Call when the dataset's object
  // topics no longer hold the data the buffer was built from: dataset removal,
  // eviction, or the explicit merge rebuild.
  void invalidateDataset(PJ::DatasetId dataset_id);

  // invalidateDataset() over every known dataset — the clear-all counterpart,
  // paired with SessionManager::clearAllObjects() at the shell's wipe sites.
  void invalidateAll();

  /// Starts an in-place reload generation without disturbing the active buffer.
  /// A repeated begin discards the unfinished staging generation and starts fresh.
  void beginReplacingLoad(PJ::DatasetId dataset_id);

  /// Publishes the completed reload generation atomically and notifies docks.
  void commitReplacingLoad(PJ::DatasetId dataset_id);

  /// Discards a failed tap generation without touching the prior active buffer.
  /// An inactive-tap service rebuilds from the restored ObjectStore history.
  void abortReplacingLoad(PJ::DatasetId dataset_id);

  // Remember `frame` as the fixed frame the user manually chose for `dataset_id`,
  // so a NEWLY-created 3D dock bound to the same TransformBuffer defaults to it
  // instead of the map/world/odom heuristic (see Scene3DDockWidget). Writes two
  // tiers: an in-session cache keyed by DatasetId (shared instantly by sibling
  // docks on the same dataset) and a cross-restart record in QSettings keyed by
  // the dataset's source path + name (the same identity pair layout restore
  // matches datasets by — name alone would let two same-named files in
  // different folders share a frame). A dataset with no source name (e.g. an
  // unnamed live stream) is remembered for this session only. An empty `frame`
  // is ignored. GUI-thread only.
  void rememberFixedFrame(PJ::DatasetId dataset_id, const QString& frame);

  // The remembered manual fixed frame for `dataset_id`, or empty if none. Checks
  // the in-session cache first, then falls back to the persisted QSettings record
  // (looked up by the dataset's source path + name), memoizing the result — hit OR miss —
  // so the hot onAvailableFrames seeding path does not re-read QSettings on every
  // frame-tree change. Callers MUST still confirm the frame exists in the live TF
  // tree before using it: a remembered frame can be absent from a different
  // recording of the same source. Mutates the memo cache, hence non-const.
  // GUI-thread only.
  [[nodiscard]] QString rememberedFixedFrame(PJ::DatasetId dataset_id);

  // Incremental, UID-keyed ObjectStore ingest for a service with no active tap.
  // With the tap active this returns false immediately because the pushing worker
  // already updated the buffer. GUI-thread only.
  //
  // The bool reports whether THIS call applied any transforms — NOT a reliable
  // "something changed" signal for repaint gating: the per-topic cursor is
  // shared across sibling docks, so a sibling tick may have already advanced it
  // and this call returns false while the buffer did change. Live followers
  // should gate repaint on TransformBuffer::revision() instead. The return is
  // kept because ingestFrameTransformsForDataset still logs it.
  bool ingestNewTransforms(PJ::DatasetId dataset_id);

 signals:
  // Emitted after a progressive publication, completed load generation, or
  // explicit rebuild so docks refresh or rebind the dataset buffer.
  void datasetTransformsReady(PJ::DatasetId dataset_id);

 private:
  // For every TF topic, drain entries since the per-topic UID cursor and ingest
  // them. GUI-thread only. Returns true if any transform was applied.
  bool ingestNewerThanCursor(PJ::DatasetId dataset_id);

  // Worker-thread callback registered by activateIngestTap. Parser bindings are
  // resolved per call, and no payload bytes survive its return.
  void ingestTappedObject(
      PJ::ObjectTopicId topic_id, PJ::DatasetId dataset_id, PJ::Timestamp store_ts, PJ::sdk::PayloadView bytes);

  // Returns the active or staging target for the worker tap, creating a
  // kKeepAll active buffer on first use. Thread-safe internal counterpart of
  // the GUI-affine transformBuffer().
  [[nodiscard]] std::shared_ptr<TransformBuffer> transformBufferForTap(PJ::DatasetId dataset_id);

  // Drops only the drain cursor/classification state for one dataset.
  void resetDrainState(PJ::DatasetId dataset_id);

  // Restores the active buffer from durable ObjectStore entries.
  void rebuildFromStore(PJ::DatasetId dataset_id);

  // The cross-restart QSettings key for `dataset_id`: its SessionManager source
  // path joined with its DataEngine source_name (name alone for a pathless
  // source), each percent-encoded so a path can't be misread as a settings group
  // separator. Empty when the dataset is unknown or has no source name (then the
  // remembered frame is session-only). GUI-thread only.
  [[nodiscard]] QString datasetSourceKey(PJ::DatasetId dataset_id) const;

  // Per-topic ingest cursor: the SequentialUID of the last entry drained into the
  // buffer. A UID is stable across front-eviction and per-topic SPARSE (UID
  // allocation is process-global), so the next tick resumes strictly forward via
  // ObjectStore::drainNewSince(cursor) — never by incrementing the value. This
  // fixes both the index-shift TOCTOU (a concurrent front-eviction can never move a
  // not-yet-ingested entry below the cursor) and the equal-timestamp skip (UIDs are
  // unique even when timestamps tie) that a timestamp cursor suffered.
  // Default-constructed (invalid) UID means "ingest from the first retained
  // entry" — the bulk file-load start. An entry evicted before its UID is
  // reached is unrecoverable by design: the store no longer holds it.
  struct TfCursor {
    PJ::SequentialUID last_ingested;
  };

  struct BufferSlot {
    std::shared_ptr<TransformBuffer> active;
    std::shared_ptr<TransformBuffer> staging;
    std::chrono::nanoseconds cache_window = std::chrono::nanoseconds::max();
    bool replacement_in_progress = false;
  };

  // Returns the active buffer, creating it from the slot policy. Caller holds
  // tap_mutex_.
  [[nodiscard]] std::shared_ptr<TransformBuffer> ensureActiveLocked(BufferSlot& slot);

  PJ::SessionManager& session_;
  mutable std::mutex tap_mutex_;
  // Active/staging routing and cache-window policy shared with the worker tap.
  std::unordered_map<PJ::DatasetId, BufferSlot> transform_buffers_;
  // Per-topic ingest cursors (keyed by ObjectTopicId::id). A topic present here
  // is a known FrameTransforms topic. Topics proven NOT to be TF go into
  // non_tf_topics_ so they are classified (parsed) at most once instead of every
  // streaming tick.
  std::unordered_map<uint32_t, TfCursor> tf_cursors_;
  std::unordered_set<uint32_t> non_tf_topics_;
  // Last manual fixed-frame choice per dataset, for the current session. Also the
  // memo for rememberedFixedFrame's QSettings fallback: a present key (value may be
  // empty) means "already looked up". Keyed by the session-local DatasetId so
  // sibling docks share without a settings round-trip; invalidateDataset drops the
  // entry while the persisted QSettings copy survives (that is the cross-restart
  // memory). See rememberFixedFrame / rememberedFixedFrame.
  std::unordered_map<PJ::DatasetId, QString> remembered_fixed_frames_;
  std::optional<PJ::ObjectIngestTapRegistry::TapLease> tap_lease_;
  bool tap_active_ = false;
};

}  // namespace pj::scene3d
