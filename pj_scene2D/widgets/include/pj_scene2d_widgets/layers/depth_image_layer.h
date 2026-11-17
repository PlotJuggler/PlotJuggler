#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomElement>
#include <QWidget>
#include <memory>

#include "pj_scene2d_core/depth_pipeline_source.h"
#include "pj_scene2d_widgets/layers/scene2d_layer.h"
#include "pj_widgets/Colormap.h"

namespace PJ {

class MediaSource;

// A depth image topic (an Image with a depth encoding), colormapped on the GPU.
class DepthImageLayer final : public Scene2DLayer {
  Q_OBJECT
 public:
  DepthImageLayer(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name,
      QObject* parent = nullptr);

  QWidget* createConfigWidget(QWidget* parent) override;

 protected:
  [[nodiscard]] std::unique_ptr<MediaSource> createMediaSource(const SceneLayerContext& ctx) override;
  void onBeforeDetach() override;
  void saveOptions(QDomElement& element) const override;
  bool loadOptions(const QDomElement& element) override;

 private:
  void applyTo(DepthPipelineSource& source) const;
  void applyOptions();
  void setColormap(Colormap colormap);
  void setInvert(bool invert);
  void setRange(float near_m, float far_m);

  Colormap colormap_ = Colormap::kTurbo;
  bool invert_ = false;
  // Metres. Auto-fit sets it once from the current frame's percentiles.
  float near_m_ = 0.0f;
  float far_m_ = 4.0f;
  // Restored from XML; there is no UI control.
  float opacity_ = 1.0f;
  DepthPipelineSource* depth_source_ = nullptr;
};

}  // namespace PJ
