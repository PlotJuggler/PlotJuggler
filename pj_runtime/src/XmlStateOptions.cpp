// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/XmlStateOptions.h"

#include <QDomDocument>
#include <QDomNamedNodeMap>
#include <QDomNodeList>
#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

thread_local XmlOptionsRecorder* t_active = nullptr;

// A bound this large, or not finite, means "no bound on that side".
constexpr double kOpenBound = 1.0e30;

std::optional<double> effectiveBound(std::optional<double> bound) {
  if (!bound.has_value() || !std::isfinite(*bound) || std::abs(*bound) >= kOpenBound) {
    return std::nullopt;
  }
  return bound;
}

// 7 significant digits: what a float holds, and the decimal a bound written as a float literal came from.
double floatPrecision(double value) {
  return QString::number(value, 'g', 7).toDouble();
}

QString formatNumber(double value, bool integer) {
  return integer ? QString::number(static_cast<qint64>(value)) : QString::number(value, 'g', 7);
}

QString rangeReason(bool integer, std::optional<double> min, std::optional<double> max) {
  const QString what = integer ? QStringLiteral("an integer") : QStringLiteral("a number");
  if (min.has_value() && max.has_value()) {
    return u"expected " + what + u" in [" + formatNumber(*min, integer) + u", " + formatNumber(*max, integer) + u']';
  }
  if (min.has_value()) {
    return u"expected " + what + u" >= " + formatNumber(*min, integer);
  }
  if (max.has_value()) {
    return u"expected " + what + u" <= " + formatNumber(*max, integer);
  }
  return integer ? QStringLiteral("expected an integer") : QStringLiteral("expected a finite number");
}

bool sameOption(const XmlOption& a, const XmlOption& b) {
  return a.element == b.element && a.attribute == b.attribute;
}

// The shared tail of the readers: reports `option`, then classifies the attribute. Returns the present text when it
// is acceptable; nullopt when it is absent (`*bad` false) or not acceptable (`*bad` true, the rejection noted). In
// tolerant mode an empty present value reads as absent, as the tolerant loaders always treated it.
// `check` gives the problem with a present value, empty when there is none.
template <typename Check>
std::optional<QString> readChecked(
    const QDomElement& element, XmlOption option, XmlBadValue bad_mode, bool* bad, const Check& check) {
  *bad = false;
  const QString attribute = option.attribute;
  QString text = element.attribute(attribute);
  const bool absent = !element.hasAttribute(attribute) || (bad_mode == XmlBadValue::kTolerate && text.isEmpty());
  const QString problem = absent ? QString() : check(option, text);
  XmlOptionsRecorder::noteOption(element, std::move(option));
  if (absent) {
    return std::nullopt;
  }
  if (!problem.isEmpty()) {
    XmlOptionsRecorder::noteRejection(element, attribute, problem);
    *bad = true;
    return std::nullopt;
  }
  return text;
}

std::optional<QString> readChecked(const QDomElement& element, XmlOption option, XmlBadValue bad_mode, bool* bad) {
  return readChecked(element, std::move(option), bad_mode, bad, xmlValueProblem);
}

// Applies the fallback for an absent or tolerated value; false only for a refused one.
template <typename T>
bool settle(bool bad, XmlBadValue bad_mode, const std::optional<T>& fallback, T& value) {
  if (bad && bad_mode == XmlBadValue::kRefuse) {
    return false;
  }
  if (fallback.has_value()) {
    value = *fallback;
  }
  return true;
}

}  // namespace

XmlOptionsRecorder::XmlOptionsRecorder() : previous_(t_active) {
  t_active = this;
}

XmlOptionsRecorder::~XmlOptionsRecorder() {
  t_active = previous_;
  if (previous_ == nullptr) {
    return;
  }
  for (XmlOption& option : options_) {
    const auto known = std::find_if(previous_->options_.begin(), previous_->options_.end(), [&](const XmlOption& o) {
      return sameOption(o, option);
    });
    if (known == previous_->options_.end()) {
      previous_->options_.push_back(std::move(option));
    }
  }
  if (previous_->first_rejection_.isEmpty()) {
    previous_->first_rejection_ = first_rejection_;
  }
}

bool XmlOptionsRecorder::active() {
  return t_active != nullptr;
}

void XmlOptionsRecorder::noteOption(const QDomElement& element, XmlOption option) {
  if (t_active == nullptr) {
    return;
  }
  option.element = element.tagName();
  option.min = effectiveBound(option.min);
  option.max = effectiveBound(option.max);
  auto& options = t_active->options_;
  if (std::none_of(options.begin(), options.end(), [&](const XmlOption& o) { return sameOption(o, option); })) {
    options.push_back(std::move(option));
  }
}

void XmlOptionsRecorder::noteRejection(const QDomElement& element, const QString& attribute, const QString& reason) {
  if (t_active == nullptr || !t_active->first_rejection_.isEmpty()) {
    return;
  }
  QString where = u'<' + element.tagName() + u'>';
  if (!attribute.isEmpty()) {
    where += u' ' + attribute + u"=\"" + element.attribute(attribute) + u'"';
  }
  t_active->first_rejection_ = where + u": " + reason;
}

QString xmlValueProblem(const XmlOption& option, const QString& text) {
  if (option.type == u"enum" || option.type == u"bool") {
    if (option.values.isEmpty() || option.values.contains(text)) {
      return {};
    }
    if (option.type == u"bool" && option.values.size() == 2) {
      return u"expected " + option.values[0] + u" or " + option.values[1];
    }
    return u"expected one of " + option.values.join(u", ");
  }
  const bool integer = option.type == u"int";
  if (!integer && option.type != u"float") {
    return {};
  }
  const std::optional<double> min = effectiveBound(option.min);
  const std::optional<double> max = effectiveBound(option.max);
  bool ok = false;
  const double value = integer ? static_cast<double>(text.toLongLong(&ok)) : text.toDouble(&ok);
  if (!ok || !std::isfinite(value) || (min.has_value() && value < *min) || (max.has_value() && value > *max)) {
    return rangeReason(integer, min, max);
  }
  return {};
}

bool readXmlEnum(
    const QDomElement& element, const QString& attribute, const QStringList& tokens, const QString& fallback,
    QString& value, XmlBadValue bad_mode) {
  bool bad = false;
  const std::optional<QString> text = readChecked(
      element, {.attribute = attribute, .type = QStringLiteral("enum"), .values = tokens, .fallback = fallback},
      bad_mode, &bad);
  if (text.has_value()) {
    value = *text;
    return true;
  }
  return settle(bad, bad_mode, std::optional<QString>(fallback), value);
}

bool readXmlBool(
    const QDomElement& element, const QString& attribute, std::optional<bool> fallback, bool& value,
    XmlBadValue bad_mode, const QString& true_token, const QString& false_token) {
  bool bad = false;
  const QString reported_fallback = fallback.has_value() ? (*fallback ? true_token : false_token) : QString();
  const std::optional<QString> text = readChecked(
      element,
      {.attribute = attribute,
       .type = QStringLiteral("bool"),
       .values = {true_token, false_token},
       .fallback = reported_fallback},
      bad_mode, &bad);
  if (text.has_value()) {
    value = *text == true_token;
    return true;
  }
  return settle(bad, bad_mode, fallback, value);
}

bool readXmlInteger(
    const QDomElement& element, const QString& attribute, std::optional<qint64> fallback, qint64 min, qint64 max,
    qint64& value, XmlBadValue bad_mode) {
  bool bad = false;
  const std::optional<QString> text = readChecked(
      element,
      {.attribute = attribute,
       .type = QStringLiteral("int"),
       .min = static_cast<double>(min),
       .max = static_cast<double>(max),
       .fallback = fallback.has_value() && XmlOptionsRecorder::active() ? QString::number(*fallback) : QString()},
      bad_mode, &bad);
  if (text.has_value()) {
    value = text->toLongLong();
    return true;
  }
  return settle(bad, bad_mode, fallback, value);
}

bool readXmlDouble(
    const QDomElement& element, const QString& attribute, std::optional<double> fallback, double min, double max,
    double& value, XmlBadValue bad_mode) {
  bool bad = false;
  const std::optional<QString> text = readChecked(
      element,
      {.attribute = attribute,
       .type = QStringLiteral("float"),
       .min = min,
       .max = max,
       .fallback =
           fallback.has_value() && XmlOptionsRecorder::active() ? QString::number(*fallback, 'g', 9) : QString()},
      bad_mode, &bad);
  if (text.has_value()) {
    value = text->toDouble();
    return true;
  }
  return settle(bad, bad_mode, fallback, value);
}

bool readXmlFloat(
    const QDomElement& element, const QString& attribute, std::optional<float> fallback, float min, float max,
    float& value, XmlBadValue bad_mode) {
  // Parsed and compared as a float, as the attribute is stored: "0.001" meets a 0.001f bound, a value rounding onto a
  // bound meets it, and one a float cannot hold (1e39, 1e-50) is refused by toFloat.
  const auto check = [min, max](const XmlOption& option, const QString& text) -> QString {
    bool ok = false;
    const float parsed = text.toFloat(&ok);
    if (ok && std::isfinite(parsed) && parsed >= min && parsed <= max) {
      return {};
    }
    return rangeReason(false, effectiveBound(option.min), effectiveBound(option.max));
  };
  bool bad = false;
  const std::optional<QString> text = readChecked(
      element,
      {.attribute = attribute,
       .type = QStringLiteral("float"),
       .min = floatPrecision(min),
       .max = floatPrecision(max),
       .fallback =
           fallback.has_value() && XmlOptionsRecorder::active() ? QString::number(*fallback, 'g', 7) : QString()},
      bad_mode, &bad, check);
  if (text.has_value()) {
    value = text->toFloat();
    return true;
  }
  return settle(bad, bad_mode, fallback, value);
}

QString readXmlText(const QDomElement& element, const QString& attribute, const QString& fallback) {
  XmlOptionsRecorder::noteOption(
      element, {.attribute = attribute, .type = QStringLiteral("text"), .fallback = fallback});
  return element.attribute(attribute, fallback);
}

QStringList unreportedXmlAttributes(const QDomElement& root, const std::vector<XmlOption>& options) {
  QStringList missing;
  const auto visit = [&](const auto& self, const QDomElement& element) -> void {
    const QDomNamedNodeMap attributes = element.attributes();
    for (int index = 0; index < attributes.count(); ++index) {
      const QString name = attributes.item(index).nodeName();
      const bool reported = std::any_of(options.begin(), options.end(), [&](const XmlOption& option) {
        return option.element == element.tagName() && option.attribute == name;
      });
      const QString entry = element.tagName() + u'.' + name;
      if (!reported && !missing.contains(entry)) {
        missing.push_back(entry);
      }
    }
    for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
      self(self, child);
    }
  };
  visit(visit, root);
  return missing;
}

namespace {

// A copy of `saved` with `attribute` set to `value` on every `tag` element carrying it, in `doc`; null when none does.
QDomElement substituted(
    QDomDocument& doc, const QDomElement& saved, const QString& tag, const QString& attribute, const QString& value) {
  QDomElement copy = doc.importNode(saved, /*deep=*/true).toElement();
  doc.appendChild(copy);
  QList<QDomElement> targets;
  if (copy.tagName() == tag) {
    targets.append(copy);
  }
  const QDomNodeList descendants = copy.elementsByTagName(tag);
  for (int index = 0; index < descendants.size(); ++index) {
    targets.append(descendants.at(index).toElement());
  }
  bool any = false;
  for (QDomElement& target : targets) {
    if (target.hasAttribute(attribute)) {
      target.setAttribute(attribute, value);
      any = true;
    }
  }
  return any ? copy : QDomElement();
}

}  // namespace

QStringList xmlSelfDescriptionProblems(
    const std::function<bool(const QDomElement&)>& validate, const QDomElement& saved,
    const XmlSelfDescriptionRules& rules) {
  QStringList problems;
  std::vector<XmlOption> options;
  {
    XmlOptionsRecorder recorder;
    if (!validate(saved) || !recorder.firstRejection().isEmpty()) {
      return {u"the saved state is refused: "_s + recorder.firstRejection()};
    }
    options = recorder.options();
  }
  for (const QString& entry : unreportedXmlAttributes(saved, options)) {
    problems.push_back(entry + u": saved but not reported"_s);
  }
  for (const XmlOption& option : options) {
    const QString name = option.element + u'.' + option.attribute;
    if (!rules.skip_tokens.contains(name)) {
      for (const QString& token : option.values) {
        QDomDocument doc;
        const QDomElement copy = substituted(doc, saved, option.element, option.attribute, token);
        if (copy.isNull()) {
          continue;  // reported by a reader the saved state does not exercise on this tag
        }
        XmlOptionsRecorder recorder;
        if (!validate(copy) || !recorder.firstRejection().isEmpty()) {
          problems.push_back(name + u'=' + token + u": its own token is refused: "_s + recorder.firstRejection());
        }
      }
    }
    if (!rules.check_bad_values || (option.type == u"text"_s && option.values.isEmpty())) {
      continue;
    }
    QDomDocument doc;
    const QDomElement bad = substituted(doc, saved, option.element, option.attribute, u"~bad~"_s);
    if (bad.isNull()) {
      continue;
    }
    XmlOptionsRecorder recorder;
    const bool accepted = validate(bad);
    if (accepted != rules.tolerates_bad.contains(name)) {
      problems.push_back(name + (accepted ? u"=~bad~ is accepted"_s : u"=~bad~ is refused, not tolerated"_s));
    }
    if (!recorder.firstRejection().contains(option.attribute + u"=\"~bad~\""_s)) {
      problems.push_back(name + u"=~bad~ is not named: "_s + recorder.firstRejection());
    }
    if (rules.load && rules.load(bad) != accepted) {
      problems.push_back(name + u"=~bad~: load and validation disagree"_s);
    }
  }
  return problems;
}

}  // namespace PJ
