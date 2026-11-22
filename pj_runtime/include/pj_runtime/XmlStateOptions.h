#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomElement>
#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

namespace PJ {

/// One attribute an `xmlLoadState` payload accepts, as its parser checks it.
// Every member has an initializer, so a designated initializer may omit any of them under -Wextra (GCC 15 and
// Clang 19 warn on an omitted member without one).
struct XmlOption {
  QString element = {};  ///< the tag the attribute sits on
  QString attribute = {};
  QString type = {};        ///< "enum", "bool", "int", "float", "color", "text" or "json"
  QStringList values = {};  ///< the accepted tokens of an "enum" (and of a "bool")
  std::optional<double> min = std::nullopt;
  std::optional<double> max = std::nullopt;
  QString fallback = {};  ///< the value an absent attribute takes; empty when the parser has none
};

/// Collects what the widget XML parsers report while it is alive on this thread: every attribute they read (its
/// type, tokens, bounds and fallback) and the first value they refuse. Validating a widget's own saved state under a
/// recorder lists the options the widget accepts; validating a candidate tells why it was refused. Recorders nest:
/// the innermost one receives the reports and the outer one gets them back when it is destroyed. Not shared across
/// threads; construct and destroy it on the thread that parses.
class XmlOptionsRecorder {
 public:
  XmlOptionsRecorder();
  ~XmlOptionsRecorder();
  XmlOptionsRecorder(const XmlOptionsRecorder&) = delete;
  XmlOptionsRecorder& operator=(const XmlOptionsRecorder&) = delete;

  /// One entry per element tag and attribute, the first report winning, in the order parsed.
  [[nodiscard]] const std::vector<XmlOption>& options() const {
    return options_;
  }
  /// `<tag> attribute="value": reason` for the first refused value; empty when none was refused.
  [[nodiscard]] const QString& firstRejection() const {
    return first_rejection_;
  }

  /// Reports `option` (its `element` is filled from `element`) to the active recorder; a no-op without one.
  static void noteOption(const QDomElement& element, XmlOption option);
  /// Reports a value the parser refuses, or one a tolerant parser accepts but replaces with a default (an unknown
  /// token read as the default, an unparsable number read as 0), to the active recorder; a no-op without one. A
  /// tolerant parser still returns success, so a layout load keeps its tolerance, while the MCP widget_state tool
  /// refuses any element its recorder found a rejection in.
  static void noteRejection(const QDomElement& element, const QString& attribute, const QString& reason);

 private:
  XmlOptionsRecorder* previous_ = nullptr;
  std::vector<XmlOption> options_;
  QString first_rejection_;
};

/// Reads enum attribute `attribute` of `element`: one of `tokens`, `fallback` when absent. Reports the option to the
/// active recorder, and a rejection when the value is not a token. False leaves `value` untouched.
[[nodiscard]] bool readXmlEnum(
    const QDomElement& element, const QString& attribute, const QStringList& tokens, const QString& fallback,
    QString& value);

/// Reads free-text attribute `attribute` of `element` (`fallback` when absent) and reports it as type "text".
[[nodiscard]] QString readXmlText(const QDomElement& element, const QString& attribute, const QString& fallback = {});

/// `tag.attribute` for every attribute of `root` and its descendants that `options` does not report: what a widget
/// saves but its validator never reads through a reporting reader. A widget's test asserts this is empty for its own
/// saved state, so an attribute added to xmlSaveState without a reporting reader fails there.
[[nodiscard]] QStringList unreportedXmlAttributes(const QDomElement& root, const std::vector<XmlOption>& options);

}  // namespace PJ
