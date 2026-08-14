// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/transform_service.h"

#include <QLoggingCategory>
#include <QSettings>
#include <QThread>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/frame_transforms.hpp"
#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/object_topic_metadata.h"  // builtinObjectTypeFor
#include "pj_scene3d_widgets/parse_locked.h"
#include "pj_scene3d_widgets/resolve_object.h"  // resolveObject, hasCanonical3DCodec

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcTransformService, "pj.scene3d.transform_service")

// QSettings group for the cross-restart "last manual fixed frame" store, keyed by
// dataset source name. Mirrors the "pj_scene3d/scene_controls" convention.
constexpr char kFixedFrameBySourceGroup[] = "pj_scene3d/fixed_frame_by_source";

struct IngestStats {
  std::size_t ingested = 0;
  std::size_t dropped_reparent = 0;   // child claimed by two parents
  std::size_t dropped_self_loop = 0;  // child == parent
  std::size_t dropped_invalid = 0;    // empty name / non-unit rotation / non-finite translation
  // Entries whose lazy re-read FAILED (source file truncated, replaced, or
  // corrupt). Their edges are permanently lost: the drain cursor advances past
  // them (a corrupt region cannot be re-read into existence), so the loss must
  // be reported, not silently skipped.
  std::size_t failed_fetch = 0;
};

// Decode one object entry already known to belong to a FrameTransforms topic and
// push each of its edges into the buffer. A malformed edge (reparent conflict /
// self-loop / invalid frame data) is a recoverable data error in a real bag:
// count it and keep going rather than aborting.
void ingestEntry(
    const PJ::ResolvedObjectEntry& entry, const PJ::SessionManager::ParserBinding& parser_binding,
    TransformBuffer& tf_buffer, IngestStats& stats) {
  // The topic is already classified as FrameTransforms. resolveObject() decodes
  // it via the MessageParser when one is bound, or via the canonical codec when
  // not (a data-source/toolbox that pushed serialized canonical transforms).
  auto obj =
      resolveObject(parser_binding, PJ::sdk::BuiltinObjectType::kFrameTransforms, entry.timestamp, entry.payload);
  if (!obj.has_value()) {
    return;
  }
  const auto* ft = std::any_cast<PJ::sdk::FrameTransforms>(&obj->object);
  if (ft == nullptr) {
    return;
  }
  for (const auto& t : ft->transforms) {
    StampedTransform st;
    // Each transform carries its OWN inner stamp from the payload, which the store
    // never rewrites. A time-shifted dataset merge records the slide in
    // payload_stamp_shift; add it so a merged source's frames land on the anchor's
    // clock (it is 0 for unmerged data, so the common path is unchanged).
    st.stamp = TimePoint{std::chrono::nanoseconds(t.timestamp + entry.payload_stamp_shift)};
    st.parent_frame = t.parent_frame_id;
    st.child_frame = t.child_frame_id;
    st.transform.t = glm::dvec3{t.translation.x, t.translation.y, t.translation.z};
    st.transform.q = glm::dquat{t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z};
    const auto result = tf_buffer.setTransform(st);
    if (result.has_value()) {
      ++stats.ingested;
      continue;
    }
    switch (result.error()) {
      case SetTransformError::kReparentConflict:
        ++stats.dropped_reparent;
        break;
      case SetTransformError::kSelfLoop:
        ++stats.dropped_self_loop;
        break;
      case SetTransformError::kInvalidFrameName:
      case SetTransformError::kInvalidRotation:
      case SetTransformError::kNonFiniteTranslation:
        ++stats.dropped_invalid;
        break;
    }
  }
}
}  // namespace

TransformService::TransformService(PJ::SessionManager& session, QObject* parent) : QObject(parent), session_(session) {}

TransformService::~TransformService() = default;

std::shared_ptr<TransformBuffer> TransformService::transformBuffer(PJ::DatasetId dataset_id) {
  auto it = transform_buffers_.find(dataset_id);
  if (it != transform_buffers_.end()) {
    return it->second;
  }
  // Default to keeping the full history: ingestFrameTransformsForDataset()
  // bulk-loads the entire dataset's TF up front, so a rolling cache window would
  // trim every dynamic edge to its last samples and make objects in dynamic
  // frames (e.g. a local costmap in `odom`) resolve only near the end of the
  // timeline. Live-streaming datasets override this with setLiveCacheWindow() to
  // bound memory in step with the ObjectStore retention budget.
  auto buf = std::make_shared<TransformBuffer>(TransformBuffer::kKeepAll);
  transform_buffers_[dataset_id] = buf;
  return buf;
}

void TransformService::invalidateDataset(PJ::DatasetId dataset_id) {
  Q_ASSERT(QThread::currentThread() == thread());
  // Drop the in-session remembered fixed frame for this dataset; the persisted
  // QSettings copy (if any) is the cross-restart memory and stays put.
  remembered_fixed_frames_.erase(dataset_id);
  PJ::ObjectStore& object_store = session_.objectStore();
  // Cursor-model equivalent of forgetting the old transforms_populated_ flag:
  // drop the per-topic ingest cursors (and not-a-TF classifications) for this
  // dataset's topics so the next ingest re-reads their TF history from scratch
  // into the cleared buffer.
  for (const auto& topic_id : object_store.listTopics(dataset_id)) {
    tf_cursors_.erase(topic_id.id);
    non_tf_topics_.erase(topic_id.id);
  }
  // listTopics only resolves topics the dataset still has, so the loop above
  // misses keys for topics this reload/eviction already removed (on the reload
  // path replaceDataset drops removed_topics before we run; on the tombstone
  // path the caller must invalidate BEFORE eviction). Sweep the maps and erase
  // any key whose store descriptor is now empty so dead cursors do not leak
  // across reloads.
  std::erase_if(tf_cursors_, [&object_store](const auto& entry) {
    return object_store.descriptor(PJ::ObjectTopicId{entry.first}).topic_name.empty();
  });
  std::erase_if(non_tf_topics_, [&object_store](uint32_t key) {
    return object_store.descriptor(PJ::ObjectTopicId{key}).topic_name.empty();
  });
  if (auto it = transform_buffers_.find(dataset_id); it != transform_buffers_.end()) {
    // Clear in place: 3D docks hold this buffer by shared_ptr, so swapping the
    // map entry would leave them rendering the stale orphan forever.
    it->second->clear();
  }
}

void TransformService::setLiveCacheWindow(PJ::DatasetId dataset_id, std::chrono::nanoseconds window) {
  Q_ASSERT(QThread::currentThread() == thread());
  transformBuffer(dataset_id)->setCacheWindow(window);
}

void TransformService::invalidateAll() {
  tf_cursors_.clear();
  non_tf_topics_.clear();
  remembered_fixed_frames_.clear();
  for (auto& [dataset_id, buffer] : transform_buffers_) {
    (void)dataset_id;
    buffer->clear();
  }
}

QString TransformService::datasetSourceKey(PJ::DatasetId dataset_id) const {
  const PJ::DatasetInfo* info = session_.dataEngine().getDataset(dataset_id);
  if (info == nullptr || info->source_name.empty()) {
    return {};
  }
  // Qualify the name with the source path: two same-named files in different
  // folders must not share a remembered frame, and fan-out members (same path,
  // distinct names) must stay distinct — so the key needs both parts. Each part
  // is percent-encoded so a '/' can't be misread as a QSettings group separator;
  // '|' never survives the encoding, making it an unambiguous joiner. A pathless
  // source (e.g. a live stream with a stable name) keys by name alone.
  const QString name = QString::fromUtf8(QUrl::toPercentEncoding(QString::fromStdString(info->source_name)));
  const QString path = session_.datasetSourcePath(dataset_id);
  if (path.isEmpty()) {
    return name;
  }
  return QString::fromUtf8(QUrl::toPercentEncoding(path)) + u'|' + name;
}

void TransformService::rememberFixedFrame(PJ::DatasetId dataset_id, const QString& frame) {
  Q_ASSERT(QThread::currentThread() == thread());
  if (frame.isEmpty()) {
    return;
  }
  remembered_fixed_frames_[dataset_id] = frame;
  const QString key = datasetSourceKey(dataset_id);
  if (key.isEmpty()) {
    return;  // no stable cross-session identity (e.g. an unnamed live stream)
  }
  QSettings settings;
  settings.beginGroup(QLatin1String(kFixedFrameBySourceGroup));
  settings.setValue(key, frame);
}

QString TransformService::rememberedFixedFrame(PJ::DatasetId dataset_id) {
  Q_ASSERT(QThread::currentThread() == thread());
  if (auto it = remembered_fixed_frames_.find(dataset_id); it != remembered_fixed_frames_.end()) {
    return it->second;
  }
  // First lookup this session: fall back to the cross-restart store, keyed by the
  // dataset's source name. Memoize the result (hit OR miss) so the hot seeding path
  // does not re-read QSettings on every frame-tree change.
  QString frame;
  const QString key = datasetSourceKey(dataset_id);
  if (!key.isEmpty()) {
    QSettings settings;
    settings.beginGroup(QLatin1String(kFixedFrameBySourceGroup));
    frame = settings.value(key).toString();
  }
  remembered_fixed_frames_[dataset_id] = frame;
  return frame;
}

void TransformService::ingestFrameTransformsForDataset(PJ::DatasetId dataset_id) {
  Q_ASSERT(QThread::currentThread() == thread());
  // Bulk path: every cursor starts at the invalid (begin-of-history) UID, so
  // this ingests the whole history in one pass (file load); ingestNewerThanCursor
  // creates the buffer. datasetTransformsReady tells 3D docks the tree is ready.
  ingestNewerThanCursor(dataset_id);
  emit datasetTransformsReady(dataset_id);
}

bool TransformService::ingestNewTransforms(PJ::DatasetId dataset_id) {
  return ingestNewerThanCursor(dataset_id);
}

bool TransformService::ingestNewerThanCursor(PJ::DatasetId dataset_id) {
  Q_ASSERT(QThread::currentThread() == thread());
  PJ::ObjectStore& object_store = session_.objectStore();
  auto tf_buffer = transformBuffer(dataset_id);
  IngestStats stats;

  for (const auto& topic_id : object_store.listTopics(dataset_id)) {
    const uint32_t key = topic_id.id;
    if (non_tf_topics_.contains(key)) {
      continue;  // already classified as not-a-TF topic; never re-probe it
    }
    const auto count = object_store.entryCount(topic_id);
    if (count == 0) {
      continue;  // nothing to classify/ingest yet; retry on a later tick
    }
    const auto parser_binding = session_.parserBindingForObjectTopic(topic_id);

    auto cursor_it = tf_cursors_.find(key);
    if (cursor_it == tf_cursors_.end()) {
      // Classify the topic's object type exactly once, then cache the verdict (a
      // TF cursor, or non_tf_topics_) so a big non-TF topic isn't re-classified
      // on every tick.
      if (!parser_binding) {
        // Parser-less topic: its bytes are a serialized canonical object, so the
        // topic's builtin_object_type metadata is authoritative — classify by
        // metadata, no decode needed (a data-source/toolbox canonical producer,
        // e.g. the Mosaico cloud toolbox).
        const PJ::sdk::BuiltinObjectType type = builtinObjectTypeFor(object_store.descriptor(topic_id));
        if (type == PJ::sdk::BuiltinObjectType::kNone) {
          continue;  // not classifiable yet (e.g. a parser topic mid-bind) — retry next tick
        }
        if (type != PJ::sdk::BuiltinObjectType::kFrameTransforms) {
          non_tf_topics_.insert(key);  // a canonical non-TF object: settled, never re-probe
          continue;
        }
      } else {
        // Parser-backed topic: the parser (not the metadata) determines the type,
        // so probe its newest entry's decoded type exactly once. Probing the
        // newest (not index 0) stays valid after streaming evicts the front.
        // A fetch_failed newest entry is PERMANENTLY unresolvable — retrying it
        // every tick would re-run a failing re-read forever and block the topic
        // from ever classifying (starving healthy older edges) — so walk back a
        // few entries to find one that resolves instead.
        std::optional<PJ::ResolvedObjectEntry> probe;
        constexpr std::size_t kMaxProbeWalkback = 8;
        for (std::size_t back = 0; back < std::min(count, kMaxProbeWalkback); ++back) {
          auto candidate = object_store.at(topic_id, count - 1 - back);
          if (candidate.has_value() && candidate->fetch_failed) {
            continue;  // permanent failure: never re-probe this entry
          }
          probe = std::move(candidate);
          break;
        }
        if (!probe.has_value() || probe->payload.bytes.empty()) {
          continue;  // can't classify yet; retry next tick
        }
        auto probe_obj = parseLocked(parser_binding, probe->timestamp, probe->payload);
        if (!probe_obj.has_value()) {
          // Parse FAILED (transient corruption / a parser that failed this tick) —
          // do NOT blacklist the topic. A single bad newest message must not
          // permanently suppress TF ingest; retry classification next tick.
          continue;
        }
        if (PJ::sdk::typeOf(probe_obj->object) != PJ::sdk::BuiltinObjectType::kFrameTransforms) {
          non_tf_topics_.insert(key);  // parsed cleanly as a different type: settled, never re-probe
          continue;
        }
      }
      // Classified as TF by either path: open a cursor and start ingesting.
      cursor_it = tf_cursors_.try_emplace(key).first;
    }

    // Drain every edge that arrived since the cursor, in arrival order, and file
    // each into the time-indexed TF buffer. Arrival order (not a time window) is
    // required: a late out-of-order edge (older stamp, newest UID) must still be
    // ingested, and the buffer places it at its own time. drainNewSince is
    // eviction-safe and advances the cursor past every entry (even one evicted
    // before it resolves), so nothing is ingested twice or skipped.
    TfCursor& cursor = cursor_it->second;
    for (const auto& entry : object_store.drainNewSince(topic_id, cursor.last_ingested)) {
      if (entry.fetch_failed) {
        ++stats.failed_fetch;
        continue;
      }
      if (entry.payload.bytes.empty()) {
        continue;
      }
      ingestEntry(entry, parser_binding, *tf_buffer, stats);
    }
  }

  if (stats.dropped_reparent != 0 || stats.dropped_self_loop != 0 || stats.dropped_invalid != 0) {
    qCWarning(lcTransformService) << "ingestNewerThanCursor" << dataset_id << ": dropped" << stats.dropped_reparent
                                  << "reparent-conflict," << stats.dropped_self_loop << "self-loop, and"
                                  << stats.dropped_invalid << "invalid edge(s)";
  }
  if (stats.failed_fetch != 0) {
    qCWarning(lcTransformService) << "ingestNewerThanCursor" << dataset_id << ":" << stats.failed_fetch
                                  << "transform message(s) LOST: lazy re-read failed (source file truncated,"
                                  << "replaced, or corrupt)";
  }
  return stats.ingested != 0;
}

}  // namespace pj::scene3d
