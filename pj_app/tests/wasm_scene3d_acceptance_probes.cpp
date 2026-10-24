// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "wasm_scene3d_acceptance_probes.h"

#include <emscripten.h>

#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QUrl>
#include <algorithm>
#include <cstdint>
#include <vector>

#include "MainWindow.h"
#include "passes/marker_rhi_pass.h"
#include "passes/model_rhi_pass.h"
#include "passes/occupancy_rhi_pass.h"
#include "passes/point_rhi_pass.h"
#include "passes/pose_rhi_pass.h"
#include "passes/trail_rhi_pass.h"
#include "passes/voxel_rhi_pass.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/layers/depth_cloud_layer.h"
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"
#include "pj_scene3d_widgets/layers/pointcloud_layer.h"
#include "pj_scene3d_widgets/layers/poses_in_frame_layer.h"
#include "pj_scene3d_widgets/layers/robot_model_layer.h"
#include "pj_scene3d_widgets/layers/scene_entities_layer.h"
#include "pj_scene3d_widgets/layers/voxel_grid_layer.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_widgets/Fnv1a.h"
#include "pj_widgets/LayerListView.h"
#include "pj_widgets/RealSlider.h"
#include "scene_view_test_access.h"
#include "scene_view_widget_rhi_quality_p.h"
#include "ui/Scene3DConfigPanel.h"

using namespace Qt::StringLiterals;

namespace {

using pj::scene3d::SceneViewTestAccess;

struct ObservedView {
  quint64 identity = 0;
  QPointer<pj::scene3d::SceneViewWidget> view;
};

QRect geometryRelativeTo(PJ::MainWindow* main_window, QWidget* control) {
  if (control == nullptr) {
    return {-1, -1, 0, 0};
  }
  const QPoint position = main_window->isAncestorOf(control)
                              ? control->mapTo(main_window, QPoint(0, 0))
                              : main_window->mapFromGlobal(control->mapToGlobal(QPoint(0, 0)));
  return {position, control->size()};
}

std::pair<quint64, quint64> framebufferDigest(const QImage& image) {
  quint64 hash = PJ::kFnv1aOffsetBasis;
  quint64 rgb_sum = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QRgb pixel = image.pixel(x, y);
      hash = PJ::fnv1aMix(hash, pixel);
      rgb_sum += static_cast<quint64>(qRed(pixel) + qGreen(pixel) + qBlue(pixel));
    }
  }
  return {hash, rgb_sum};
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_scene3d_foundation() {
  static std::vector<ObservedView> observed_views;
  static quint64 next_identity = 1;
  static quint64 sequence = 0;

  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    if (main_window == nullptr) {
      continue;
    }

    QList<PJ::Scene3DDockWidget*> docks;
    for (auto* dock : main_window->findChildren<PJ::Scene3DDockWidget*>()) {
      if (dock != nullptr && dock->isVisibleTo(main_window)) {
        docks.push_back(dock);
      }
    }
    std::sort(docks.begin(), docks.end(), [main_window](const auto* lhs, const auto* rhs) {
      const QPoint lhs_position = lhs->mapTo(main_window, QPoint(0, 0));
      const QPoint rhs_position = rhs->mapTo(main_window, QPoint(0, 0));
      return lhs_position.x() != rhs_position.x() ? lhs_position.x() < rhs_position.x()
                                                  : lhs_position.y() < rhs_position.y();
    });

    const quint64 current_sequence = ++sequence;
    auto* slider = main_window->findChild<PJ::RealSlider*>(u"timeSlider"_s);
    QPushButton* right_panel_button = nullptr;
    for (auto* button : main_window->findChildren<QPushButton*>()) {
      if (button->toolTip() == QObject::tr("Toggle right panel")) {
        right_panel_button = button;
        break;
      }
    }
    const QRect right_toggle_geometry = geometryRelativeTo(main_window, right_panel_button);
    auto* right_panel = main_window->findChild<QWidget*>(u"localToolbarWidget"_s);
    const QRect slider_geometry = geometryRelativeTo(main_window, slider);
    qInfo(
        "PJ_WASM_SCENE3D_FOUNDATION_SUMMARY sequence=%llu docks=%lld slider=%d,%d,%dx%d "
        "value=%.17g range=%.17g,%.17g enabled=%d window=%dx%d",
        static_cast<unsigned long long>(current_sequence), static_cast<long long>(docks.size()), slider_geometry.x(),
        slider_geometry.y(), slider_geometry.width(), slider_geometry.height(),
        slider != nullptr ? slider->getValue() : 0.0, slider != nullptr ? slider->getMinimum() : 0.0,
        slider != nullptr ? slider->getMaximum() : 0.0, slider != nullptr && slider->isEnabled() ? 1 : 0,
        main_window->width(), main_window->height());

    for (qsizetype index = 0; index < docks.size(); ++index) {
      auto* dock = docks[index];
      auto* view = dock->sceneView();
      quint64 identity = 0;
      if (view != nullptr) {
        const auto observed = std::find_if(
            observed_views.cbegin(), observed_views.cend(), [view](const auto& entry) { return entry.view == view; });
        if (observed != observed_views.cend()) {
          identity = observed->identity;
        } else {
          identity = next_identity++;
          observed_views.push_back({identity, view});
        }
      }

      QStringList frame_names;
      for (const auto& frame : dock->availableFrames()) {
        frame_names.push_back(QString::fromStdString(frame.name));
      }
      const QByteArray encoded_frames = QUrl::toPercentEncoding(frame_names.join(u','));
      const QByteArray encoded_fixed = QUrl::toPercentEncoding(dock->currentFixedFrame());
      const QByteArray encoded_follow = QUrl::toPercentEncoding(dock->currentFollowFrame());
      auto* fixed_combo = dock->findChild<QComboBox*>(u"scene3dFixedFrameCombo"_s);
      auto* camera_combo = dock->findChild<QComboBox*>(u"cameraModelCombo"_s);
      auto* home_button = dock->findChild<QToolButton*>(u"cameraHomeButton"_s);
      const QRect view_geometry = geometryRelativeTo(main_window, view);
      const QRect fixed_geometry = geometryRelativeTo(main_window, fixed_combo);
      const QRect camera_geometry = geometryRelativeTo(main_window, camera_combo);
      const QRect home_geometry = geometryRelativeTo(main_window, home_button);
      const auto camera = view != nullptr ? view->camera().state() : pj::scene3d::CameraState{};
      const QImage framebuffer = view != nullptr ? view->grabFramebuffer() : QImage{};
      const auto [frame_hash, rgb_sum] = framebufferDigest(framebuffer);

      qInfo(
          "PJ_WASM_SCENE3D_FOUNDATION_DOCK sequence=%llu index=%lld identity=%llu "
          "view=%d,%d,%dx%d visible=%d frames=%s fixed=%s auto=%d follow=%s model=%d "
          "camera=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g vertices=%d,%d resolved=%d "
          "fixed_control=%d,%d,%dx%d camera_control=%d,%d,%dx%d home=%d,%d,%dx%d "
          "frame_hash=%llu rgb_sum=%llu framebuffer=%dx%d",
          static_cast<unsigned long long>(current_sequence), static_cast<long long>(index),
          static_cast<unsigned long long>(identity), view_geometry.x(), view_geometry.y(), view_geometry.width(),
          view_geometry.height(), view != nullptr && view->isVisibleTo(main_window) ? 1 : 0, encoded_frames.constData(),
          encoded_fixed.constData(), dock->isAutoRootMode() ? 1 : 0, encoded_follow.constData(),
          view != nullptr ? static_cast<int>(view->cameraModel()) : -1, static_cast<double>(camera.focal.x),
          static_cast<double>(camera.focal.y), static_cast<double>(camera.focal.z), static_cast<double>(camera.radius),
          static_cast<double>(camera.azimuth), static_cast<double>(camera.elevation),
          static_cast<double>(camera.ortho_scale),
          view != nullptr ? SceneViewTestAccess::lastLineVertexCount(*view) : 0,
          view != nullptr ? SceneViewTestAccess::lastTriangleVertexCount(*view) : 0,
          view != nullptr ? SceneViewTestAccess::lastResolvedFrameCount(*view) : 0, fixed_geometry.x(),
          fixed_geometry.y(), fixed_geometry.width(), fixed_geometry.height(), camera_geometry.x(), camera_geometry.y(),
          camera_geometry.width(), camera_geometry.height(), home_geometry.x(), home_geometry.y(),
          home_geometry.width(), home_geometry.height(), static_cast<unsigned long long>(frame_hash),
          static_cast<unsigned long long>(rgb_sum), framebuffer.width(), framebuffer.height());

      int point_layers = 0;
      int compressed_layers = 0;
      int compressed_decoding = 0;
      quint64 compressed_starts = 0;
      quint64 compressed_completed = 0;
      quint64 compressed_off_main = 0;
      int depth_layers = 0;
      int depth_decoding = 0;
      quint64 depth_starts = 0;
      quint64 depth_completed = 0;
      quint64 depth_off_main = 0;
      int pose_layers = 0;
      int occupancy_layers = 0;
      int voxel_layers = 0;
      int marker_layers = 0;
      int marker_decoding = 0;
      quint64 marker_starts = 0;
      quint64 marker_completed = 0;
      quint64 marker_off_main = 0;
      quint64 marker_cubes = 0;
      quint64 marker_spheres = 0;
      quint64 marker_cylinders = 0;
      quint64 marker_arrows = 0;
      quint64 marker_axes = 0;
      quint64 marker_lines = 0;
      quint64 marker_triangles = 0;
      quint64 marker_skipped_text = 0;
      quint64 marker_skipped_models = 0;
      quint64 marker_skipped_invalid = 0;
      quint64 ready_models = 0;
      quint64 live_models = 0;
      quint64 model_bytes = 0;
      int robot_layers = 0;
      quint64 robot_meshes = 0;
      quint64 robot_draws = 0;
      quint64 robot_visuals = 0;
      quint64 robot_collisions = 0;
      quint64 robot_placeholders = 0;
      quint64 robot_bridges = 0;
      int robot_source_type = -1;
      int robot_display_mode = -1;
      quint64 robot_revision = 0;
      QString robot_source;
      QStringList robot_statuses;
      QStringList warnings;
      QStringList ui_order;
      for (const PJ::SceneLayerInfo& info : dock->layers()) {
        ui_order.push_back(QString::number(info.topic_id.id));
        PJ::ISceneLayer* layer = dock->layerFor(info.topic_id);
        if (auto* point = dynamic_cast<pj::scene3d::PointCloudLayer*>(layer)) {
          ++point_layers;
          if (point->isCompressedForTest()) {
            ++compressed_layers;
            compressed_decoding += point->decodeInFlightForTest() ? 1 : 0;
            compressed_starts += static_cast<quint64>(point->startDecodeCountForTest());
            compressed_completed += static_cast<quint64>(point->decodeCompletionsForTest());
            compressed_off_main += static_cast<quint64>(point->decodeOffMainCompletionsForTest());
          }
          if (!point->statusWarning().isEmpty()) {
            warnings.push_back(point->statusWarning());
          }
        } else if (auto* depth = dynamic_cast<pj::scene3d::DepthCloudLayer*>(layer)) {
          ++depth_layers;
          depth_decoding += depth->decodeInFlightForTest() ? 1 : 0;
          depth_starts += depth->decodeStartsForTest();
          depth_completed += depth->decodeCompletionsForTest();
          depth_off_main += depth->decodeOffMainCompletionsForTest();
          if (!depth->statusWarning().isEmpty()) {
            warnings.push_back(depth->statusWarning());
          }
        } else if (auto* poses = dynamic_cast<pj::scene3d::PosesInFrameLayer*>(layer)) {
          ++pose_layers;
          if (!poses->statusWarning().isEmpty()) {
            warnings.push_back(poses->statusWarning());
          }
        } else if (auto* occupancy = dynamic_cast<pj::scene3d::OccupancyGridLayer*>(layer)) {
          ++occupancy_layers;
          if (!occupancy->statusWarning().isEmpty()) {
            warnings.push_back(occupancy->statusWarning());
          }
        } else if (auto* voxels = dynamic_cast<pj::scene3d::VoxelGridLayer*>(layer)) {
          ++voxel_layers;
          if (!voxels->statusWarning().isEmpty()) {
            warnings.push_back(voxels->statusWarning());
          }
        } else if (auto* markers = dynamic_cast<pj::scene3d::SceneEntitiesLayer*>(layer)) {
          ++marker_layers;
          marker_decoding += markers->decodeInFlightForTest() ? 1 : 0;
          marker_starts += markers->decodeStartsForTest();
          marker_completed += markers->decodeCompletionsForTest();
          marker_off_main += markers->decodeOffMainCompletionsForTest();
          const auto& geometry = markers->geometry();
          marker_cubes += geometry.cubes.size();
          marker_spheres += geometry.spheres.size();
          marker_cylinders += geometry.cylinders.size();
          marker_arrows += geometry.arrows.size();
          marker_axes += geometry.axes.size();
          marker_lines += geometry.line_vertices;
          marker_triangles += geometry.triangle_vertices;
          marker_skipped_text += geometry.skipped_texts;
          marker_skipped_models += geometry.skipped_models;
          marker_skipped_invalid += geometry.skipped_invalid;
          ready_models += markers->readyModelCountForTest();
          live_models += markers->liveModelCountForTest();
          model_bytes += markers->modelRetainedBytes();
          if (!markers->statusWarning().isEmpty()) {
            warnings.push_back(markers->statusWarning());
          }
        } else if (auto* robot = dynamic_cast<pj::scene3d::RobotModelLayer*>(layer)) {
          ++robot_layers;
          robot_meshes += robot->modelMeshes().size();
          robot_draws += robot->modelDrawCalls().size();
          for (const auto& draw : robot->modelDrawCalls()) {
            robot_visuals += draw.group == pj::scene3d::ModelDrawGroup::kVisual ? 1U : 0U;
            robot_collisions += draw.group == pj::scene3d::ModelDrawGroup::kCollision ? 1U : 0U;
            robot_placeholders += draw.mesh_key == "robot:placeholder" ? 1U : 0U;
          }
          robot_bridges += robot->fixedJointBridgeCount();
          if (robot_source_type < 0) {
            robot_source_type = static_cast<int>(robot->sourceType());
            robot_display_mode = static_cast<int>(robot->displayMode());
            robot_revision = robot->modelRevision();
            robot_source = robot->sourceValue();
          }
          robot_statuses.push_back(robot->statusText());
        }
      }

      QStringList submitted_order;
      if (view != nullptr) {
        for (const std::uint32_t topic_id : SceneViewTestAccess::lastSubmittedLayerIds(*view)) {
          submitted_order.push_back(QString::number(topic_id));
        }
      }
      auto* config_panel = main_window->findChild<PJ::Scene3DConfigPanel*>();
      PJ::LayerListView* layer_list = nullptr;
      if (config_panel != nullptr && config_panel->boundDockForTest() == dock) {
        layer_list = config_panel->layerListForTest();
      }
      QStringList row_centers;
      if (auto* list = layer_list != nullptr ? layer_list->findChild<QListWidget*>() : nullptr) {
        const std::vector<qint64> order = layer_list->order();
        for (int row = 0; row < list->count() && static_cast<std::size_t>(row) < order.size(); ++row) {
          const QPoint center =
              main_window->mapFromGlobal(list->viewport()->mapToGlobal(list->visualItemRect(list->item(row)).center()));
          row_centers.push_back(
              QString::number(order[static_cast<std::size_t>(row)]) + u':' + QString::number(center.x()) + u':' +
              QString::number(center.y()));
        }
      }
      const QRect list_geometry = geometryRelativeTo(main_window, layer_list);
      const QByteArray encoded_warnings = QUrl::toPercentEncoding(warnings.join(u'|'));
      qInfo(
          "PJ_WASM_SCENE3D_DATA sequence=%llu index=%lld "
          "points=%d,%d,%d,%llu,%llu,%llu,%d compressed=%d "
          "depth=%d,%d,%llu,%llu,%llu,%d poses=%d,%d occupancy=%d,%d,%d,%d voxels=%d,%d,%d,%d trails=%d,%d "
          "order=%s submitted=%s list=%d,%d,%dx%d rows=%s toggle=%d,%d,%dx%d right_visible=%d warnings=%s",
          static_cast<unsigned long long>(current_sequence), static_cast<long long>(index), point_layers,
          view != nullptr ? SceneViewTestAccess::pointPass(*view).lastPointLayerCount() : 0,
          view != nullptr ? SceneViewTestAccess::pointPass(*view).lastPointVertexCount() : 0,
          static_cast<unsigned long long>(compressed_starts), static_cast<unsigned long long>(compressed_completed),
          static_cast<unsigned long long>(compressed_off_main), compressed_decoding, compressed_layers, depth_layers,
          depth_decoding, static_cast<unsigned long long>(depth_starts),
          static_cast<unsigned long long>(depth_completed), static_cast<unsigned long long>(depth_off_main),
          view != nullptr ? SceneViewTestAccess::pointPass(*view).lastDepthVertexCount() : 0, pose_layers,
          view != nullptr ? SceneViewTestAccess::posePass(*view).lastArmCount() : 0, occupancy_layers,
          view != nullptr ? SceneViewTestAccess::occupancyPass(*view).lastCellCount() : 0,
          view != nullptr ? SceneViewTestAccess::occupancyPass(*view).fullUploadCount() : 0,
          view != nullptr ? SceneViewTestAccess::occupancyPass(*view).partialUploadCount() : 0, voxel_layers,
          view != nullptr ? SceneViewTestAccess::voxelPass(*view).lastVoxelCount() : 0,
          view != nullptr ? SceneViewTestAccess::voxelPass(*view).fullUploadCount() : 0,
          view != nullptr ? SceneViewTestAccess::max3DTextureSize(*view) : 0,
          view != nullptr ? SceneViewTestAccess::trailPass(*view).lastTrailCount() : 0,
          view != nullptr ? SceneViewTestAccess::trailPass(*view).lastVertexCount() : 0,
          QUrl::toPercentEncoding(ui_order.join(u',')).constData(),
          QUrl::toPercentEncoding(submitted_order.join(u',')).constData(), list_geometry.x(), list_geometry.y(),
          list_geometry.width(), list_geometry.height(), QUrl::toPercentEncoding(row_centers.join(u',')).constData(),
          right_toggle_geometry.x(), right_toggle_geometry.y(), right_toggle_geometry.width(),
          right_toggle_geometry.height(), right_panel != nullptr && right_panel->isVisibleTo(main_window) ? 1 : 0,
          encoded_warnings.constData());

      const auto model_maps =
          view != nullptr ? SceneViewTestAccess::modelPass(*view).lastTextureSlotCounts() : std::array<int, 5>{};
      auto* source_combo = config_panel != nullptr && config_panel->boundDockForTest() == dock
                               ? config_panel->findChild<QComboBox*>(u"scene3dModelSource"_s)
                               : nullptr;
      qInfo(
          "PJ_WASM_SCENE3D_MODELS sequence=%llu index=%lld "
          "markers=%d,%d,%llu,%llu,%llu,%d,%d,%d families=%llu,%llu,%llu,%llu,%llu,%llu,%llu "
          "skipped=%llu,%llu,%llu models=%llu,%llu,%llu,%d,%d,%d maps=%d,%d,%d,%d,%d "
          "robots=%d,%llu,%llu,%llu,%llu,%llu,%llu,%d,%d,%llu source=%s status=%s source_index=%d",
          static_cast<unsigned long long>(current_sequence), static_cast<long long>(index), marker_layers,
          marker_decoding, static_cast<unsigned long long>(marker_starts),
          static_cast<unsigned long long>(marker_completed), static_cast<unsigned long long>(marker_off_main),
          view != nullptr ? SceneViewTestAccess::markerPass(*view).lastLayerCount() : 0,
          view != nullptr ? SceneViewTestAccess::markerPass(*view).lastInstanceCount() : 0,
          view != nullptr ? SceneViewTestAccess::markerPass(*view).lastStreamVertexCount() : 0,
          static_cast<unsigned long long>(marker_cubes), static_cast<unsigned long long>(marker_spheres),
          static_cast<unsigned long long>(marker_cylinders), static_cast<unsigned long long>(marker_arrows),
          static_cast<unsigned long long>(marker_axes), static_cast<unsigned long long>(marker_lines),
          static_cast<unsigned long long>(marker_triangles), static_cast<unsigned long long>(marker_skipped_text),
          static_cast<unsigned long long>(marker_skipped_models),
          static_cast<unsigned long long>(marker_skipped_invalid), static_cast<unsigned long long>(ready_models),
          static_cast<unsigned long long>(live_models), static_cast<unsigned long long>(model_bytes),
          view != nullptr ? SceneViewTestAccess::modelPass(*view).lastLayerCount() : 0,
          view != nullptr ? SceneViewTestAccess::modelPass(*view).lastDrawCount() : 0,
          view != nullptr ? SceneViewTestAccess::modelPass(*view).lastTriangleCount() : 0, model_maps[0], model_maps[1],
          model_maps[2], model_maps[3], model_maps[4], robot_layers, static_cast<unsigned long long>(robot_meshes),
          static_cast<unsigned long long>(robot_draws), static_cast<unsigned long long>(robot_visuals),
          static_cast<unsigned long long>(robot_collisions), static_cast<unsigned long long>(robot_placeholders),
          static_cast<unsigned long long>(robot_bridges), robot_source_type, robot_display_mode,
          static_cast<unsigned long long>(robot_revision), QUrl::toPercentEncoding(robot_source).constData(),
          QUrl::toPercentEncoding(robot_statuses.join(u'|')).constData(),
          source_combo != nullptr ? source_combo->currentIndex() : -1);

      const QSize hdr_size = view != nullptr ? SceneViewTestAccess::hdrRenderSize(*view) : QSize{};
      const QByteArray quality_errors = QUrl::toPercentEncoding(
          (view != nullptr ? SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kHdr).capability_error
                           : QString{}) +
          u'|' +
          (view != nullptr ? SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kSsao).capability_error
                           : QString{}) +
          u'|' +
          (view != nullptr ? SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kEdl).capability_error
                           : QString{}) +
          u'|' +
          (view != nullptr ? SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kShadow).capability_error
                           : QString{}));
      qInfo(
          "PJ_WASM_SCENE3D_QUALITY sequence=%llu index=%lld "
          "quality=%d,%d,%d,%d,%llu,%llu,%d,%d,%llu,%d,%d,%llu,%d,%d,%d,%d,%d,%d,%llu errors=%s",
          static_cast<unsigned long long>(current_sequence), static_cast<long long>(index),
          view != nullptr && SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kHdr).last_active ? 1 : 0,
          view != nullptr && SceneViewTestAccess::hdrResourcesReady(*view) ? 1 : 0, hdr_size.width(), hdr_size.height(),
          static_cast<unsigned long long>(view != nullptr ? SceneViewTestAccess::hdrAllocatedBytes(*view) : 0U),
          static_cast<unsigned long long>(
              view != nullptr ? SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kHdr).resource_generation
                              : 0U),
          view != nullptr && SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kSsao).last_active ? 1 : 0,
          view != nullptr && SceneViewTestAccess::ssaoResourcesReady(*view) ? 1 : 0,
          static_cast<unsigned long long>(
              view != nullptr ? SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kSsao).resource_generation
                              : 0U),
          view != nullptr && SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kEdl).last_active ? 1 : 0,
          view != nullptr && SceneViewTestAccess::edlResourcesReady(*view) ? 1 : 0,
          static_cast<unsigned long long>(
              view != nullptr ? SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kEdl).resource_generation
                              : 0U),
          view != nullptr && SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kShadow).last_active ? 1 : 0,
          view != nullptr && SceneViewTestAccess::shadowResourcesReady(*view) ? 1 : 0,
          view != nullptr && SceneViewTestAccess::lastShadowFitValid(*view) ? 1 : 0,
          view != nullptr ? SceneViewTestAccess::shadowMapSize(*view) : 0,
          view != nullptr ? SceneViewTestAccess::modelPass(*view).lastShadowDrawCount() : 0,
          view != nullptr ? SceneViewTestAccess::modelPass(*view).lastShadowTriangleCount() : 0,
          static_cast<unsigned long long>(
              view != nullptr
                  ? SceneViewTestAccess::stage(*view, SceneViewTestAccess::Stage::kShadow).resource_generation
                  : 0U),
          quality_errors.constData());
    }
    return;
  }
  qWarning("PJ_WASM_SCENE3D_FOUNDATION_FAILED no main window");
}

void setScene3dFallback(int mode, bool enabled) {
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    for (auto* dock : widget->findChildren<PJ::Scene3DDockWidget*>()) {
      auto* view = dock != nullptr ? dock->sceneView() : nullptr;
      if (view == nullptr) {
        continue;
      }
      if (mode == 0) {
        SceneViewTestAccess::forceStageFallback(*view, SceneViewTestAccess::Stage::kHdr, enabled);
      } else if (mode == 1) {
        SceneViewTestAccess::forceStageFallback(*view, SceneViewTestAccess::Stage::kSsao, enabled);
      } else if (mode == 3) {
        SceneViewTestAccess::forceStageFallback(*view, SceneViewTestAccess::Stage::kShadow, enabled);
      } else {
        SceneViewTestAccess::forceStageFallback(*view, SceneViewTestAccess::Stage::kEdl, enabled);
      }
    }
  }
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_force_scene3d_fallback(int mode, int enabled) {
  setScene3dFallback(mode, enabled != 0);
}

// clang-format off
EM_JS(void, installScene3dFoundationProbe, (), {
  globalThis.pjWasmReportScene3DFoundationProbe = () =>
      Module._pj_wasm_test_report_scene3d_foundation();
  globalThis.pjWasmForceScene3DFallbackProbe = (mode, enabled) =>
      Module._pj_wasm_test_force_scene3d_fallback(mode, enabled ? 1 : 0);
});
// clang-format on

}  // namespace

namespace PJ {

void installWasmScene3dAcceptanceProbes() {
  installScene3dFoundationProbe();
}

}  // namespace PJ
