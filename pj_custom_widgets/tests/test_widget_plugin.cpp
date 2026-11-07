// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Widget plugin with real Qt, loaded by the headless end-to-end tests of the Custom Widget panel
// exactly as a shipped widget library would be: a SHARED library found through --plugin-dir.
// Two widget types: "label" (a number and a text input) and "table" (one `multiple` number input).

// The Qt header must precede PJ_WIDGET_PLUGIN: the macro reads QT_VERSION_* for build_info.
#include <QLabel>
#include <QPointer>
#include <QString>
#include <pj_plugins/sdk/widget_plugin_base.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kManifest =
    R"({"id":"pj_test_widgets","name":"Test Widgets","version":"1.0.0",)"
    R"("qt_modules":["Core","Gui","Widgets"],"widgets":[)"
    R"({"type":"label","name":"Label","icon":"","settings":[{"key":"prefix","type":"text","default":""}]},)"
    R"({"type":"table","name":"Table","icon":""}]})";

class TestWidgets : public PJ::WidgetPluginBase {
 public:
  explicit TestWidgets(std::string_view type) : table_(type == "table") {
    if (!table_ && type != "label") {
      throw std::invalid_argument("unknown widget type");
    }
  }

  ~TestWidgets() override {
    // The host detaches the widget before destroying the instance; it may be gone already.
    delete label_.data();
  }

  std::vector<PJ::WidgetInput> inputs() const override {
    if (table_) {
      return {PJ::WidgetInput("rows", "Rows", "", PJ::WidgetInputKind::kNumber, true)};
    }
    return {PJ::WidgetInput("value", "Value"), PJ::WidgetInput("state", "State", "", PJ::WidgetInputKind::kText)};
  }

  void* createWidget(void* parent) override {
    label_ = new QLabel(static_cast<QWidget*>(parent));
    label_->setText(QStringLiteral("no data"));
    return label_.data();
  }

  void onUpdate(PJ::Span<const PJ_widget_value_t> values, double /*time*/) override {
    if (!reported_ && !values.empty() && widgetHostBound()) {
      // Observable from outside the process: the diagnostics dump of the end-to-end run. Which
      // wires hold a sample yet depends on the playhead, so the message names the wires only.
      reported_ = true;
      std::string labels;
      for (const PJ_widget_value_t& value : values) {
        labels += (labels.empty() ? "" : ", ") + std::string(value.label.data, value.label.size);
      }
      widgetHost().reportWidgetMessage(PJ::WidgetMessageLevel::kInfo, "first update: " + labels);
    }
    QStringList parts;
    for (const PJ_widget_value_t& value : values) {
      const QString label = QString::fromUtf8(value.label.data, static_cast<qsizetype>(value.label.size));
      if (value.kind == PJ_WIDGET_VALUE_NUMBER) {
        parts << QStringLiteral("%1=%2").arg(label).arg(value.number);
      } else if (value.kind == PJ_WIDGET_VALUE_TEXT) {
        parts << QStringLiteral("%1=%2").arg(
            label, QString::fromUtf8(value.text.data, static_cast<qsizetype>(value.text.size)));
      }
    }
    if (label_ != nullptr) {
      label_->setText(prefix_ + parts.join(QStringLiteral("; ")));
    }
  }

  std::string saveConfig() const override {
    return config_;
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    config_ = std::string(config_json);
    const auto at = config_.find("\"prefix\":\"");
    prefix_.clear();
    if (at != std::string::npos) {
      const auto begin = at + 10;
      prefix_ = QString::fromStdString(config_.substr(begin, config_.find('"', begin) - begin));
    }
    return PJ::okStatus();
  }

 private:
  bool table_ = false;
  bool reported_ = false;
  std::string config_ = "{}";
  QString prefix_;
  QPointer<QLabel> label_;
};

}  // namespace

PJ_WIDGET_PLUGIN(TestWidgets, kManifest)
