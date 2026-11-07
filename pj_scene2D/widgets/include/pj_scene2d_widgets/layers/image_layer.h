#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomElement>
#include <QWidget>
#include <memory>

#include "pj_scene2d_widgets/layers/scene2d_layer.h"

namespace PJ {

class MediaSource;
class ImagePipelineSource;
class ToggleSwitch;

// Scene2DLayer for a single Image topic: contributes an image MediaSource to the
// composite. Images always magnify with nearest sampling (crisp pixels). Exposes one
// display control — a rectification toggle (undistort the image with its CameraInfo,
// on by default; turn off to override the always-on auto-decision for an
// already-rectified stream).
class ImageLayer final : public Scene2DLayer {
  Q_OBJECT
 public:
  ImageLayer(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name,
      QObject* parent = nullptr);

  QWidget* createConfigWidget(QWidget* parent) override;

 protected:
  [[nodiscard]] std::unique_ptr<MediaSource> createMediaSource(const SceneLayerContext& ctx) override;
  void onBeforeDetach() override;
  void saveOptions(QDomElement& element) const override;
  bool loadOptions(const QDomElement& element) override;

 private:
  void setRectifyEnabled(bool enabled);
  void applyOptions();
  /// Keep the control's eligibility and explanation in sync with the displayed image.
  void updateRectifyControl(ToggleSwitch* toggle) const;

  bool rectify_enabled_ = true;  ///< true = undistort with CameraInfo (default), false = raw passthrough
  ImagePipelineSource* image_source_ = nullptr;  ///< borrowed; valid between attach and detach
};

}  // namespace PJ
