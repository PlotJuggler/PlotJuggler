// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/wasm/poses_in_frame_layer_wasm.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLoggingCategory>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <any>
#include <cmath>
#include <exception>
#include <glm/glm.hpp>
#include <new>
#include <utility>

#include "layers/layer_xml_validation.h"
#include "pj_base/builtin/poses_in_frame.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/resolve_object.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/ToggleSwitch.h"

using namespace Qt::StringLiterals;

namespace pj::scene3d {
namespace {

Q_LOGGING_CATEGORY(lcWasmPoses, "pj.scene3d.wasm.poses_in_frame")

bool finiteInstance(const PoseTriadInstance& instance) {
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      if (!std::isfinite(instance.model[column][row])) {
        return false;
      }
    }
    if (!std::isfinite(instance.color[column])) {
      return false;
    }
  }
  return true;
}

void expandInstanceBounds(AABB& bounds, const PoseTriadInstance& instance) {
  const glm::vec3 origin = glm::vec3(instance.model * glm::vec4(0.0F, 0.0F, 0.0F, 1.0F));
  const glm::vec3 tip = glm::vec3(instance.model * glm::vec4(1.0F, 0.0F, 0.0F, 1.0F));
  // The shared unit arrow's widest section is a 0.10-radius cone. Uniform
  // instance scale makes this conservative in source axes for every rotation.
  const float radius = 0.10F * glm::length(glm::vec3(instance.model[0]));
  const glm::vec3 padding(radius);
  expandAABB(bounds, origin - padding);
  expandAABB(bounds, origin + padding);
  expandAABB(bounds, tip - padding);
  expandAABB(bounds, tip + padding);
}

}  // namespace

WasmPosesInFrameLayer::WasmPosesInFrameLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : PJ::ISceneLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)) {}

WasmPosesInFrameLayer::~WasmPosesInFrameLayer() = default;

PJ::SceneLayerInfo WasmPosesInFrameLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kPosesInFrame,
      .display_name = display_name_,
      .family_name = u"PosesInFrame"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> WasmPosesInFrameLayer::timeRange() const {
  return PJ::liveTopicTimeRange(session_ != nullptr ? &session_->objectStore() : nullptr, topic_id_);
}

bool WasmPosesInFrameLayer::attach(const PJ::SceneLayerContext& context) {
  if (context.session == nullptr) {
    qCWarning(lcWasmPoses) << "attach: session is null";
    return false;
  }
  session_ = context.session;
  if (!session_->parserBindingForObjectTopic(topic_id_) &&
      !hasCanonical3DCodec(PJ::sdk::BuiltinObjectType::kPosesInFrame)) {
    qCWarning(lcWasmPoses) << "attach: no parser or canonical codec for topic" << topic_id_.id;
    session_ = nullptr;
    return false;
  }
  disconnect(reload_connection_);
  reload_connection_ = connect(
      session_, &PJ::SessionManager::datasetAboutToBeReplaced, this,
      &WasmPosesInFrameLayer::onDatasetAboutToBeReplaced);
  if (!bootstrap()) {
    qCDebug(lcWasmPoses) << "attach: first sample is not available yet for topic" << topic_id_.id;
  }
  const PJ::Range<PJ::Timepoint> range = timeRange();
  if (range.max >= range.min) {
    requested_time_ = range.min;
    decode_dirty_ = true;
  }
  return true;
}

void WasmPosesInFrameLayer::detach() {
  disconnect(reload_connection_);
  reload_connection_ = {};
  session_ = nullptr;
  requested_time_.reset();
  budget_rejected_arm_count_.reset();
  decode_dirty_ = false;
  staged_uid_ = {};
  staged_style_revision_ = -1;
  clearGeometry();
  updateSourceFrame({});
  setWarning({});
}

void WasmPosesInFrameLayer::setTrackerTime(PJ::Timepoint time) {
  requested_time_ = time;
  decode_dirty_ = true;
  if (visible_) {
    emit repaintRequested();
  }
}

std::uint64_t WasmPosesInFrameLayer::renderKey(PJ::Timepoint time) const {
  if (session_ == nullptr) {
    return PJ::kNoSampleRenderKey;
  }
  const auto entry = session_->objectStore().latestAt(topic_id_, PJ::toRaw(time));
  return entry.has_value() ? entry->sequential_uid.value : PJ::kNoSampleRenderKey;
}

void WasmPosesInFrameLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  if (!visible_) {
    staged_uid_ = {};
    staged_style_revision_ = -1;
    budget_rejected_arm_count_.reset();
    clearGeometry();
  }
  emit visibilityChanged(visible_);
  emit repaintRequested();
}

bool WasmPosesInFrameLayer::prepareForRender(std::uint64_t remaining_view_arms) {
  if (budget_rejected_arm_count_.has_value()) {
    if (*budget_rejected_arm_count_ > remaining_view_arms) {
      return false;
    }
    budget_rejected_arm_count_.reset();
    staged_uid_ = {};
    staged_style_revision_ = -1;
    decode_dirty_ = requested_time_.has_value();
  }
  if (static_cast<std::uint64_t>(instances_.size()) > remaining_view_arms) {
    budget_rejected_arm_count_ = instances_.size();
    setWarning(tr("Layer needs %1 pose arrows; only %2 remain in the browser view budget")
                   .arg(QString::number(instances_.size()), QString::number(remaining_view_arms)));
    clearGeometry();
    return true;
  }
  if (!visible_ || !decode_dirty_ || !requested_time_.has_value()) {
    return false;
  }
  decode_dirty_ = false;
  const bool changed = decodeAt(*requested_time_);
  if (static_cast<std::uint64_t>(instances_.size()) > remaining_view_arms) {
    budget_rejected_arm_count_ = instances_.size();
    setWarning(tr("Layer needs %1 pose arrows; only %2 remain in the browser view budget")
                   .arg(QString::number(instances_.size()), QString::number(remaining_view_arms)));
    clearGeometry();
    return true;
  }
  return changed;
}

bool WasmPosesInFrameLayer::bootstrap() {
  if (session_ == nullptr) {
    return false;
  }
  const auto first = session_->objectStore().at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty() ||
      !browserPosePayloadFits(static_cast<std::uint64_t>(first->payload.bytes.size()))) {
    return false;
  }
  const auto binding = session_->parserBindingForObjectTopic(topic_id_);
  auto object = resolveObject(binding, PJ::sdk::BuiltinObjectType::kPosesInFrame, first->timestamp, first->payload);
  if (!object.has_value()) {
    return false;
  }
  const auto* poses = object->object.get<PJ::sdk::PosesInFrame>();
  if (poses == nullptr) {
    return false;
  }
  updateSourceFrame(poses->frame_id);
  return true;
}

bool WasmPosesInFrameLayer::decodeAt(PJ::Timepoint time) {
  if (session_ == nullptr) {
    return false;
  }
  const auto entry = session_->objectStore().latestAt(topic_id_, PJ::toRaw(time));
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    const bool changed = !instances_.empty();
    staged_uid_ = {};
    staged_style_revision_ = -1;
    clearGeometry();
    setWarning({});
    return changed;
  }
  if (entry->sequential_uid == staged_uid_ && staged_style_revision_ == style_revision_) {
    return false;
  }
  if (!browserPosePayloadFits(static_cast<std::uint64_t>(entry->payload.bytes.size()))) {
    setWarning(tr("Pose payload is %1 MiB; the browser limit is %2 MiB")
                   .arg(QString::number(static_cast<double>(entry->payload.bytes.size()) / (1024.0 * 1024.0), 'f', 1))
                   .arg(kMaxWireBytesPerLayer / (1024ULL * 1024ULL)));
    clearGeometry();
    staged_uid_ = entry->sequential_uid;
    staged_style_revision_ = style_revision_;
    return true;
  }

  const auto binding = session_->parserBindingForObjectTopic(topic_id_);
  auto object = resolveObject(binding, PJ::sdk::BuiltinObjectType::kPosesInFrame, entry->timestamp, entry->payload);
  if (!object.has_value()) {
    setWarning(tr("Pose array could not be decoded: %1").arg(QString::fromStdString(object.error())));
    clearGeometry();
    staged_uid_ = entry->sequential_uid;
    staged_style_revision_ = style_revision_;
    return true;
  }
  const auto* poses = object->object.get<PJ::sdk::PosesInFrame>();
  if (poses == nullptr) {
    setWarning(tr("This browser layer accepts PosesInFrame samples only"));
    clearGeometry();
    staged_uid_ = entry->sequential_uid;
    staged_style_revision_ = style_revision_;
    return true;
  }
  updateSourceFrame(poses->frame_id);
  if (!browserPoseCountFits(static_cast<std::uint64_t>(poses->poses.size()))) {
    setWarning(tr("Pose array has %1 poses; the browser limit is %2")
                   .arg(QString::number(poses->poses.size()), QString::number(kMaxPosesPerLayer)));
    clearGeometry();
    staged_uid_ = entry->sequential_uid;
    staged_style_revision_ = style_revision_;
    return true;
  }

  std::vector<PoseTriadInstance> expanded;
  try {
    expanded = buildPoseTriadInstances(
        *poses, PoseTriadStyle{
                    .axis_length = gizmo_size_,
                    .opacity = gizmo_opacity_,
                    .x_arrow_only = x_arrow_only_,
                    .override_color = override_color_enabled_,
                    .color = glm::vec3(override_color_.redF(), override_color_.greenF(), override_color_.blueF()),
                });
  } catch (const std::bad_alloc&) {
    setWarning(tr("Not enough browser memory to expand this pose array"));
    clearGeometry();
    staged_uid_ = entry->sequential_uid;
    staged_style_revision_ = style_revision_;
    return true;
  } catch (const std::exception& error) {
    setWarning(tr("Pose-array expansion failed: %1").arg(QString::fromUtf8(error.what())));
    clearGeometry();
    staged_uid_ = entry->sequential_uid;
    staged_style_revision_ = style_revision_;
    return true;
  }

  AABB bounds;
  const auto finite_end =
      std::remove_if(expanded.begin(), expanded.end(), [&bounds](const PoseTriadInstance& instance) {
        if (!finiteInstance(instance)) {
          return true;
        }
        expandInstanceBounds(bounds, instance);
        return false;
      });
  const bool had_nonfinite = finite_end != expanded.end();
  expanded.erase(finite_end, expanded.end());
  instances_ = std::move(expanded);
  source_bounds_ = bounds;
  ++geometry_revision_;
  staged_uid_ = entry->sequential_uid;
  staged_style_revision_ = style_revision_;
  if (!poses->poses.empty() && instances_.empty()) {
    setWarning(tr("Pose array contains no finite poses"));
  } else if (had_nonfinite) {
    setWarning(tr("Non-finite poses were skipped"));
  } else {
    setWarning({});
  }
  return true;
}

void WasmPosesInFrameLayer::requestDecode() {
  budget_rejected_arm_count_.reset();
  decode_dirty_ = requested_time_.has_value();
  emit repaintRequested();
}

void WasmPosesInFrameLayer::clearGeometry() {
  const bool had_geometry = !instances_.empty() || source_bounds_.valid;
  std::vector<PoseTriadInstance>{}.swap(instances_);
  source_bounds_ = {};
  if (had_geometry) {
    ++geometry_revision_;
  }
}

void WasmPosesInFrameLayer::updateSourceFrame(const std::string& frame) {
  if (source_frame_ == frame) {
    return;
  }
  source_frame_ = frame;
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
}

void WasmPosesInFrameLayer::setWarning(QString warning) {
  if (warning_reason_ == warning) {
    return;
  }
  warning_reason_ = std::move(warning);
  emit warningChanged(!warning_reason_.isEmpty(), warning_reason_);
}

void WasmPosesInFrameLayer::setGizmoSize(float metres) {
  metres = std::clamp(metres, 0.01F, 100.0F);
  if (gizmo_size_ == metres) {
    return;
  }
  gizmo_size_ = metres;
  ++style_revision_;
  emit configurationChanged();
  requestDecode();
}

void WasmPosesInFrameLayer::setGizmoOpacity(float opacity) {
  opacity = std::clamp(opacity, 0.0F, 1.0F);
  if (gizmo_opacity_ == opacity) {
    return;
  }
  gizmo_opacity_ = opacity;
  ++style_revision_;
  emit configurationChanged();
  requestDecode();
}

void WasmPosesInFrameLayer::setXArrowOnly(bool x_arrow_only) {
  if (x_arrow_only_ == x_arrow_only) {
    return;
  }
  x_arrow_only_ = x_arrow_only;
  ++style_revision_;
  emit configurationChanged();
  requestDecode();
}

void WasmPosesInFrameLayer::setOverrideColorEnabled(bool enabled) {
  if (override_color_enabled_ == enabled) {
    return;
  }
  override_color_enabled_ = enabled;
  ++style_revision_;
  emit configurationChanged();
  requestDecode();
}

void WasmPosesInFrameLayer::setOverrideColor(QColor color) {
  if (!color.isValid() || override_color_.rgb() == color.rgb()) {
    return;
  }
  override_color_ = color;
  ++style_revision_;
  emit configurationChanged();
  requestDecode();
}

QWidget* WasmPosesInFrameLayer::createConfigWidget(QWidget* parent) {
  auto* container = new QWidget(parent);
  auto* outer = new QVBoxLayout(container);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  auto* form = new QFormLayout();
  form->setContentsMargins(0, 0, 0, 0);
  form->setHorizontalSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  form->setVerticalSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  outer->addLayout(form);

  auto* size = new PJ::DoubleScrubber(container);
  size->setObjectName(u"wasmPoseArrowLength"_s);
  size->setRange(0.01, 5.0);
  size->setDecimals(3);
  size->setSingleStep(0.01);
  size->setValue(gizmo_size_);
  form->addRow(tr("Arrow length (m):"), size);

  auto* opacity = new PJ::DoubleScrubber(container);
  opacity->setObjectName(u"wasmPoseOpacity"_s);
  opacity->setRange(0.0, 1.0);
  opacity->setDecimals(2);
  opacity->setSingleStep(0.05);
  opacity->setValue(gizmo_opacity_);
  form->addRow(tr("Opacity:"), opacity);

  auto* x_only = new PJ::ToggleSwitch(container);
  x_only->setObjectName(u"wasmPoseXArrowOnly"_s);
  x_only->setChecked(x_arrow_only_, false);
  form->addRow(tr("X Arrow only:"), x_only);

  auto* override_row = new QWidget(container);
  auto* override_layout = new QHBoxLayout(override_row);
  override_layout->setContentsMargins(0, 0, 0, 0);
  override_layout->setSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  auto* override_toggle = new PJ::ToggleSwitch(override_row);
  override_toggle->setObjectName(u"wasmPoseOverrideColorEnabled"_s);
  override_toggle->setChecked(override_color_enabled_, false);
  auto* color = new PJ::ColorPickerWidget(override_row);
  color->setObjectName(u"wasmPoseOverrideColor"_s);
  color->setColor(override_color_);
  override_layout->addWidget(override_toggle);
  override_layout->addWidget(color);
  override_layout->addStretch();
  form->addRow(tr("Override color:"), override_row);

  connect(
      size, &PJ::DoubleScrubber::valueChanged, this, [this](double value) { setGizmoSize(static_cast<float>(value)); });
  connect(opacity, &PJ::DoubleScrubber::valueChanged, this, [this](double value) {
    setGizmoOpacity(static_cast<float>(value));
  });
  connect(x_only, &PJ::ToggleSwitch::toggled, this, &WasmPosesInFrameLayer::setXArrowOnly);
  connect(override_toggle, &PJ::ToggleSwitch::toggled, this, &WasmPosesInFrameLayer::setOverrideColorEnabled);
  connect(color, &PJ::ColorPickerWidget::colorChanged, this, [this, override_toggle](QColor selected) {
    setOverrideColor(selected);
    if (!override_toggle->isChecked()) {
      override_toggle->setChecked(true);
    }
    setOverrideColorEnabled(true);
  });
  return container;
}

QDomElement WasmPosesInFrameLayer::xmlSaveState(QDomDocument& document) const {
  QDomElement element = document.createElement(u"poses_in_frame"_s);
  element.setAttribute(u"gizmo_size"_s, static_cast<double>(gizmo_size_));
  element.setAttribute(u"gizmo_opacity"_s, static_cast<double>(gizmo_opacity_));
  element.setAttribute(u"x_arrow_only"_s, x_arrow_only_ ? 1 : 0);
  element.setAttribute(u"override_color"_s, override_color_enabled_ ? 1 : 0);
  element.setAttribute(u"override_color_value"_s, override_color_.name(QColor::HexRgb));
  return element;
}

std::optional<WasmPosesInFrameLayer::ParsedSettings> WasmPosesInFrameLayer::parseSettings(const QDomElement& element) {
  // Accept-and-ignore the desktop build's nested <trail>: the browser has no
  // trail layer, but a desktop-saved layout must still restore the poses.
  if (element.isNull() || element.tagName() != "poses_in_frame"_L1 || !detail::isLeafPayload(element, "trail"_L1)) {
    return std::nullopt;
  }
  ParsedSettings settings;
  if (!detail::parseFiniteFloat(element, "gizmo_size", 0.15F, 0.01F, 100.0F, settings.gizmo_size) ||
      !detail::parseFiniteFloat(element, "gizmo_opacity", 1.0F, 0.0F, 1.0F, settings.gizmo_opacity) ||
      !detail::parseZeroOne(element, "x_arrow_only", false, settings.x_arrow_only) ||
      !detail::parseZeroOne(element, "override_color", false, settings.override_color_enabled)) {
    return std::nullopt;
  }
  if (element.hasAttribute(u"override_color_value"_s)) {
    QColor color(element.attribute(u"override_color_value"_s));
    if (!color.isValid()) {
      return std::nullopt;
    }
    settings.override_color = color;
  }
  return settings;
}

bool WasmPosesInFrameLayer::validateXml(const QDomElement& element) {
  return parseSettings(element).has_value();
}

bool WasmPosesInFrameLayer::xmlLoadState(const QDomElement& element) {
  const auto settings = parseSettings(element);
  if (!settings.has_value()) {
    return false;
  }
  gizmo_size_ = settings->gizmo_size;
  gizmo_opacity_ = settings->gizmo_opacity;
  x_arrow_only_ = settings->x_arrow_only;
  override_color_enabled_ = settings->override_color_enabled;
  if (settings->override_color.has_value()) {
    override_color_ = *settings->override_color;
  }
  ++style_revision_;
  emit configurationChanged();
  requestDecode();
  return true;
}

void WasmPosesInFrameLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (session_ == nullptr || session_->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
  staged_uid_ = {};
  staged_style_revision_ = -1;
  budget_rejected_arm_count_.reset();
  decode_dirty_ = requested_time_.has_value();
  clearGeometry();
  setWarning({});
  emit repaintRequested();
}

}  // namespace pj::scene3d
