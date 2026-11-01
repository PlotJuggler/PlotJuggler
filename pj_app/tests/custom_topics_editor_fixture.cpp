// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Test-only Toolbox plugin standing in for the Transform Editor: its manifest declares the
// `custom_topics_editor` capability, so the host makes it the Custom Topics editor, and its panel
// previews the topic `/cloud` in an embedded 3D scene view (the `scenePreview` frame's
// `scene_view` document). Its `droppedItems` label is a drop target that shows the names and data sources the
// last drop delivered, one per line, so a test reads what the plugin received. Consumed by
// main_window_editor_integration_test.

#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kDialogManifestJson =
    R"({"id":"custom-topics-editor-fixture-panel","name":"Editor Fixture Panel","version":"1.0.0"})";

constexpr const char* kUiContent = R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>EditorFixture</class>
 <widget class="QWidget" name="EditorFixture">
  <layout class="QVBoxLayout">
   <item><widget class="QFrame" name="scenePreview"/></item>
   <item>
    <widget class="QLabel" name="droppedItems">
     <property name="text"><string>drop here</string></property>
    </widget>
   </item>
  </layout>
 </widget>
</ui>
)";

}  // namespace

class EditorFixtureDialog : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return kDialogManifestJson;
  }
  std::string ui_content() const override {
    return kUiContent;
  }
  std::string widget_data() override {
    PJ::WidgetData data;
    data.setSceneView("scenePreview", PJ::SceneView{.kind = "3d", .topics = {{.topic = "/cloud", .dataset = {}}}});
    data.setDropTarget("droppedItems");
    if (!dropped_.empty()) {
      data.setText("droppedItems", dropped_);
    }
    return data.toJson();
  }
  bool onDroppedItems(std::string_view /*widget_name*/, const std::vector<DroppedItem>& items) override {
    dropped_.clear();
    for (const DroppedItem& item : items) {
      dropped_ += (dropped_.empty() ? "" : "\n") + item.name + "@" + std::to_string(item.data_source_id);
    }
    return true;
  }

 private:
  std::string dropped_;
};

class EditorFixtureToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog;
  }
  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

 private:
  EditorFixtureDialog dialog_;
};

PJ_TOOLBOX_PLUGIN(
    EditorFixtureToolbox, R"({"id":"custom-topics-editor-fixture","name":"Editor Fixture","version":"1.0.0",)"
                          R"("capabilities":["custom_topics_editor"]})")

PJ_DIALOG_PLUGIN(EditorFixtureDialog, kDialogManifestJson)
