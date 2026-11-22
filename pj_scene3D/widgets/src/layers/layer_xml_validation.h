// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QDomElement>
#include <QString>
#include <QStringList>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <utility>

#include "pj_runtime/XmlStateOptions.h"
#include "pj_widgets/Colormap.h"

namespace pj::scene3d::detail {

using namespace Qt::StringLiterals;

// Layer payloads are attribute-only leaves. Unknown attributes are ignored (forward compatibility); unknown nested
// elements are a structural error because an older build cannot restore them faithfully. `allowed_nested_tag` is the
// one exception: a layer that OWNS a sub-layer persists it as a single nested payload of that tag (PosesInFrameLayer's
// embedded <trail>); every other nested element fails, and so does a second copy of the allowed one.
inline bool isLeafPayload(const QDomElement& element, QLatin1String allowed_nested_tag = {}) {
  bool seen_nested = false;
  for (QDomNode child = element.firstChild(); !child.isNull(); child = child.nextSibling()) {
    if (child.isElement()) {
      if (allowed_nested_tag.isEmpty() || seen_nested || child.toElement().tagName() != allowed_nested_tag) {
        PJ::XmlOptionsRecorder::noteRejection(
            element, {}, u"unexpected child <"_s + child.toElement().tagName() + u'>');
        return false;
      }
      seen_nested = true;
      continue;
    }
    if ((child.isText() || child.isCDATASection()) && !child.nodeValue().trimmed().isEmpty()) {
      PJ::XmlOptionsRecorder::noteRejection(element, {}, u"unexpected text content"_s);
      return false;
    }
  }
  return true;
}

// A PJ::XmlOption from its fields in order, the ones left out empty: the readers below build one per call.
inline PJ::XmlOption xmlOption(
    QString attribute, QString type, QStringList values = {}, std::optional<double> minimum = std::nullopt,
    std::optional<double> maximum = std::nullopt, QString fallback = {}) {
  PJ::XmlOption option;
  option.attribute = std::move(attribute);
  option.type = std::move(type);
  option.values = std::move(values);
  option.min = minimum;
  option.max = maximum;
  option.fallback = std::move(fallback);
  return option;
}

// The readers below report what they read to the active PJ::XmlOptionsRecorder (no-op without one): the attribute,
// its type, bounds and the value an absent attribute takes, and a rejection naming the refused attribute. Validation
// and load share them, so the options an agent is shown are the ones the loader enforces.

// A bound at the float/double limits means "any finite value": it is reported, but kept out of the reason.
inline QString numberReason(double minimum, double maximum) {
  constexpr double kOpen = 1.0e30;
  if (minimum <= -kOpen && maximum >= kOpen) {
    return u"expected a finite number"_s;
  }
  return u"expected a number in ["_s + QString::number(minimum, 'g', 6) + u", "_s + QString::number(maximum, 'g', 6) +
         u']';
}

// Shared by the float and double readers: parses with `convert`, the range checked in the reader's own precision.
template <typename Number, typename Convert>
bool parseFiniteNumber(
    const QDomElement& element, const char* attribute, Number fallback, Number minimum, Number maximum, Number& value,
    Convert convert) {
  const QString name = QLatin1String(attribute);
  PJ::XmlOptionsRecorder::noteOption(
      element, xmlOption(
                   name, u"float"_s, {}, static_cast<double>(minimum), static_cast<double>(maximum),
                   QString::number(static_cast<double>(fallback), 'g', 6)));
  bool ok = true;
  value = element.hasAttribute(name) ? convert(element.attribute(name), &ok) : fallback;
  if (ok && std::isfinite(value) && value >= minimum && value <= maximum) {
    return true;
  }
  PJ::XmlOptionsRecorder::noteRejection(
      element, name, numberReason(static_cast<double>(minimum), static_cast<double>(maximum)));
  return false;
}

inline bool parseFiniteFloat(
    const QDomElement& element, const char* attribute, float fallback, float minimum, float maximum, float& value) {
  return parseFiniteNumber(element, attribute, fallback, minimum, maximum, value, [](const QString& text, bool* ok) {
    return text.toFloat(ok);
  });
}

inline bool parseFiniteDouble(
    const QDomElement& element, const char* attribute, double fallback, double minimum, double maximum, double& value) {
  return parseFiniteNumber(element, attribute, fallback, minimum, maximum, value, [](const QString& text, bool* ok) {
    return text.toDouble(ok);
  });
}

inline bool parseBoundedInt(
    const QDomElement& element, const char* attribute, int fallback, int minimum, int maximum, int& value) {
  const QString name = QLatin1String(attribute);
  PJ::XmlOptionsRecorder::noteOption(
      element,
      xmlOption(
          name, u"int"_s, {}, static_cast<double>(minimum), static_cast<double>(maximum), QString::number(fallback)));
  bool ok = true;
  value = element.hasAttribute(name) ? element.attribute(name).toInt(&ok) : fallback;
  if (ok && value >= minimum && value <= maximum) {
    return true;
  }
  PJ::XmlOptionsRecorder::noteRejection(
      element, name,
      u"expected a number in ["_s + QString::number(minimum) + u", "_s + QString::number(maximum) + u']');
  return false;
}

// A two-token boolean, reported as type "bool" with its tokens (`true_token` first).
inline bool parseBoolTokens(
    const QDomElement& element, const char* attribute, const QString& true_token, const QString& false_token,
    bool fallback, bool& value) {
  const QString name = QLatin1String(attribute);
  PJ::XmlOptionsRecorder::noteOption(
      element,
      xmlOption(
          name, u"bool"_s, {true_token, false_token}, std::nullopt, std::nullopt, fallback ? true_token : false_token));
  if (!element.hasAttribute(name)) {
    value = fallback;
    return true;
  }
  const QString text = element.attribute(name);
  if (text == true_token || text == false_token) {
    value = text == true_token;
    return true;
  }
  PJ::XmlOptionsRecorder::noteRejection(element, name, u"expected "_s + true_token + u" or "_s + false_token);
  return false;
}

inline bool parseTrueFalse(const QDomElement& element, const char* attribute, bool fallback, bool& value) {
  return parseBoolTokens(element, attribute, u"true"_s, u"false"_s, fallback, value);
}

inline bool parseZeroOne(const QDomElement& element, const char* attribute, bool fallback, bool& value) {
  return parseBoolTokens(element, attribute, u"1"_s, u"0"_s, fallback, value);
}

// A colour attribute (any QColor name, "#rrggbb" as saved). Absent: true with `value` untouched; `fallback` is only
// what is reported. Invalid: false.
inline bool parseColor(
    const QDomElement& element, const char* attribute, const QString& fallback, std::optional<QColor>& value) {
  const QString name = QLatin1String(attribute);
  PJ::XmlOptionsRecorder::noteOption(element, xmlOption(name, u"color"_s, {}, std::nullopt, std::nullopt, fallback));
  if (!element.hasAttribute(name)) {
    return true;
  }
  const QColor color(element.attribute(name));
  if (!color.isValid()) {
    PJ::XmlOptionsRecorder::noteRejection(element, name, u"expected a colour"_s);
    return false;
  }
  value = color;
  return true;
}

// The colormap tokens PJ::parseColormap accepts, in enum order.
inline QStringList colormapTokens() {
  QStringList tokens;
  for (int index = 0; index < PJ::kColormapCount; ++index) {
    tokens.push_back(PJ::colormapToken(static_cast<PJ::Colormap>(index)));
  }
  return tokens;
}

// A colormap saved by token ("turbo", ...), Turbo when absent.
inline bool parseColormapToken(const QDomElement& element, const char* attribute, PJ::Colormap& value) {
  QString token;
  if (!PJ::readXmlEnum(
          element, QLatin1String(attribute), colormapTokens(), PJ::colormapToken(PJ::Colormap::kTurbo), token)) {
    return false;
  }
  value = *PJ::parseColormap(token);
  return true;
}

// A token a loader tolerates (any other value takes the default path): reported with its tokens, and a present,
// non-empty value outside them is noted as a rejection while the read still succeeds, so a strict caller refuses it.
inline void noteTolerantToken(
    const QDomElement& element, const QString& attribute, const QString& type, const QStringList& tokens,
    const QString& fallback = {}) {
  PJ::XmlOptionsRecorder::noteOption(element, xmlOption(attribute, type, tokens, std::nullopt, std::nullopt, fallback));
  const QString text = element.attribute(attribute);
  if (!text.isEmpty() && !tokens.contains(text)) {
    PJ::XmlOptionsRecorder::noteRejection(
        element, attribute,
        tokens.size() == 2 ? u"expected "_s + tokens[0] + u" or "_s + tokens[1]
                           : u"expected one of "_s + tokens.join(u", "_s));
  }
}

// A cross-attribute constraint the per-attribute readers cannot see (range_min <= range_max, ...).
inline bool requireConstraint(bool holds, const QDomElement& element, const char* attribute, const char* reason) {
  if (!holds) {
    PJ::XmlOptionsRecorder::noteRejection(element, QLatin1String(attribute), QLatin1String(reason));
  }
  return holds;
}

}  // namespace pj::scene3d::detail
