// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/FixedFrameMemory.h"

#include <QSettings>
#include <QThread>
#include <QUrl>

#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_runtime/SessionManager.h"

namespace pj::scene3d {

namespace {
// QSettings group for the cross-restart "last manual fixed frame" store, keyed by
// dataset source. Mirrors the "pj_scene3d/scene_controls" convention.
constexpr char kFixedFrameBySourceGroup[] = "pj_scene3d/fixed_frame_by_source";
}  // namespace

FixedFrameMemory::FixedFrameMemory(PJ::SessionManager& session, QObject* parent) : QObject(parent), session_(session) {}

QString FixedFrameMemory::sourceKey(PJ::DatasetId dataset_id) const {
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

void FixedFrameMemory::remember(PJ::DatasetId dataset_id, const QString& frame) {
  Q_ASSERT(QThread::currentThread() == thread());
  if (frame.isEmpty()) {
    return;
  }
  session_frames_[dataset_id] = frame;
  const QString key = sourceKey(dataset_id);
  if (key.isEmpty()) {
    return;  // no stable cross-session identity (e.g. an unnamed live stream)
  }
  QSettings settings;
  settings.beginGroup(QLatin1String(kFixedFrameBySourceGroup));
  settings.setValue(key, frame);
}

QString FixedFrameMemory::remembered(PJ::DatasetId dataset_id) {
  Q_ASSERT(QThread::currentThread() == thread());
  if (auto it = session_frames_.find(dataset_id); it != session_frames_.end()) {
    return it->second;
  }
  QString frame;
  const QString key = sourceKey(dataset_id);
  if (!key.isEmpty()) {
    QSettings settings;
    settings.beginGroup(QLatin1String(kFixedFrameBySourceGroup));
    frame = settings.value(key).toString();
  }
  session_frames_[dataset_id] = frame;
  return frame;
}

}  // namespace pj::scene3d
