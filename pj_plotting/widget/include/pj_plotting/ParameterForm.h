// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QWidget>
#include <nlohmann/json.hpp>
#include <vector>

#include "pj_scripting/filter_class.h"

class QFormLayout;

namespace PJ {

/// A parameter editor built entirely from a filter class's parameter schema: the deliberate dynamic-widget exception to
/// the ".ui-first" rule. One widget per `ParamSpec` (int -> QSpinBox, number -> QLineEdit, boolean -> ToggleSwitch,
/// enum -> PJ::ComboBox, string -> QLineEdit, text -> QPlainTextEdit), bound through the params JSON whose keys are the
/// parameter names. No per-filter Qt.
class ParameterForm : public QWidget {
  Q_OBJECT
 public:
  explicit ParameterForm(QWidget* parent = nullptr);

  /// Rebuilds the rows for `schema`, seeding each editor with its default.
  void setSchema(const std::vector<scripting::ParamSpec>& schema);
  void clear();

  /// Populates the editors from a params JSON without emitting `changed()`.
  void setValues(const nlohmann::json& values);
  /// The editor state as a params JSON (keys == parameter names).
  [[nodiscard]] nlohmann::json values() const;

  [[nodiscard]] bool isEmpty() const {
    return rows_.empty();
  }

 signals:
  /// Any editor changed (not emitted during setSchema/setValues).
  void changed();

 private:
  void updateConditionalVisibility();
  void onEditorChanged();

  struct Row {
    scripting::ParamSpec spec;
    QWidget* editor = nullptr;
    QWidget* label = nullptr;
  };

  QFormLayout* form_ = nullptr;
  std::vector<Row> rows_;
  bool silent_ = false;  ///< suppresses changed() while we drive the widgets
};

}  // namespace PJ
