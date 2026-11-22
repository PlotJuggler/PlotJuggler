// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/scene_state_xml.h"

#include <cmath>
#include <glm/trigonometric.hpp>
#include <limits>
#include <nlohmann/json.hpp>
#include <numbers>
#include <string>
#include <tuple>

#include "layers/layer_xml_validation.h"
#include "pj_runtime/XmlStateOptions.h"

using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {

// Same contract as the XML readers: a missing key is valid, a present key must
// be a finite number inside [minimum, maximum].
bool readJsonFloat(
    const nlohmann::json& object, const char* key, float minimum, float maximum, std::optional<float>& output) {
  const auto iterator = object.find(key);
  if (iterator == object.end()) {
    return true;
  }
  if (!iterator->is_number()) {
    return false;
  }
  try {
    const double value = iterator->get<double>();
    if (!std::isfinite(value) || value < minimum || value > maximum) {
      return false;
    }
    const float converted = static_cast<float>(value);
    if (!std::isfinite(converted)) {
      return false;
    }
    output = converted;
    return true;
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}

}  // namespace

bool readXmlBool(const QDomElement& element, const QString& key, std::optional<bool>& output) {
  PJ::XmlOptionsRecorder::noteOption(element, detail::xmlOption(key, u"bool"_s, {u"true"_s, u"false"_s}));
  if (!element.hasAttribute(key)) {
    return true;
  }
  const QString value = element.attribute(key);
  if (value != "true"_L1 && value != "false"_L1) {
    PJ::XmlOptionsRecorder::noteRejection(element, key, u"expected true or false"_s);
    return false;
  }
  output = value == "true"_L1;
  return true;
}

bool readXmlInt(const QDomElement& element, const QString& key, int minimum, int maximum, std::optional<int>& output) {
  PJ::XmlOptionsRecorder::noteOption(
      element, detail::xmlOption(key, u"int"_s, {}, static_cast<double>(minimum), static_cast<double>(maximum)));
  if (!element.hasAttribute(key)) {
    return true;
  }
  bool ok = false;
  const int value = element.attribute(key).toInt(&ok);
  if (!ok || value < minimum || value > maximum) {
    PJ::XmlOptionsRecorder::noteRejection(
        element, key,
        u"expected a number in ["_s + QString::number(minimum) + u", "_s + QString::number(maximum) + u']');
    return false;
  }
  output = value;
  return true;
}

bool readXmlFloat(
    const QDomElement& element, const QString& key, float minimum, float maximum, std::optional<float>& output) {
  PJ::XmlOptionsRecorder::noteOption(
      element, detail::xmlOption(key, u"float"_s, {}, static_cast<double>(minimum), static_cast<double>(maximum)));
  if (!element.hasAttribute(key)) {
    return true;
  }
  bool ok = false;
  const double value = element.attribute(key).toDouble(&ok);
  const auto converted = static_cast<float>(value);
  if (!ok || !std::isfinite(value) || value < minimum || value > maximum || !std::isfinite(converted)) {
    PJ::XmlOptionsRecorder::noteRejection(
        element, key,
        u"expected a number in ["_s + QString::number(minimum, 'g', 6) + u", "_s + QString::number(maximum, 'g', 6) +
            u']');
    return false;
  }
  output = converted;
  return true;
}

std::optional<ValidatedCameraState> validateCameraState(const QString& encoded, std::string* bad_field) {
  const auto fail = [bad_field](const char* field) -> std::optional<ValidatedCameraState> {
    if (bad_field != nullptr) {
      *bad_field = field;
    }
    return std::nullopt;
  };
  const nlohmann::json object = nlohmann::json::parse(encoded.toStdString(), nullptr, false);
  if (object.is_discarded() || !object.is_object()) {
    return fail("");
  }
  constexpr float kFloatMax = std::numeric_limits<float>::max();
  constexpr float kMinimumScale = 1e-3f;
  constexpr float kPolarLimit = std::numbers::pi_v<float> * 0.5f - 0.05f;
  ValidatedCameraState state;
  const auto focal = object.find("focal");
  if (focal != object.end()) {
    if (!focal->is_array() || focal->size() != 3) {
      return fail("focal");
    }
    glm::vec3 value{0.0f};
    for (int index = 0; index < 3; ++index) {
      const nlohmann::json& component = (*focal)[static_cast<std::size_t>(index)];
      if (!component.is_number()) {
        return fail("focal");
      }
      try {
        const double number = component.get<double>();
        if (!std::isfinite(number) || number < -kFloatMax || number > kFloatMax) {
          return fail("focal");
        }
        value[index] = static_cast<float>(number);
      } catch (const nlohmann::json::exception&) {
        return fail("focal");
      }
    }
    state.focal = value;
  }
  for (const auto& [key, minimum, maximum, field] :
       {std::tuple{"radius", kMinimumScale, kFloatMax, &state.radius},
        std::tuple{"azimuth", -kFloatMax, kFloatMax, &state.azimuth},
        std::tuple{"elevation", -kPolarLimit, kPolarLimit, &state.elevation},
        std::tuple{"fov_y", glm::radians(1.0f), glm::radians(179.0f), &state.fov_y},
        std::tuple{"ortho_scale", kMinimumScale, kFloatMax, &state.ortho_scale}}) {
    if (!readJsonFloat(object, key, minimum, maximum, *field)) {
      return fail(key);
    }
  }
  const auto perspective = object.find("perspective");
  if (perspective != object.end()) {
    if (!perspective->is_boolean()) {
      return fail("perspective");
    }
    state.perspective = perspective->get<bool>();
  }
  return state;
}

bool readSceneControls(const QDomElement& element, ValidatedSceneControls& output) {
  return readXmlBool(element, u"grid_visible"_s, output.grid_visible) &&
         readXmlInt(element, u"grid_style"_s, 0, 1, output.grid_style) &&
         readXmlFloat(element, u"grid_extent_m"_s, kGridExtentMinM, kGridExtentMaxM, output.grid_extent_m) &&
         readXmlInt(element, u"grid_divisions"_s, 1, 200, output.grid_divisions) &&
         readXmlBool(element, u"axes_visible"_s, output.axes_visible) &&
         readXmlFloat(element, u"gizmo_size_m"_s, kGizmoSizeMinM, kGizmoSizeMaxM, output.gizmo_size_m) &&
         readXmlFloat(element, u"gizmo_opacity"_s, 0.0f, 1.0f, output.gizmo_opacity) &&
         readXmlBool(element, u"tf_parent_lines"_s, output.tf_parent_lines);
}

}  // namespace pj::scene3d
