#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <unordered_map>

#include "pj_base/types.hpp"

namespace PJ {
class SessionManager;
}  // namespace PJ

namespace pj::scene3d {

// Remembers the fixed frame the user last picked by hand for a dataset, so a
// NEWLY-created 3D dock on the same dataset defaults to it instead of the
// map/world/odom heuristic (see Scene3DDockWidget::resolveAutoFixedFrame). A
// view preference, not TF data, so it lives here rather than in TransformService.
//
// Two tiers: an in-session cache keyed by DatasetId (shared instantly by sibling
// docks) and a cross-restart record in QSettings keyed by the dataset's source
// path + name (the identity pair layout restore matches datasets by; name alone
// would let two same-named files in different folders share a frame).
//
// THREADING: GUI thread only.
class FixedFrameMemory : public QObject {
  Q_OBJECT
 public:
  // `session` must outlive this object.
  explicit FixedFrameMemory(PJ::SessionManager& session, QObject* parent = nullptr);

  // Records `frame` as the manual choice for `dataset_id`, in both tiers. A dataset
  // with no source name (e.g. an unnamed live stream) is remembered for this
  // session only. An empty `frame` is ignored.
  void remember(PJ::DatasetId dataset_id, const QString& frame);

  // The remembered manual frame for `dataset_id`, or empty if none. Checks the
  // session cache, then falls back to the persisted record, memoizing the result
  // (hit OR miss) so the hot frame-tree seeding path does not re-read QSettings.
  // Callers MUST still confirm the frame exists in the live TF tree: a remembered
  // frame can be absent from a different recording of the same source.
  // Mutates the memo, hence non-const.
  [[nodiscard]] QString remembered(PJ::DatasetId dataset_id);

 private:
  // The cross-restart QSettings key for `dataset_id`: its source path joined with
  // its source name (name alone for a pathless source), each percent-encoded so a
  // path can't be misread as a settings group separator. Empty when the dataset is
  // unknown or has no source name (then the choice is session-only).
  [[nodiscard]] QString sourceKey(PJ::DatasetId dataset_id) const;

  PJ::SessionManager& session_;
  // A present key (value may be empty) means "already looked up".
  std::unordered_map<PJ::DatasetId, QString> session_frames_;
};

}  // namespace pj::scene3d
