// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/builtin/grid_map.hpp"
#include "pj_datastore/object_store.hpp"  // PJ::ObjectTopicId, PJ::SequentialUID
#include "pj_runtime/SessionManager.h"    // PJ::DatasetId
#include "pj_scene3d_core/grid_map_view.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_widgets/Colormap.h"

class QFormLayout;
class QWidget;

namespace pj::scene3d {

// Persisted `elevation_field` token for "draw a flat plane": angle brackets keep it
// distinct from any real channel name. An empty request means "auto-pick".
inline constexpr std::string_view kGridMapElevationNone = "<none>";

// Scene3DLayer for an sdk::GridMap topic: the sample at-or-before the tracker time
// is resolved (parser or canonical codec), validated, de-interleaved into an
// elevation + a color channel, and staged ONCE per new sample / selector change;
// the view reads the staged grid through prepareForRender()/stagedGrid(). The
// requested field names are kept apart from the effective ones so a persisted
// request is honoured again once its field reappears.
class GridMapLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  GridMapLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~GridMapLayer() override;

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] SceneContributions sceneContributions() override {
    return {.grid_map = this};
  }
  [[nodiscard]] QString sourceFrame() const override;
  [[nodiscard]] uint64_t renderKey(PJ::Timepoint time) const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  void setVisible(bool visible) override;

  [[nodiscard]] std::optional<AABB> worldBounds() const override;

  // QRhi view hook, called once per frame for a visible layer: decodes and packs
  // the sample at the last tracker time when a tracker move or selector change is
  // pending. The result is stagedGrid(); stagedRevision() bumps on every change.
  void prepareForRender();
  // The grid to draw, nullopt when none (before the first sample, a refused one).
  [[nodiscard]] const std::optional<GridMapUpload>& stagedGrid() const {
    return staged_grid_;
  }
  [[nodiscard]] std::uint64_t stagedRevision() const {
    return staged_revision_;
  }
  // The last decoded sample's frame id (empty before the first sample).
  [[nodiscard]] const std::string& renderFrameId() const override {
    return source_frame_;
  }
  [[nodiscard]] std::optional<PJ::Timepoint> renderStamp() const override {
    return render_stamp_;
  }
  // Display params as set (the persisted values).
  [[nodiscard]] PJ::Colormap colormap() const {
    return colormap_;
  }
  [[nodiscard]] bool autoRange() const {
    return auto_range_;
  }
  [[nodiscard]] std::pair<double, double> manualRange() const {
    return {manual_lo_, manual_hi_};
  }
  [[nodiscard]] bool lighting() const {
    return lighting_;
  }
  [[nodiscard]] double opacity() const {
    return opacity_;
  }
  [[nodiscard]] bool visible() const {
    return visible_;
  }

  QWidget* createConfigWidget(QWidget* parent) override;

  // Display params. The two selectors re-pack on the next render; the rest are
  // uniform-only. Empty = auto; kGridMapElevationNone = flat plane.
  void setElevationField(const QString& field_name);
  void setColorField(const QString& field_name);  // empty = follow the elevation field
  void setColormap(PJ::Colormap colormap);
  void setAutoRange(bool on);
  void setManualRange(double lo, double hi);
  void setLighting(bool on);
  void setOpacity(double opacity);

#ifdef PJ_SCENE3D_TEST_HOOKS
  void renderAtForTest(int64_t time_ns) {
    renderAt(time_ns);
  }
  [[nodiscard]] QString resolvedElevationFieldForTest() const {
    return QString::fromStdString(resolved_elevation_);
  }
  [[nodiscard]] QString resolvedColorFieldForTest() const {
    return QString::fromStdString(resolved_color_);
  }
  [[nodiscard]] bool hasGridForTest() const {
    return cached_grid_.has_value();
  }
  [[nodiscard]] bool passHasStagedGridForTest() const {
    return staged_grid_.has_value();
  }
  // An empty upload when nothing is staged (a reset optional holds no arrays).
  [[nodiscard]] const GridMapUpload& stagedGridForTest() const {
    static const GridMapUpload kNone;
    return staged_grid_.has_value() ? *staged_grid_ : kNone;
  }
  [[nodiscard]] int packCountForTest() const {
    return pack_count_;
  }
  [[nodiscard]] const std::vector<std::string>& fieldNamesForTest() const {
    return field_names_;
  }
  [[nodiscard]] std::pair<double, double> manualRangeForTest() const {
    return {manual_lo_, manual_hi_};
  }
#endif

 signals:
  // The scalar field list or the effective picks changed (a new schema, or a
  // request re-resolved): the config panel's selectors rebuild from field_names_.
  void fieldsChanged();

 private:
  // Decode the first sample at attach to learn the source frame + field list.
  bool bootstrap();
  // Fetch the sample at time_ns, pack the selected fields, stage the upload. Cheap
  // no-op when the same sample + selectors are already staged (the scrub path).
  void renderAt(int64_t time_ns);
  // Hand a packed grid to the view (stagedGrid()).
  void stageGrid(GridMapUpload upload);
  void clearStagedGrid();
  // Forget everything learned from the topic's bytes (parse/upload memos, bounds,
  // frame, field list, effective picks, warnings); the persisted requests and
  // display settings survive.
  void resetStreamingState();
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);
  // Resolve the effective elevation / color fields for `grid` from the requests,
  // falling back (with a status warning) when a requested field is missing.
  // Returns false when the grid offers no usable color field.
  bool resolveFields(const PJ::sdk::GridMap& grid);
  void pushDisplayParamsToPass();
  // Bounds change inside renderAt, i.e. during the paint that follows the dock's
  // bounds union, so a changed extent requests another repaint to re-fit the camera.
  void setBounds(std::optional<AABB> bounds);
  // The auto-range toggle plus the two manual-range scrubbers it seeds/enables.
  void buildRangeRows(QFormLayout* form, QWidget* parent);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  QMetaObject::Connection reload_connection_;

  // Parse memo: the canonical GridMap of the store entry `cached_grid_uid_`. Its
  // anchor keeps the cell bytes alive; a selector change re-packs from it without
  // re-transcoding. On the parser route that anchor's control block lives in the
  // plugin DSO, so the binding keepalive is held alongside and declared FIRST:
  // the grid (and its anchor) must be destroyed before the DSO can be unmapped.
  // Empty on the canonical route.
  std::shared_ptr<void> cached_keepalive_;
  PJ::SequentialUID cached_grid_uid_{};
  std::optional<PJ::sdk::GridMap> cached_grid_;
  std::optional<AABB> bounds_;

  // The sample UID staged in the pass. Reset to force a re-pack (selector change,
  // dataset replace, empty tick).
  PJ::SequentialUID uploaded_uid_{};

  std::string source_frame_;
  std::optional<PJ::Timepoint> render_stamp_;  // objectStamp() of the staged sample
  QString fixed_frame_;
  bool tracker_dirty_ = false;
  bool visible_ = true;
  int pack_count_ = 0;

  // Requested (persisted) vs effective field names.
  std::string elevation_request_;
  std::string color_request_;
  std::string resolved_elevation_;  // empty = flat plane
  std::string resolved_color_;
  std::vector<std::string> field_names_;  // scalar fields of the last decoded grid, for the UI
  // resolveFields memo: the full schema (name, offset, datatype, count: a swapped
  // datatype changes the auto-pick) and requests it last resolved, plus the
  // warning it produced (re-applied when the memo hits).
  std::vector<PJ::sdk::PointField> fields_memo_;
  std::optional<std::pair<std::string, std::string>> resolved_requests_;
  QString resolve_warning_;

  PJ::Colormap colormap_ = PJ::Colormap::kTurbo;
  bool auto_range_ = true;
  double manual_lo_ = 0.0;
  double manual_hi_ = 1.0;
  FiniteRange last_auto_range_;  // seeds the manual spinboxes when auto is switched off
  bool lighting_ = true;
  double opacity_ = 1.0;

  std::optional<GridMapUpload> staged_grid_;
  std::uint64_t staged_revision_ = 0;
  std::optional<PJ::Timepoint> requested_time_;  // the last tracker time, drained by prepareForRender()
};

}  // namespace pj::scene3d
