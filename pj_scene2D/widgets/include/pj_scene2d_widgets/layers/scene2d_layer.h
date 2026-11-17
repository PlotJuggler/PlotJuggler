#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QWidget>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene_common/scene_layer.h"

namespace PJ {

class MediaSource;
class SessionManager;

// One topic that contributes a MediaSource to the dock's composite and follows the playhead.
class Scene2DLayer : public ISceneLayer {
  Q_OBJECT
 public:
  Scene2DLayer(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, QString display_name, QString family_name,
      QObject* parent = nullptr);
  ~Scene2DLayer() override;

  [[nodiscard]] SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;

  bool attach(const SceneLayerContext& ctx) override;
  void detach() override;
  void setTrackerTime(PJ::Timepoint time) override;
  bool refreshAt(PJ::Timepoint time) override;
  // Fingerprints the active sample at `time`, so the dock skips the per-tick repaint of an unchanged frame. A new
  // sample's async decode repaints through the frame-ready callback regardless.
  [[nodiscard]] uint64_t renderKey(PJ::Timepoint time) const override;
  void setVisible(bool visible) override;
  QWidget* createConfigWidget(QWidget* parent) override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  [[nodiscard]] virtual MediaSource* mediaSource() {
    return borrowedSource();
  }
  [[nodiscard]] std::optional<int64_t> lastTrackerTimeNs() const noexcept;
  /// Sets the annotation part of the row warning: why this layer's annotations are hidden (not paired with the
  /// shown image), empty once they are shown. GUI thread.
  void setAnnotationWarning(const QString& warning) {
    setWarningPart(SceneWarningParts::Part::kAnnotation, warning);
  }

 protected:
  /// Non-null only after attach() stores the session and before detach() clears it;
  /// guaranteed valid inside createMediaSource(), onAfterAttach(), and onBeforeDetach().
  [[nodiscard]] ObjectStore* objectStore() const noexcept;
  /// Same attach window as objectStore(); nullptr before attach(), after detach(),
  /// and after a failed createMediaSource() during attach().
  [[nodiscard]] SessionManager* sessionManager() const noexcept;
  [[nodiscard]] ObjectTopicId topicId() const noexcept;
  [[nodiscard]] sdk::BuiltinObjectType objectType() const noexcept;
  [[nodiscard]] QString displayName() const;

  [[nodiscard]] SequentialUID shownUidAt(PJ::Timepoint time) const;
  void setSource(std::unique_ptr<MediaSource> source);
  [[nodiscard]] MediaSource* borrowedSource() const noexcept;
  /// A callback any thread may call, also after the layer is destroyed; it repaints on the GUI thread.
  [[nodiscard]] std::function<void()> makeQueuedRepaintCallback();
  /// Called during attach() after objectStore()/sessionManager() become valid.
  /// Returning nullptr fails attach() and clears those pointers again.
  [[nodiscard]] virtual std::unique_ptr<MediaSource> createMediaSource(const SceneLayerContext& ctx) = 0;
  virtual void onAfterAttach();
  virtual void onBeforeDetach();
  virtual void saveOptions(QDomElement& element) const;
  virtual bool loadOptions(const QDomElement& element);

 private:
  SceneLayerInfo info_;
  SessionManager* session_ = nullptr;
  ObjectStore* store_ = nullptr;
  std::unique_ptr<MediaSource> source_;
  std::optional<int64_t> last_tracker_time_ns_;
  SequentialUID shown_uid_;  ///< the entry shown at last_tracker_time_ns_ (invalid: none)
  QMetaObject::Connection dataset_replace_connection_;
  // replaceDataset keeps the ObjectTopicId and often the timestamps: renderKey folds this in, or the post-load seed
  // coalesces away the re-decode of the replacement.
  uint64_t data_generation_ = 0;
};

}  // namespace PJ
