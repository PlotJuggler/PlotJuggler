#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QImage>
#include <QList>
#include <QPoint>
#include <QRhiWidget>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "pj_base/time.hpp"
#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/model_budget.h"
#include "pj_scene3d_core/occupancy_grid_budget.h"
#include "pj_scene3d_core/pointcloud_budget.h"
#include "pj_scene3d_core/poses_budget.h"
#include "pj_scene3d_core/poses_in_frame_render.h"
#include "pj_scene3d_core/scene_entities_budget.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/voxel_grid_budget.h"
#include "pj_scene3d_widgets/mesh_data.h"
#include "pj_scene3d_widgets/mesh_shading_params.h"
#include "pj_scene3d_widgets/passes/grid_geometry.h"

class QEvent;
class QMouseEvent;
class QWheelEvent;
class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderBuffer;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;
class QRhiRenderPassDescriptor;

namespace PJ {
class ISceneLayer;
}

namespace pj::scene3d {

class WasmPointRenderable;
class WasmPosesInFrameLayer;
class WasmOccupancyGridLayer;
class WasmVoxelGridLayer;
class WasmSceneEntitiesLayer;
class WasmModelRenderable;

// WebAssembly product implementation of the public SceneViewWidget contract.
// The native class with the same name remains a QOpenGLWidget in the other
// platform branch of scene_view_widget.h.  This class owns only QRhi resources;
// camera and TF semantics continue to come from pj_scene3d_core.
class SceneViewWidget : public QRhiWidget {
  Q_OBJECT

 public:
  using GridStyle = pj::scene3d::GridStyle;
  enum class CameraModel { kOrbit, kXyOrbit, kFly, kTopDownOrtho };

  explicit SceneViewWidget(QWidget* parent = nullptr);
  ~SceneViewWidget() override;

  void setTransformBuffer(std::shared_ptr<TransformBuffer> tf);
  void setTrackerTime(PJ::Timepoint time);
  void setFixedFrame(const std::string& frame);
  [[nodiscard]] const std::string& fixedFrame() const {
    return fixed_frame_;
  }

  void setFollowFrame(const std::string& frame);
  [[nodiscard]] const std::string& followFrame() const {
    return follow_frame_;
  }
  [[nodiscard]] uint64_t followRenderKey(PJ::Timepoint time) const;
  void recenterOnFollowFrame();

  [[nodiscard]] ICamera& camera() {
    return *camera_;
  }
  [[nodiscard]] const ICamera& camera() const {
    return *camera_;
  }
  void setCameraModel(CameraModel model);
  [[nodiscard]] CameraModel cameraModel() const {
    return camera_model_;
  }
  void resetCamera();
  void setSceneBounds(const AABB& bounds);
  void setLayers(const std::vector<PJ::ISceneLayer*>& ordered_layers);

  void setGridStyle(GridStyle style);
  [[nodiscard]] GridStyle gridStyle() const {
    return grid_style_;
  }
  void setGridDivisions(int divisions);
  [[nodiscard]] int gridDivisions() const {
    return grid_divisions_;
  }
  void setGridExtentMetres(float extent_m);
  [[nodiscard]] float gridExtentMetres() const {
    return grid_extent_m_;
  }
  void setGridVisible(bool visible);
  [[nodiscard]] bool gridVisible() const {
    return grid_visible_;
  }

  void setAxesVisible(bool visible);
  [[nodiscard]] bool axesVisible() const {
    return axes_visible_;
  }
  void setGizmoSize(float length_m);
  [[nodiscard]] float gizmoSize() const {
    return gizmo_size_m_;
  }
  void setGizmoOpacity(float opacity);
  [[nodiscard]] float gizmoOpacity() const {
    return gizmo_opacity_;
  }
  void setTfConnectionsVisible(bool visible);
  [[nodiscard]] bool tfConnectionsVisible() const {
    return tf_connections_visible_;
  }

  [[nodiscard]] MeshShadingParams& meshShadingParams() {
    return shading_params_;
  }
  [[nodiscard]] const MeshShadingParams& meshShadingParams() const {
    return shading_params_;
  }
  void setMeshShadingParams(const MeshShadingParams& params);

  void refreshAvailableFrames();
  [[nodiscard]] uint64_t tfRenderKey(PJ::Timepoint time) const;

  // Delivers the current scene as an image to `done`; use it instead of
  // grabFramebuffer(). A shown view first presents one real frame (asynchronously,
  // up to 2 s): Qt 6.11.1's OpenGL QRhi advances its frame number only in
  // swapchain frames, so across grab-only frames a pipeline sharing an SRB keeps
  // stale uniforms (an old camera). The image is null when that frame never comes
  // (occluded window). A never-shown view is grabbed at once, and in Qt 6.11.1
  // only ONCE: each such grab creates a new QRhi without re-running initialize().
  // `done` runs once on the GUI thread, or never if the view is destroyed first.
  // Never blocks: no nested event loop, which Qt for WebAssembly cannot run.
  void grabPresentedFrame(std::function<void(const QImage&)> done);

  // Observation-only browser acceptance seams.  They report the last submitted
  // product frame; no test method mutates the camera, TF buffer, or renderer.
  [[nodiscard]] int lastLineVertexCountForTest() const {
    return last_line_vertex_count_;
  }
  [[nodiscard]] int lastTriangleVertexCountForTest() const {
    return last_triangle_vertex_count_;
  }
  [[nodiscard]] int lastResolvedFrameCountForTest() const {
    return last_resolved_frame_count_;
  }
  [[nodiscard]] int lastPointVertexCountForTest() const {
    return last_point_vertex_count_;
  }
  [[nodiscard]] int lastPointLayerCountForTest() const {
    return last_point_layer_count_;
  }
  [[nodiscard]] int lastCubeLayerCountForTest() const {
    return last_cube_layer_count_;
  }
  [[nodiscard]] int lastCubeInstanceCountForTest() const {
    return last_cube_instance_count_;
  }
  [[nodiscard]] int cameraFitCountForTest() const {
    return camera_fit_count_;
  }
  [[nodiscard]] AABB lastPointBoundsForTest() const {
    return last_point_bounds_;
  }
  [[nodiscard]] int lastDepthLayerCountForTest() const {
    return last_depth_layer_count_;
  }
  [[nodiscard]] int lastDepthVertexCountForTest() const {
    return last_depth_vertex_count_;
  }
  [[nodiscard]] AABB lastDepthBoundsForTest() const {
    return last_depth_bounds_;
  }
  [[nodiscard]] int lastPoseLayerCountForTest() const {
    return last_pose_layer_count_;
  }
  [[nodiscard]] int lastPoseArmCountForTest() const {
    return last_pose_arm_count_;
  }
  [[nodiscard]] AABB lastPoseBoundsForTest() const {
    return last_pose_bounds_;
  }
  [[nodiscard]] int lastOccupancyLayerCountForTest() const {
    return last_occupancy_layer_count_;
  }
  [[nodiscard]] int lastOccupancyCellCountForTest() const {
    return last_occupancy_cell_count_;
  }
  [[nodiscard]] int occupancyFullUploadCountForTest() const {
    return occupancy_full_upload_count_;
  }
  [[nodiscard]] int occupancyPartialUploadCountForTest() const {
    return occupancy_partial_upload_count_;
  }
  [[nodiscard]] AABB lastOccupancyBoundsForTest() const {
    return last_occupancy_bounds_;
  }
  [[nodiscard]] int lastVoxelLayerCountForTest() const {
    return last_voxel_layer_count_;
  }
  [[nodiscard]] int lastVoxelCountForTest() const {
    return last_voxel_count_;
  }
  [[nodiscard]] int voxelFullUploadCountForTest() const {
    return voxel_full_upload_count_;
  }
  [[nodiscard]] int max3DTextureSizeForTest() const {
    return max_3d_texture_size_;
  }
  [[nodiscard]] AABB lastVoxelBoundsForTest() const {
    return last_voxel_bounds_;
  }
  [[nodiscard]] int lastMarkerLayerCountForTest() const {
    return last_marker_layer_count_;
  }
  [[nodiscard]] int lastMarkerInstanceCountForTest() const {
    return last_marker_instance_count_;
  }
  [[nodiscard]] int lastMarkerStreamVertexCountForTest() const {
    return last_marker_stream_vertex_count_;
  }
  [[nodiscard]] AABB lastMarkerBoundsForTest() const {
    return last_marker_bounds_;
  }
  [[nodiscard]] int lastModelLayerCountForTest() const {
    return last_model_layer_count_;
  }
  [[nodiscard]] int lastModelDrawCountForTest() const {
    return last_model_draw_count_;
  }
  [[nodiscard]] int lastModelTriangleCountForTest() const {
    return last_model_triangle_count_;
  }
  [[nodiscard]] std::array<int, 5> lastModelTextureSlotCountsForTest() const {
    return last_model_texture_slot_counts_;
  }
  [[nodiscard]] AABB lastModelBoundsForTest() const {
    return last_model_bounds_;
  }
  [[nodiscard]] bool lastShadowActiveForTest() const {
    return shadow_stage_.last_active;
  }
  [[nodiscard]] bool shadowResourcesReadyForTest() const {
    return shadow_texture_ != nullptr && shadow_render_target_ != nullptr;
  }
  [[nodiscard]] bool lastShadowFitValidForTest() const {
    return last_shadow_fit_valid_;
  }
  [[nodiscard]] int shadowMapSizeForTest() const {
    return shadow_map_size_;
  }
  [[nodiscard]] int lastShadowDrawCountForTest() const {
    return last_shadow_draw_count_;
  }
  [[nodiscard]] int lastShadowTriangleCountForTest() const {
    return last_shadow_triangle_count_;
  }
  [[nodiscard]] AABB lastShadowBoundsForTest() const {
    return last_shadow_bounds_;
  }
  [[nodiscard]] const QString& shadowCapabilityErrorForTest() const {
    return shadow_stage_.capability_error;
  }
  [[nodiscard]] std::uint64_t shadowResourceGenerationForTest() const {
    return shadow_stage_.resource_generation;
  }
  [[nodiscard]] bool lastHdrActiveForTest() const {
    return hdr_stage_.last_active;
  }
  [[nodiscard]] bool hdrResourcesReadyForTest() const {
    return hdr_render_target_ != nullptr && hdr_depth_render_target_ != nullptr && hdr_resolve_color_ != nullptr &&
           hdr_resolve_depth_ != nullptr && hdr_resolve_coverage_ != nullptr;
  }
  [[nodiscard]] QSize hdrRenderSizeForTest() const {
    return hdr_render_size_;
  }
  [[nodiscard]] std::uint64_t hdrAllocatedBytesForTest() const {
    return hdr_allocated_bytes_;
  }
  [[nodiscard]] const QString& hdrCapabilityErrorForTest() const {
    return hdr_stage_.capability_error;
  }
  [[nodiscard]] std::uint64_t hdrResourceGenerationForTest() const {
    return hdr_stage_.resource_generation;
  }
  void setForceHdrFallbackForTest(bool force);
  [[nodiscard]] bool lastSsaoActiveForTest() const {
    return ssao_stage_.last_active;
  }
  [[nodiscard]] bool ssaoResourcesReadyForTest() const {
    return ssao_raw_render_target_ != nullptr && ssao_pipeline_ != nullptr && ssao_shader_resources_ != nullptr &&
           ssao_uniform_buffer_ != nullptr;
  }
  [[nodiscard]] const QString& ssaoCapabilityErrorForTest() const {
    return ssao_stage_.capability_error;
  }
  [[nodiscard]] std::uint64_t ssaoResourceGenerationForTest() const {
    return ssao_stage_.resource_generation;
  }
  void setForceSsaoFallbackForTest(bool force);
  // Same acceptance-only fault injection for the shadow chain: forces the
  // capability error the specs assert on and releases the shadow resources.
  void setForceShadowFallbackForTest(bool force);
  [[nodiscard]] bool lastEdlActiveForTest() const {
    return edl_stage_.last_active;
  }
  [[nodiscard]] bool edlResourcesReadyForTest() const {
    return edl_mesh_mask_render_target_ != nullptr && edl_mesh_mask_triangle_pipeline_ != nullptr &&
           edl_mesh_mask_line_pipeline_ != nullptr;
  }
  [[nodiscard]] const QString& edlCapabilityErrorForTest() const {
    return edl_stage_.capability_error;
  }
  [[nodiscard]] const QString& renderingWarningForTest() const {
    return rendering_warning_;
  }
  [[nodiscard]] std::uint64_t edlResourceGenerationForTest() const {
    return edl_stage_.resource_generation;
  }
  void setForceEdlFallbackForTest(bool force);
  [[nodiscard]] const std::vector<std::uint32_t>& lastSubmittedLayerIdsForTest() const {
    return last_submitted_layer_ids_;
  }
  // QRhi::backendName() of the last initialize() ("OpenGL", "Vulkan", ...):
  // the backend actually used, which can differ from api() after a fallback.
  // Kept after releaseResources(), so it survives an offscreen grab.
  [[nodiscard]] const QString& rhiBackendNameForTest() const {
    return rhi_backend_name_;
  }

 signals:
  void framesChanged(const QList<FrameRow>& frames);
  void presentationChanged();
  void renderingWarningChanged(const QString& warning);

 protected:
  void initialize(QRhiCommandBuffer* command_buffer) override;
  void render(QRhiCommandBuffer* command_buffer) override;
  void releaseResources() override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  // Shared placement step of the per-family render loops; see the definition.
  [[nodiscard]] std::optional<Transform> resolveFixedFromSource(
      const std::string& source_frame, const std::string& fixed_frame) const;
  using OrderedLayer = std::variant<
      WasmPointRenderable*, WasmPosesInFrameLayer*, WasmOccupancyGridLayer*, WasmVoxelGridLayer*,
      WasmSceneEntitiesLayer*, WasmModelRenderable*>;
  struct OrderedLayerEntry {
    OrderedLayer layer;
    std::uint32_t topic_id = 0;
  };

  void setPointRenderableLayers(const std::vector<WasmPointRenderable*>& layers);
  void setPosesInFrameLayers(const std::vector<WasmPosesInFrameLayer*>& layers);
  void setOccupancyGridLayers(const std::vector<WasmOccupancyGridLayer*>& layers);
  void setVoxelGridLayers(const std::vector<WasmVoxelGridLayer*>& layers);
  void setSceneEntitiesLayers(const std::vector<WasmSceneEntitiesLayer*>& layers);
  void setModelRenderableLayers(const std::vector<WasmModelRenderable*>& layers);

  struct Vertex {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
  };

  struct PointLayerGpu {
    QRhiBuffer* vertex_buffer = nullptr;
    QRhiBuffer* uniform_buffer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 vertex_capacity = 0;
    quint32 vertex_count = 0;
    uint64_t uploaded_revision = ~uint64_t{0};
  };

  struct PoseLayerGpu {
    QRhiBuffer* instance_buffer = nullptr;
    QRhiBuffer* uniform_buffer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 instance_capacity = 0;
    quint32 instance_count = 0;
    uint64_t uploaded_revision = ~uint64_t{0};
  };

  struct OccupancyLayerGpu {
    QRhiTexture* texture = nullptr;
    QRhiBuffer* uniform_buffer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 width = 0;
    quint32 height = 0;
    uint64_t uploaded_revision = ~uint64_t{0};
  };

  struct VoxelLayerGpu {
    QRhiTexture* texture = nullptr;
    QRhiBuffer* uniform_buffer = nullptr;
    QRhiShaderResourceBindings* shader_resources = nullptr;
    quint32 columns = 0;
    quint32 rows = 0;
    quint32 slices = 0;
    int value_kind = -1;
    uint64_t uploaded_revision = ~uint64_t{0};
  };

  struct MarkerInstance {
    std::array<float, 16> model{};
    float r = 1.0F;
    float g = 1.0F;
    float b = 1.0F;
    float a = 1.0F;
    float bottom_scale = 1.0F;
    float top_scale = 1.0F;
    float unused0 = 0.0F;
    float unused1 = 0.0F;
  };

  struct MarkerMeshGpu {
    QRhiBuffer* vertex_buffer = nullptr;
    QRhiBuffer* triangle_index_buffer = nullptr;
    QRhiBuffer* edge_index_buffer = nullptr;
    quint32 triangle_index_count = 0;
    quint32 edge_index_count = 0;
  };

  struct ModelInstance {
    std::array<float, 16> model{};
    std::array<float, 4> tint{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> params{};
    std::array<float, 4> texture_flags{};
    std::array<float, 4> pbr_factors{};
    std::array<float, 4> emissive_factor{};
  };

  struct ShadowInstance {
    std::array<float, 16> model{};
  };
  static_assert(sizeof(ShadowInstance) == 64U);

  struct ModelTextureGpu {
    QRhiTexture* texture = nullptr;
    QImage pending_image;
    bool owns_texture = false;
    bool texture_upload_pending = false;
    bool present = false;
  };

  struct ModelMaterialGpu {
    std::array<ModelTextureGpu, 5> textures{};
    QRhiShaderResourceBindings* shader_resources = nullptr;
  };

  struct ModelMeshGpu {
    QRhiBuffer* vertex_buffer = nullptr;
    QRhiBuffer* triangle_index_buffer = nullptr;
    QRhiBuffer* edge_index_buffer = nullptr;
    std::vector<quint32> edge_indices;
    std::vector<quint32> edge_offsets;
    std::vector<quint32> edge_counts;
    std::vector<ModelMaterialGpu> materials;
    std::shared_ptr<const MeshData> source;
    bool upload_pending = false;
  };

  struct ModelLayerGpu {
    std::unordered_map<std::string, ModelMeshGpu> meshes;
    std::uint64_t uploaded_revision = ~std::uint64_t{0};
  };

  void applyFollow();
  [[nodiscard]] std::string effectiveFixedFrame() const;
  void buildGeometry(const glm::dvec3& render_origin);
  void appendLine(const glm::dvec3& a, const glm::dvec3& b, const glm::vec4& color);
  void appendTriangle(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, const glm::vec4& color);
  bool ensureVertexBuffer(
      QRhi* owner, QRhiBuffer*& buffer, quint32& capacity, quint32 required_bytes, quint32 minimum_capacity);
  bool ensurePointLayerGpu(QRhi* owner, WasmPointRenderable* layer, PointLayerGpu& gpu);
  void releasePointLayerGpu(PointLayerGpu& gpu);
  bool ensurePoseLayerGpu(QRhi* owner, PoseLayerGpu& gpu);
  void releasePoseLayerGpu(PoseLayerGpu& gpu);
  bool ensureOccupancyLayerGpu(
      QRhi* owner, WasmOccupancyGridLayer* layer, OccupancyLayerGpu& gpu, bool& texture_recreated);
  void releaseOccupancyLayerGpu(OccupancyLayerGpu& gpu);
  bool ensureVoxelLayerGpu(QRhi* owner, WasmVoxelGridLayer* layer, VoxelLayerGpu& gpu, bool& texture_recreated);
  [[nodiscard]] QString voxelGpuRejection(QRhi* owner, const WasmVoxelGridLayer* layer);
  void releaseVoxelLayerGpu(VoxelLayerGpu& gpu);
  void releaseMarkerMeshGpu(MarkerMeshGpu& gpu);
  bool ensureModelLayerGpu(QRhi* owner, WasmModelRenderable* layer, ModelLayerGpu& gpu);
  QRhiShaderResourceBindings* createModelMaterialBindings(QRhi* owner, const ModelMaterialGpu& material);
  bool rebuildModelMaterialBindings(QRhi* owner, ModelMaterialGpu& material);
  bool rebuildAllModelMaterialBindings(QRhi* owner);
  void releaseModelMeshGpu(ModelMeshGpu& gpu);
  void releaseModelLayerGpu(ModelLayerGpu& gpu);
  bool ensureShadowResources(QRhi* owner);
  void releaseShadowResources(bool restore_model_bindings);
  bool ensureHdrResources(QRhi* owner, const QSize& size);
  bool ensureHdrPipelines(QRhi* owner);
  void releaseHdrTargetResources();
  void releaseHdrResources();
  bool ensureSsaoResources(QRhi* owner, const QSize& size);
  void releaseSsaoTargetResources();
  void releaseSsaoResources();
  bool ensureEdlResources(QRhi* owner, const QSize& size);
  void releaseEdlTargetResources();
  void releaseEdlResources();
  void updateRenderingWarning();
  void emitPresentationIfChanged(const CameraState& before);

  std::shared_ptr<TransformBuffer> tf_;
  PJ::Timepoint render_time_{};
  std::string fixed_frame_;
  std::string follow_frame_;
  glm::dvec3 follow_previous_origin_{0.0};
  bool follow_seeded_ = false;

  std::unique_ptr<ICamera> camera_{std::make_unique<OrbitCamera>()};
  CameraModel camera_model_ = CameraModel::kOrbit;
  AABB scene_bounds_{};

  GridStyle grid_style_ = GridStyle::kLines;
  float grid_extent_m_ = 10.0F;
  int grid_divisions_ = 10;
  bool grid_visible_ = true;
  bool axes_visible_ = true;
  float gizmo_size_m_ = 0.15F;
  float gizmo_opacity_ = 1.0F;
  bool tf_connections_visible_ = true;
  MeshShadingParams shading_params_;

  uint64_t last_frames_revision_ = ~uint64_t{0};
  QList<FrameRow> last_frame_list_;
  mutable std::vector<std::string> tf_render_key_frames_;

  QRhi* resource_rhi_ = nullptr;
  QString rhi_backend_name_;
  QRhiBuffer* uniform_buffer_ = nullptr;
  QRhiBuffer* render_mode_uniform_buffer_ = nullptr;
  QRhiBuffer* annotation_render_mode_uniform_buffer_ = nullptr;
  QRhiBuffer* line_buffer_ = nullptr;
  QRhiBuffer* triangle_buffer_ = nullptr;
  QRhiShaderResourceBindings* shader_resources_ = nullptr;
  QRhiShaderResourceBindings* annotation_shader_resources_ = nullptr;
  QRhiGraphicsPipeline* line_pipeline_ = nullptr;
  QRhiGraphicsPipeline* triangle_pipeline_ = nullptr;
  QRhiGraphicsPipeline* line_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* triangle_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* point_pipeline_ = nullptr;
  QRhiGraphicsPipeline* cube_pipeline_ = nullptr;
  QRhiGraphicsPipeline* pose_pipeline_ = nullptr;
  QRhiGraphicsPipeline* occupancy_pipeline_ = nullptr;
  QRhiGraphicsPipeline* voxel_pipeline_ = nullptr;
  QRhiGraphicsPipeline* marker_triangle_pipeline_ = nullptr;
  QRhiGraphicsPipeline* marker_triangle_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* marker_triangle_cull_pipeline_ = nullptr;
  QRhiGraphicsPipeline* marker_triangle_cull_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* marker_line_pipeline_ = nullptr;
  QRhiGraphicsPipeline* marker_line_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* model_triangle_pipeline_ = nullptr;
  QRhiGraphicsPipeline* model_triangle_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* model_line_pipeline_ = nullptr;
  QRhiGraphicsPipeline* model_line_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_line_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_annotation_line_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_triangle_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_line_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_triangle_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_point_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_cube_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_pose_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_occupancy_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_voxel_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_marker_triangle_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_marker_triangle_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_marker_triangle_cull_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_marker_triangle_cull_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_marker_line_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_marker_line_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_model_triangle_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_model_triangle_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_model_line_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_model_line_no_depth_pipeline_ = nullptr;
  QRhiGraphicsPipeline* hdr_grid_shadow_pipeline_ = nullptr;
  QRhiBuffer* point_layout_uniform_buffer_ = nullptr;
  QRhiShaderResourceBindings* point_layout_shader_resources_ = nullptr;
  QRhiBuffer* cube_vertex_buffer_ = nullptr;
  QRhiBuffer* cube_index_buffer_ = nullptr;
  QRhiBuffer* pose_layout_uniform_buffer_ = nullptr;
  QRhiShaderResourceBindings* pose_layout_shader_resources_ = nullptr;
  QRhiBuffer* pose_vertex_buffer_ = nullptr;
  QRhiBuffer* pose_index_buffer_ = nullptr;
  QRhiBuffer* occupancy_layout_uniform_buffer_ = nullptr;
  QRhiShaderResourceBindings* occupancy_layout_shader_resources_ = nullptr;
  QRhiTexture* occupancy_layout_texture_ = nullptr;
  QRhiSampler* occupancy_sampler_ = nullptr;
  QRhiBuffer* occupancy_quad_buffer_ = nullptr;
  QRhiBuffer* voxel_layout_uniform_buffer_ = nullptr;
  QRhiShaderResourceBindings* voxel_layout_shader_resources_ = nullptr;
  QRhiTexture* voxel_layout_texture_ = nullptr;
  QRhiSampler* voxel_sampler_ = nullptr;
  QRhiTexture* colormap_texture_ = nullptr;
  QRhiSampler* colormap_sampler_ = nullptr;
  QRhiBuffer* marker_instance_buffer_ = nullptr;
  QRhiBuffer* model_uniform_buffer_ = nullptr;
  QRhiBuffer* model_instance_buffer_ = nullptr;
  QRhiTexture* model_white_texture_ = nullptr;
  QRhiSampler* model_sampler_ = nullptr;
  QRhiShaderResourceBindings* model_layout_shader_resources_ = nullptr;
  QRhiTexture* shadow_texture_ = nullptr;
  QRhiSampler* shadow_sampler_ = nullptr;
  QRhiTextureRenderTarget* shadow_render_target_ = nullptr;
  QRhiRenderPassDescriptor* shadow_render_pass_descriptor_ = nullptr;
  QRhiBuffer* shadow_uniform_buffer_ = nullptr;
  QRhiBuffer* shadow_instance_buffer_ = nullptr;
  QRhiShaderResourceBindings* shadow_shader_resources_ = nullptr;
  QRhiShaderResourceBindings* grid_shadow_shader_resources_ = nullptr;
  QRhiGraphicsPipeline* shadow_pipeline_ = nullptr;
  QRhiGraphicsPipeline* grid_shadow_pipeline_ = nullptr;
  QRhiRenderBuffer* hdr_msaa_color_ = nullptr;
  QRhiRenderBuffer* hdr_msaa_depth_ = nullptr;
  QRhiTexture* hdr_resolve_color_ = nullptr;
  QRhiTexture* hdr_resolve_depth_ = nullptr;
  QRhiTexture* hdr_resolve_coverage_ = nullptr;
  QRhiTextureRenderTarget* hdr_render_target_ = nullptr;
  QRhiTextureRenderTarget* hdr_depth_render_target_ = nullptr;
  QRhiTextureRenderTarget* ssao_raw_render_target_ = nullptr;
  QRhiTextureRenderTarget* edl_mesh_mask_render_target_ = nullptr;
  QRhiRenderPassDescriptor* hdr_render_pass_descriptor_ = nullptr;
  QRhiRenderPassDescriptor* hdr_depth_render_pass_descriptor_ = nullptr;
  QRhiRenderPassDescriptor* ssao_render_pass_descriptor_ = nullptr;
  QRhiRenderPassDescriptor* edl_mesh_mask_render_pass_descriptor_ = nullptr;
  QRhiSampler* hdr_color_sampler_ = nullptr;
  QRhiSampler* hdr_coverage_sampler_ = nullptr;
  QRhiBuffer* composite_uniform_buffer_ = nullptr;
  QRhiBuffer* ssao_uniform_buffer_ = nullptr;
  QRhiShaderResourceBindings* present_shader_resources_ = nullptr;
  QRhiShaderResourceBindings* ssao_shader_resources_ = nullptr;
  QRhiGraphicsPipeline* present_pipeline_ = nullptr;
  QRhiGraphicsPipeline* ssao_pipeline_ = nullptr;
  QRhiGraphicsPipeline* edl_mesh_mask_triangle_pipeline_ = nullptr;
  QRhiGraphicsPipeline* edl_mesh_mask_line_pipeline_ = nullptr;
  std::unordered_map<QRhiGraphicsPipeline*, QRhiGraphicsPipeline*> hdr_depth_pipelines_;
  std::array<MarkerMeshGpu, 5> marker_meshes_{};
  bool colormap_upload_pending_ = false;
  bool cube_upload_pending_ = false;
  bool pose_mesh_upload_pending_ = false;
  bool occupancy_quad_upload_pending_ = false;
  bool marker_mesh_upload_pending_ = false;
  bool model_white_upload_pending_ = false;
  quint32 pose_index_count_ = 0;
  quint32 line_buffer_capacity_ = 0;
  quint32 triangle_buffer_capacity_ = 0;
  quint32 marker_instance_buffer_capacity_ = 0;
  quint32 model_instance_buffer_capacity_ = 0;
  quint32 shadow_instance_buffer_capacity_ = 0;
  std::vector<Vertex> line_vertices_;
  std::vector<Vertex> triangle_vertices_;
  std::vector<MarkerInstance> marker_instances_;
  std::vector<ModelInstance> model_instances_;
  std::vector<ShadowInstance> shadow_instances_;
  quint32 grid_line_vertex_count_ = 0;
  quint32 grid_triangle_vertex_count_ = 0;
  int last_line_vertex_count_ = 0;
  int last_triangle_vertex_count_ = 0;
  int last_resolved_frame_count_ = 0;
  std::vector<OrderedLayerEntry> ordered_layers_;
  std::vector<std::uint32_t> last_submitted_layer_ids_;

  std::vector<WasmPointRenderable*> point_layers_;
  std::unordered_map<WasmPointRenderable*, PointLayerGpu> point_layer_gpu_;
  std::unordered_set<WasmPointRenderable*> point_layers_pending_fit_;
  int last_point_vertex_count_ = 0;
  int last_point_layer_count_ = 0;
  int last_cube_layer_count_ = 0;
  int last_cube_instance_count_ = 0;
  int camera_fit_count_ = 0;
  AABB last_point_bounds_;
  int last_depth_vertex_count_ = 0;
  int last_depth_layer_count_ = 0;
  AABB last_depth_bounds_;

  std::vector<WasmPosesInFrameLayer*> pose_layers_;
  std::unordered_map<WasmPosesInFrameLayer*, PoseLayerGpu> pose_layer_gpu_;
  std::unordered_set<WasmPosesInFrameLayer*> pose_layers_pending_fit_;
  int last_pose_layer_count_ = 0;
  int last_pose_arm_count_ = 0;
  // TF "Frames" gizmos drawn as solid instanced arrow triads (the desktop look),
  // reusing the pose arrow mesh/pipeline. Each frame's fixed-frame transform is
  // baked into the instance model (uniform fixed_from_source is identity), so
  // the GPU resources reuse the pose PoseLayerGpu shape.
  std::vector<PoseTriadInstance> tf_triad_instances_;
  PoseLayerGpu tf_triad_gpu_;
  AABB last_pose_bounds_;

  std::vector<WasmOccupancyGridLayer*> occupancy_layers_;
  std::unordered_map<WasmOccupancyGridLayer*, OccupancyLayerGpu> occupancy_layer_gpu_;
  std::unordered_set<WasmOccupancyGridLayer*> occupancy_layers_pending_fit_;
  int last_occupancy_layer_count_ = 0;
  int last_occupancy_cell_count_ = 0;
  int occupancy_full_upload_count_ = 0;
  int occupancy_partial_upload_count_ = 0;
  AABB last_occupancy_bounds_;

  std::vector<WasmVoxelGridLayer*> voxel_layers_;
  std::unordered_map<WasmVoxelGridLayer*, VoxelLayerGpu> voxel_layer_gpu_;
  std::unordered_set<WasmVoxelGridLayer*> voxel_layers_pending_fit_;
  int last_voxel_layer_count_ = 0;
  int last_voxel_count_ = 0;
  int voxel_full_upload_count_ = 0;
  int max_3d_texture_size_ = 0;
  AABB last_voxel_bounds_;

  std::vector<WasmSceneEntitiesLayer*> marker_layers_;
  std::unordered_set<WasmSceneEntitiesLayer*> marker_layers_pending_fit_;
  int last_marker_layer_count_ = 0;
  int last_marker_instance_count_ = 0;
  int last_marker_stream_vertex_count_ = 0;
  AABB last_marker_bounds_;
  std::vector<WasmModelRenderable*> model_layers_;
  std::unordered_map<WasmModelRenderable*, ModelLayerGpu> model_layer_gpu_;
  int last_model_layer_count_ = 0;
  int last_model_draw_count_ = 0;
  int last_model_triangle_count_ = 0;
  std::array<int, 5> last_model_texture_slot_counts_{};
  AABB last_model_bounds_;
  bool last_shadow_fit_valid_ = false;
  int shadow_map_size_ = 0;
  int last_shadow_draw_count_ = 0;
  int last_shadow_triangle_count_ = 0;
  AABB last_shadow_bounds_;
  // Last shadow failure already forwarded to the caster layers, so a persistent
  // error is noted once instead of re-marked every frame.
  QString noted_shadow_failure_;
  QSize hdr_render_size_;
  std::uint64_t hdr_allocated_bytes_ = 0;
  QString rendering_warning_;

  // Bookkeeping every optional quality stage carries: whether it ran last
  // frame, the acceptance-test fallback override, the size-keyed failure latch
  // (a resize re-probes — a failure never blocks the stage for the QRhi's whole
  // lifetime), and the resource generation the probes report.
  struct QualityStageState {
    bool last_active = false;
    bool force_fallback_for_test = false;
    QSize rejected_size;
    QString capability_error;
    std::uint64_t resource_generation = 0;
    // True while the stage must stay down for this output size: a failure is
    // latched only against the size it happened at.
    [[nodiscard]] bool blockedFor(const QSize& size) const {
      return !capability_error.isEmpty() && rejected_size == size;
    }
  };
  QualityStageState shadow_stage_;
  QualityStageState hdr_stage_;
  QualityStageState ssao_stage_;
  QualityStageState edl_stage_;

  QPoint last_mouse_position_;
  Qt::MouseButton active_button_ = Qt::NoButton;
  bool camera_gesture_changed_ = false;
};

}  // namespace pj::scene3d
