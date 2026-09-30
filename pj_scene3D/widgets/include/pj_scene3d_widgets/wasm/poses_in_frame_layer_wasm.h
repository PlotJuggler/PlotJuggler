#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QDomDocument>
#include <QMetaObject>
#include <QString>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pj_datastore/object_store.hpp"
#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/poses_budget.h"
#include "pj_scene3d_core/poses_in_frame_render.h"
#include "pj_scene_common/scene_layer.h"

namespace PJ {
class SessionManager;
}

namespace pj::scene3d {

// Emscripten-only canonical PosesInFrame adapter. It shares decode, pose-to-mat4
// expansion, XML vocabulary, and display defaults with the native layer while
// leaving QRhi ownership in SceneViewWidget. The desktop PosesInFrameLayer and
// OpenGL pass are not compiled through this path.
class WasmPosesInFrameLayer final : public PJ::ISceneLayer {
  Q_OBJECT

 public:
  static constexpr std::uint64_t kMaxPosesPerLayer = kSceneLimits.max_poses_per_layer;
  static constexpr std::uint64_t kMaxWireBytesPerLayer = kSceneLimits.max_pose_wire_bytes_per_layer;

  WasmPosesInFrameLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~WasmPosesInFrameLayer() override;

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  bool attach(const PJ::SceneLayerContext& context) override;
  void detach() override;
  void setTrackerTime(PJ::Timepoint time) override;
  [[nodiscard]] std::uint64_t renderKey(PJ::Timepoint time) const override;
  void setVisible(bool visible) override;
  QWidget* createConfigWidget(QWidget* parent) override;
  QDomElement xmlSaveState(QDomDocument& document) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool prepareForRender(std::uint64_t remaining_view_arms);

  [[nodiscard]] const std::vector<PoseTriadInstance>& instances() const {
    return instances_;
  }
  [[nodiscard]] std::uint64_t geometryRevision() const {
    return geometry_revision_;
  }
  [[nodiscard]] const std::string& sourceFrame() const {
    return source_frame_;
  }
  [[nodiscard]] std::optional<AABB> sourceBounds() const {
    return source_bounds_.valid ? std::optional<AABB>{source_bounds_} : std::nullopt;
  }
  [[nodiscard]] bool visible() const {
    return visible_;
  }
  [[nodiscard]] float gizmoSize() const {
    return gizmo_size_;
  }
  [[nodiscard]] float gizmoOpacity() const {
    return gizmo_opacity_;
  }
  [[nodiscard]] bool xArrowOnly() const {
    return x_arrow_only_;
  }
  [[nodiscard]] bool overrideColorEnabled() const {
    return override_color_enabled_;
  }
  [[nodiscard]] QColor overrideColor() const {
    return override_color_;
  }
  [[nodiscard]] QString warningReason() const {
    return warning_reason_;
  }

  void setGizmoSize(float metres);
  void setGizmoOpacity(float opacity);
  void setXArrowOnly(bool x_arrow_only);
  void setOverrideColorEnabled(bool enabled);
  void setOverrideColor(QColor color);

  [[nodiscard]] static bool validateXml(const QDomElement& element);

 signals:
  void sourceFrameChanged(QString frame);

 private:
  struct ParsedSettings {
    float gizmo_size = 0.15F;
    float gizmo_opacity = 1.0F;
    bool x_arrow_only = false;
    bool override_color_enabled = false;
    std::optional<QColor> override_color;
  };

  [[nodiscard]] static std::optional<ParsedSettings> parseSettings(const QDomElement& element);
  bool bootstrap();
  bool decodeAt(PJ::Timepoint time);
  void requestDecode();
  void clearGeometry();
  void updateSourceFrame(const std::string& frame);
  void setWarning(QString warning);
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  PJ::SessionManager* session_ = nullptr;
  QMetaObject::Connection reload_connection_;

  std::vector<PoseTriadInstance> instances_;
  std::string source_frame_;
  AABB source_bounds_;
  std::uint64_t geometry_revision_ = 0;
  PJ::SequentialUID staged_uid_{};
  int staged_style_revision_ = -1;
  std::optional<PJ::Timepoint> requested_time_;
  std::optional<std::uint64_t> budget_rejected_arm_count_;
  bool decode_dirty_ = false;
  bool visible_ = true;
  QString warning_reason_;

  float gizmo_size_ = 0.15F;
  float gizmo_opacity_ = 1.0F;
  bool x_arrow_only_ = false;
  bool override_color_enabled_ = false;
  QColor override_color_{255, 0, 0};
  int style_revision_ = 0;
};

}  // namespace pj::scene3d
