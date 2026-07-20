// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Tests for the dialog widget binding: the PjUiLoader custom-widget vocabulary
// (RangeSlider, DateRangePicker), the RangeSlider data binding (bounds/values +
// duration labels), the generic field-validity indicator (setFieldValid), and
// the table sort-key suite (WidgetBindingTableSort — typed cells, rank ordering,
// ragged deliveries, sort indicator, header-click events).

#include <pj_widgets/ComboBox.h>
#include <pj_widgets/ComboBoxGradientDelegate.h>
#include <pj_widgets/CredentialsEditor.h>
#include <pj_widgets/DateRangePicker.h>
#include <pj_widgets/DualOptionsWidget.h>
#include <pj_widgets/RangeSlider.h>
#include <pj_widgets/Scrollbar.h>
#include <pj_widgets/ToggleSwitch.h>

#include <QAbstractScrollArea>
#include <QApplication>
#include <QBuffer>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpacerItem>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>
#include <limits>
#include <nlohmann/json.hpp>
#include <pj_plugins/host/widget_data_view.hpp>
#include <pj_plugins/host/widget_event_builder.hpp>
#include <pj_plugins/host_qt/pj_ui_loader.hpp>
#include <pj_plugins/host_qt/widget_adapters.hpp>
#include <pj_plugins/host_qt/widget_binding.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace {

QApplication* qapp() {
  static int argc = 0;
  static QApplication app(argc, nullptr);
  return &app;
}

struct Event {
  std::string name;
  std::string json;
};

// Wire connectWidgetSignals to a recorder so tests can assert what the plugin
// would have received.
std::vector<Event>* recorder() {
  static std::vector<Event> events;
  return &events;
}

// PjUiLoader resolves the host's custom widget classes by name; plain Qt classes
// fall through to the base QUiLoader.
TEST(PjUiLoader, RegistersBuildingBlocks) {
  qapp();
  const QByteArray ui = R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>Root</class>
 <widget class="QWidget" name="Root">
  <layout class="QVBoxLayout">
   <item><widget class="RangeSlider" name="rangeSlider"/></item>
   <item><widget class="DateRangePicker" name="datePicker"/></item>
   <item><widget class="CredentialsEditor" name="certContents"/></item>
   <item><widget class="PJ::ComboBox" name="pjCombo"/></item>
   <item><widget class="QLineEdit" name="plainEdit"/></item>
  </layout>
 </widget>
</ui>)";
  QByteArray data(ui);
  QBuffer buffer(&data);
  buffer.open(QIODevice::ReadOnly);
  PJ::PjUiLoader loader;
  QWidget* root = loader.load(&buffer);
  ASSERT_NE(root, nullptr);
  EXPECT_NE(root->findChild<PJ::RangeSlider*>("rangeSlider"), nullptr);
  EXPECT_NE(root->findChild<PJ::DateRangePicker*>("datePicker"), nullptr);
  EXPECT_NE(root->findChild<PJ::CredentialsEditor*>("certContents"), nullptr);
  // The canonical dropdown must come back as the real PJ::ComboBox (gradient
  // popup), not the plain QComboBox the base loader would create.
  EXPECT_NE(root->findChild<PJ::ComboBox*>("pjCombo"), nullptr);
  // The cert dialog addresses CredentialsEditor's inner inputs by name; they
  // must be reachable for the plugin's setText("certPath"/...) to land.
  EXPECT_NE(root->findChild<QLineEdit*>("certPath"), nullptr);
  EXPECT_NE(root->findChild<QLineEdit*>("apiKey"), nullptr);
  EXPECT_NE(root->findChild<QLineEdit*>("plainEdit"), nullptr);
  delete root;
}

// Bounds + handle values are applied, and a time span turns on the floating
// duration labels.
TEST(WidgetBindingRangeSlider, AppliesBoundsValuesAndTimeSpan) {
  qapp();
  QWidget root;
  auto* slider = new PJ::RangeSlider(Qt::Horizontal, PJ::RangeSlider::kDoubleHandles, &root);
  slider->setObjectName("rangeSlider");

  PJ::WidgetData wd;
  wd.setRangeSliderBounds("rangeSlider", 0, 1000);
  wd.setRangeSliderValues("rangeSlider", 200, 800);
  wd.setRangeSliderTimeSpan("rangeSlider", 0, 1'000'000'000'000LL);
  PJ::WidgetDataView view(wd.toJson());
  PJ::applyWidgetData(&root, view);

  EXPECT_EQ(slider->getMaximun(), 1000);
  EXPECT_EQ(slider->getLowerValue(), 200);
  EXPECT_EQ(slider->getUpperValue(), 800);
  EXPECT_TRUE(slider->floatingLabelsVisible());
}

// The plugin owns the rule and pushes {valid, tooltip}; the host renders the
// tooltip plus a red border when invalid, and clears it when valid.
TEST(WidgetBindingFieldValidity, RendersTooltipAndBackground) {
  qapp();
  QWidget root;
  auto* edit = new QLineEdit(&root);
  edit->setObjectName("apiKey");

  PJ::WidgetData bad;
  bad.setFieldValid("apiKey", false, "invalid key");
  PJ::applyWidgetData(&root, PJ::WidgetDataView(bad.toJson()));
  EXPECT_EQ(edit->toolTip().toStdString(), "invalid key");
  // PJ3 parity: invalid fields get a light-red background, not a border.
  EXPECT_TRUE(edit->styleSheet().contains("background-color")) << "invalid field should show a background cue";

  PJ::WidgetData good;
  good.setFieldValid("apiKey", true);
  PJ::applyWidgetData(&root, PJ::WidgetDataView(good.toJson()));
  EXPECT_TRUE(edit->styleSheet().isEmpty()) << "valid field should clear the cue";
}

// --- Editable QComboBox handling (generic; ported from gor/mosaico) ----------

TEST(WidgetBindingCombo, EditableComboForwardsTypedTextAsTextChanged) {
  qapp();
  recorder()->clear();
  QWidget root;
  auto* combo = new QComboBox(&root);
  combo->setObjectName("comboUri");
  combo->setEditable(true);

  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  combo->setEditText("grpc+tls://my.server:6726");

  // At least one event must carry the typed text under the "text" key so the
  // typed dispatcher routes it to onTextChanged.
  bool saw_text = false;
  for (const auto& ev : *recorder()) {
    if (ev.name != "comboUri") {
      continue;
    }
    auto j = nlohmann::json::parse(ev.json, nullptr, false);
    if (!j.is_discarded() && j.contains("text") && j["text"] == "grpc+tls://my.server:6726") {
      saw_text = true;
    }
  }
  EXPECT_TRUE(saw_text) << "editable combo edit-text must emit a text_changed event";
}

TEST(WidgetBindingCombo, EditableComboReflectsPluginText) {
  qapp();
  QWidget root;
  auto* combo = new QComboBox(&root);
  combo->setObjectName("comboUri");
  combo->setEditable(true);

  PJ::WidgetData wd;
  wd.setText("comboUri", "host.example:9999");
  PJ::WidgetDataView view(wd.toJson());
  PJ::applyWidgetData(&root, view);

  EXPECT_EQ(combo->currentText().toStdString(), "host.example:9999");
}

TEST(WidgetBindingCombo, NonEditableComboEmitsIndexNotText) {
  qapp();
  recorder()->clear();
  QWidget root;
  auto* combo = new QComboBox(&root);
  combo->setObjectName("mode");
  combo->addItems({"alpha", "beta", "gamma"});

  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  combo->setCurrentIndex(2);

  bool saw_index = false;
  for (const auto& ev : *recorder()) {
    if (ev.name != "mode") {
      continue;
    }
    auto j = nlohmann::json::parse(ev.json, nullptr, false);
    if (!j.is_discarded() && j.contains("current_index")) {
      saw_index = true;
      EXPECT_FALSE(j.contains("text")) << "non-editable combo must not masquerade as text";
    }
  }
  EXPECT_TRUE(saw_index) << "non-editable combo selection must emit an index event";
}

TEST(WidgetBindingCombo, IdenticalItemsPreserveTypedText) {
  // Re-applying the SAME item set must not clear an editable combo's line edit
  // (which would bounce the caret to the end while the user is mid-typing).
  qapp();
  QWidget root;
  auto* combo = new QComboBox(&root);
  combo->setObjectName("c");
  combo->setEditable(true);
  combo->addItems({"alpha", "beta"});
  combo->setCurrentText("user.typed.host:1");

  PJ::WidgetData wd;
  wd.setItems("c", {"alpha", "beta"});  // same items, no text field
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  EXPECT_EQ(combo->currentText().toStdString(), "user.typed.host:1");
}

TEST(WidgetBindingCombo, ChangedItemsRebuild) {
  qapp();
  QWidget root;
  auto* combo = new QComboBox(&root);
  combo->setObjectName("c");
  combo->setEditable(true);
  combo->addItems({"alpha"});

  PJ::WidgetData wd;
  wd.setItems("c", {"alpha", "beta", "gamma"});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  EXPECT_EQ(combo->count(), 3);
}

TEST(WidgetBindingRadioGroupAdapter, ConvertsSafePairAndPreservesRadioEvents) {
  qapp();
  recorder()->clear();

  QWidget root;
  auto* root_layout = new QVBoxLayout(&root);
  auto* row = new QWidget(&root);
  auto* row_layout = new QHBoxLayout(row);
  auto* frame = new QRadioButton(u"Frame"_s, row);
  frame->setObjectName("frameMode");
  frame->setChecked(true);
  auto* arrow = new QRadioButton(u"Arrow"_s, row);
  arrow->setObjectName("arrowMode");
  auto* group = new QButtonGroup(row);
  group->addButton(frame);
  group->addButton(arrow);
  row_layout->addWidget(frame);
  row_layout->addWidget(arrow);
  root_layout->addWidget(row);

  PJ::adaptRadioGroups(&root);

  auto* dual = row->findChild<PJ::DualOptionsWidget*>();
  ASSERT_NE(dual, nullptr);
  EXPECT_TRUE(frame->isHidden());
  EXPECT_TRUE(arrow->isHidden());
  EXPECT_EQ(dual->selectedIndex(), 0);

  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  dual->setSelectedIndex(1);

  EXPECT_TRUE(arrow->isChecked());
  EXPECT_FALSE(frame->isChecked());
  bool saw_arrow_checked = false;
  for (const auto& ev : *recorder()) {
    if (ev.name != "arrowMode") {
      continue;
    }
    const auto j = nlohmann::json::parse(ev.json, nullptr, false);
    saw_arrow_checked = !j.is_discarded() && j.value("checked", false);
  }
  EXPECT_TRUE(saw_arrow_checked) << "the hidden original radio button must still drive plugin onToggled callbacks";
}

TEST(WidgetBindingRadioGroupAdapter, WidgetDataSyncsVisibleDualOptionsWidget) {
  qapp();

  QWidget root;
  auto* row_layout = new QHBoxLayout(&root);
  auto* frame = new QRadioButton(u"Frame"_s, &root);
  frame->setObjectName("frameMode");
  frame->setChecked(true);
  auto* arrow = new QRadioButton(u"Arrow"_s, &root);
  arrow->setObjectName("arrowMode");
  auto* group = new QButtonGroup(&root);
  group->addButton(frame);
  group->addButton(arrow);
  row_layout->addWidget(frame);
  row_layout->addWidget(arrow);

  PJ::adaptRadioGroups(&root);
  auto* dual = root.findChild<PJ::DualOptionsWidget*>();
  ASSERT_NE(dual, nullptr);
  ASSERT_EQ(dual->selectedIndex(), 0);

  PJ::WidgetData wd;
  wd.setChecked("arrowMode", true);
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  EXPECT_TRUE(frame->isHidden());
  EXPECT_TRUE(arrow->isHidden());
  EXPECT_TRUE(arrow->isChecked());
  EXPECT_EQ(dual->selectedIndex(), 1);
}

TEST(WidgetBindingRadioGroupAdapter, ConvertsAfterInitialWidgetDataSelectsRadio) {
  qapp();

  QWidget root;
  auto* row_layout = new QHBoxLayout(&root);
  auto* frame = new QRadioButton(u"Frame"_s, &root);
  frame->setObjectName("frameMode");
  auto* arrow = new QRadioButton(u"Arrow"_s, &root);
  arrow->setObjectName("arrowMode");
  auto* group = new QButtonGroup(&root);
  group->addButton(frame);
  group->addButton(arrow);
  row_layout->addWidget(frame);
  row_layout->addWidget(arrow);

  PJ::adaptRadioGroups(&root);
  EXPECT_EQ(root.findChild<PJ::DualOptionsWidget*>(), nullptr)
      << "no-selection pairs stay untouched until plugin data chooses an option";

  PJ::WidgetData wd;
  wd.setChecked("arrowMode", true);
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  auto* dual = root.findChild<PJ::DualOptionsWidget*>();
  ASSERT_NE(dual, nullptr);
  EXPECT_TRUE(frame->isHidden());
  EXPECT_TRUE(arrow->isHidden());
  EXPECT_EQ(dual->selectedIndex(), 1);
}

TEST(WidgetBindingRadioGroupAdapter, ConvertsPairEmbeddedInMixedBoxRow) {
  qapp();

  QWidget root;
  auto* row_layout = new QHBoxLayout(&root);
  auto* label = new QLabel(u"Timestamp:"_s, &root);
  auto* publish = new QRadioButton(u"publish"_s, &root);
  publish->setObjectName("publishTimestamp");
  publish->setChecked(true);
  auto* log = new QRadioButton(u"log"_s, &root);
  log->setObjectName("logTimestamp");
  auto* group = new QButtonGroup(&root);
  group->addButton(publish);
  group->addButton(log);
  auto* header = new QCheckBox(u"Use timestamp inside message (header)"_s, &root);
  row_layout->addWidget(label);
  row_layout->addWidget(publish);
  row_layout->addWidget(log);
  row_layout->addStretch();
  row_layout->addWidget(header);

  PJ::adaptRadioGroups(&root);

  auto* dual = root.findChild<PJ::DualOptionsWidget*>();
  ASSERT_NE(dual, nullptr);
  EXPECT_TRUE(publish->isHidden());
  EXPECT_TRUE(log->isHidden());
  EXPECT_FALSE(label->isHidden());
  EXPECT_FALSE(header->isHidden());
  EXPECT_EQ(dual->selectedIndex(), 0);
}

TEST(WidgetBindingRadioGroupAdapter, LeavesUngroupedTwoRadioRowUntouched) {
  qapp();

  QWidget root;
  auto* row_layout = new QHBoxLayout(&root);
  auto* first = new QRadioButton(u"First"_s, &root);
  first->setChecked(true);
  auto* second = new QRadioButton(u"Second"_s, &root);
  row_layout->addWidget(first);
  row_layout->addWidget(second);

  PJ::adaptRadioGroups(&root);

  EXPECT_EQ(root.findChild<PJ::DualOptionsWidget*>(), nullptr);
  EXPECT_FALSE(first->isHidden());
  EXPECT_FALSE(second->isHidden());
}

TEST(WidgetBindingRadioGroupAdapter, ConvertsButtonGroupsInsideNestedLayouts) {
  qapp();

  QWidget root;
  auto* outer_layout = new QVBoxLayout(&root);

  auto* array_row = new QHBoxLayout();
  auto* spin = new QSpinBox(&root);
  auto* clamp = new QRadioButton(u"Clamp"_s, &root);
  auto* skip = new QRadioButton(u"Skip"_s, &root);
  auto* array_group = new QButtonGroup(&root);
  array_group->addButton(clamp);
  array_group->addButton(skip);
  skip->setChecked(true);
  array_row->addWidget(new QLabel(u"When an array size exceeds:"_s, &root));
  array_row->addWidget(spin);
  array_row->addStretch();
  array_row->addWidget(clamp);
  array_row->addWidget(skip);
  outer_layout->addLayout(array_row);

  auto* timestamp_row = new QHBoxLayout();
  auto* publish = new QRadioButton(u"publish"_s, &root);
  auto* log = new QRadioButton(u"log"_s, &root);
  auto* timestamp_group = new QButtonGroup(&root);
  timestamp_group->addButton(publish);
  timestamp_group->addButton(log);
  publish->setChecked(true);
  timestamp_row->addWidget(new QLabel(u"Timestamp:"_s, &root));
  timestamp_row->addWidget(publish);
  timestamp_row->addWidget(log);
  timestamp_row->addStretch();
  timestamp_row->addWidget(new QCheckBox(u"Use timestamp inside message (header)"_s, &root));
  outer_layout->addLayout(timestamp_row);

  PJ::adaptRadioGroups(&root);

  const auto duals = root.findChildren<PJ::DualOptionsWidget*>();
  ASSERT_EQ(duals.size(), 2);
  EXPECT_TRUE(clamp->isHidden());
  EXPECT_TRUE(skip->isHidden());
  EXPECT_TRUE(publish->isHidden());
  EXPECT_TRUE(log->isHidden());
}

TEST(WidgetBindingRadioGroupAdapter, ConvertsIndependentButtonGroupsSharingParent) {
  qapp();

  QWidget root;
  auto* row_layout = new QHBoxLayout(&root);
  auto* publish = new QRadioButton(u"publish"_s, &root);
  auto* log = new QRadioButton(u"log"_s, &root);
  auto* clamp = new QRadioButton(u"Clamp"_s, &root);
  auto* skip = new QRadioButton(u"Skip"_s, &root);
  auto* timestamp_group = new QButtonGroup(&root);
  timestamp_group->addButton(publish);
  timestamp_group->addButton(log);
  auto* overflow_group = new QButtonGroup(&root);
  overflow_group->addButton(clamp);
  overflow_group->addButton(skip);
  publish->setChecked(true);
  skip->setChecked(true);
  row_layout->addWidget(new QLabel(u"Timestamp:"_s, &root));
  row_layout->addWidget(publish);
  row_layout->addWidget(log);
  row_layout->addStretch();
  row_layout->addWidget(new QLabel(u"When an array size exceeds:"_s, &root));
  row_layout->addWidget(clamp);
  row_layout->addWidget(skip);

  PJ::adaptRadioGroups(&root);

  const auto duals = root.findChildren<PJ::DualOptionsWidget*>();
  ASSERT_EQ(duals.size(), 2);
  EXPECT_TRUE(publish->isHidden());
  EXPECT_TRUE(log->isHidden());
  EXPECT_TRUE(clamp->isHidden());
  EXPECT_TRUE(skip->isHidden());
}

TEST(WidgetBindingRadioGroupAdapter, ConvertsGroupedPairInsideGridRow) {
  qapp();

  QWidget root;
  auto* grid = new QGridLayout(&root);
  auto* spin = new QSpinBox(&root);
  auto* clamp = new QRadioButton(u"Clamp"_s, &root);
  auto* skip = new QRadioButton(u"Skip"_s, &root);
  skip->setChecked(true);
  auto* overflow_group = new QButtonGroup(&root);
  overflow_group->addButton(clamp);
  overflow_group->addButton(skip);
  grid->addWidget(new QLabel(u"When an array size exceeds:"_s, &root), 0, 0);
  grid->addWidget(spin, 0, 1);
  grid->addItem(
      new QSpacerItem(
          PJ::theme::space(PJ::theme::Space::Section), PJ::theme::space(PJ::theme::Space::Tight),
          QSizePolicy::Expanding, QSizePolicy::Minimum),
      0, 2);
  grid->addWidget(clamp, 0, 3);
  grid->addWidget(skip, 0, 4);
  grid->addWidget(new QCheckBox(u"Use timestamp inside message (header)"_s, &root), 1, 0, 1, 5);

  PJ::adaptRadioGroups(&root);

  auto* dual = root.findChild<PJ::DualOptionsWidget*>();
  ASSERT_NE(dual, nullptr);
  EXPECT_TRUE(clamp->isHidden());
  EXPECT_TRUE(skip->isHidden());
  EXPECT_EQ(dual->selectedIndex(), 1);
}

TEST(WidgetBindingRadioGroupAdapter, LeavesUngroupedLargerRadioSetUntouched) {
  qapp();

  QWidget root;
  auto* row_layout = new QHBoxLayout(&root);
  auto* a = new QRadioButton(u"A"_s, &root);
  a->setChecked(true);
  auto* b = new QRadioButton(u"B"_s, &root);
  auto* c = new QRadioButton(u"C"_s, &root);
  auto* d = new QRadioButton(u"D"_s, &root);
  row_layout->addWidget(a);
  row_layout->addWidget(b);
  row_layout->addWidget(c);
  row_layout->addWidget(d);

  PJ::adaptRadioGroups(&root);

  EXPECT_EQ(root.findChild<PJ::DualOptionsWidget*>(), nullptr);
  EXPECT_FALSE(a->isHidden());
  EXPECT_FALSE(b->isHidden());
  EXPECT_FALSE(c->isHidden());
  EXPECT_FALSE(d->isHidden());
}

TEST(WidgetBindingRadioGroupAdapter, ConvertsExplicitThreeButtonGroup) {
  qapp();

  QWidget root;
  auto* row_layout = new QHBoxLayout(&root);
  auto* contains = new QRadioButton(u"Contains"_s, &root);
  contains->setObjectName("filterContains");
  auto* wildcard = new QRadioButton(u"Wildcard"_s, &root);
  wildcard->setObjectName("filterWildcard");
  wildcard->setChecked(true);
  auto* regexp = new QRadioButton(u"RegExp"_s, &root);
  regexp->setObjectName("filterRegExp");
  auto* group = new QButtonGroup(&root);
  group->addButton(contains);
  group->addButton(wildcard);
  group->addButton(regexp);
  row_layout->addWidget(contains);
  row_layout->addWidget(wildcard);
  row_layout->addWidget(regexp);

  PJ::adaptRadioGroups(&root);

  auto* dual = root.findChild<PJ::DualOptionsWidget*>();
  ASSERT_NE(dual, nullptr);
  EXPECT_EQ(dual->optionCount(), 3);
  EXPECT_TRUE(contains->isHidden());
  EXPECT_TRUE(wildcard->isHidden());
  EXPECT_TRUE(regexp->isHidden());
  EXPECT_EQ(dual->selectedIndex(), 1);

  dual->setSelectedIndex(2);
  EXPECT_TRUE(regexp->isChecked());
  EXPECT_FALSE(wildcard->isChecked());

  PJ::WidgetData wd;
  wd.setChecked("filterContains", true);
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));
  EXPECT_EQ(dual->selectedIndex(), 0);
  EXPECT_TRUE(contains->isHidden());
}

TEST(WidgetCheckBoxAdapter, ConvertsCheckBoxToLabeledToggleAndPreservesEvents) {
  qapp();
  recorder()->clear();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* check = new QCheckBox(u"Enable streaming"_s, &root);
  check->setObjectName("enableStreaming");
  layout->addWidget(check);

  PJ::adaptCheckBoxes(&root);

  auto* toggle = root.findChild<PJ::ToggleSwitch*>();
  ASSERT_NE(toggle, nullptr);
  EXPECT_TRUE(check->isHidden());
  EXPECT_EQ(toggle->text(), u"Enable streaming"_s);
  EXPECT_EQ(toggle->labelSide(), PJ::ToggleSwitch::LabelSide::Left);
  EXPECT_FALSE(toggle->isChecked());

  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  // Flip the toggle; it emits `toggled` only once its slide animation settles
  // (iOS-switch semantics), so wait for it, then assert the hidden checkbox
  // followed AND drove the plugin callback.
  toggle->setChecked(true);
  QTest::qWait(300);

  EXPECT_TRUE(check->isChecked());
  bool saw_checked = false;
  for (const auto& ev : *recorder()) {
    if (ev.name != "enableStreaming") {
      continue;
    }
    const auto j = nlohmann::json::parse(ev.json, nullptr, false);
    saw_checked = !j.is_discarded() && j.value("checked", false);
  }
  EXPECT_TRUE(saw_checked) << "the hidden original checkbox must still drive plugin onToggled callbacks";
}

TEST(WidgetCheckBoxAdapter, WidgetDataSyncsToggle) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* check = new QCheckBox(u"Loop"_s, &root);
  check->setObjectName("loop");
  layout->addWidget(check);

  PJ::adaptCheckBoxes(&root);
  auto* toggle = root.findChild<PJ::ToggleSwitch*>();
  ASSERT_NE(toggle, nullptr);
  ASSERT_FALSE(toggle->isChecked());

  PJ::WidgetData wd;
  wd.setChecked("loop", true);
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  EXPECT_TRUE(check->isHidden());
  EXPECT_TRUE(check->isChecked());
  EXPECT_TRUE(toggle->isChecked());
}

TEST(WidgetCheckBoxAdapter, LeavesTristateCheckBoxUntouched) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* check = new QCheckBox(u"Partial"_s, &root);
  check->setTristate(true);
  layout->addWidget(check);

  PJ::adaptCheckBoxes(&root);

  EXPECT_EQ(root.findChild<PJ::ToggleSwitch*>(), nullptr);
  EXPECT_FALSE(check->isHidden());
}

TEST(WidgetCheckBoxAdapter, LeavesTextlessCheckBoxUntouched) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* check = new QCheckBox(&root);  // no label
  layout->addWidget(check);

  PJ::adaptCheckBoxes(&root);

  EXPECT_EQ(root.findChild<PJ::ToggleSwitch*>(), nullptr);
  EXPECT_FALSE(check->isHidden());
}

TEST(WidgetCheckBoxAdapter, LeavesHostCompositeInternalCheckBoxUntouched) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  // CredentialsEditor owns an internal "allow insecure" checkbox; the adapter
  // must treat composites as opaque and leave their internals alone.
  auto* creds = new PJ::CredentialsEditor(&root);
  layout->addWidget(creds);

  PJ::adaptCheckBoxes(&root);

  for (auto* cb : creds->findChildren<QCheckBox*>()) {
    EXPECT_EQ(cb->property("_pj_toggle_switch").value<QObject*>(), nullptr);
    EXPECT_FALSE(cb->isHidden());
  }
}

TEST(WidgetComboBoxAdapter, UpgradesPlainComboBoxInPlacePreservingState) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* combo = new QComboBox(&root);
  combo->setObjectName("mode");
  combo->addItems({u"a"_s, u"b"_s, u"c"_s});
  combo->setCurrentIndex(2);
  layout->addWidget(combo);

  PJ::adaptComboBoxes(&root);

  // Same widget object — model + current index survive (no swap).
  EXPECT_EQ(root.findChild<QComboBox*>("mode"), combo);
  EXPECT_EQ(combo->count(), 3);
  EXPECT_EQ(combo->currentIndex(), 2);
  // Gradient delegate now installed.
  EXPECT_NE(qobject_cast<PJ::ComboBoxGradientDelegate*>(combo->itemDelegate()), nullptr);
}

TEST(WidgetComboBoxAdapter, LeavesPromotedComboBoxStyled) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* combo = new PJ::ComboBox(&root);  // already promoted in the .ui
  layout->addWidget(combo);

  PJ::adaptComboBoxes(&root);

  // Still its own class, still gradient-styled (from its constructor).
  EXPECT_NE(qobject_cast<PJ::ComboBox*>(root.findChild<QComboBox*>()), nullptr);
  EXPECT_NE(qobject_cast<PJ::ComboBoxGradientDelegate*>(combo->itemDelegate()), nullptr);
}

// Full-width tabs contract (dexory_cloud_panel.ui "filterTabs"): a tab bar only
// gets the whole pane width in documentMode, and Qt's setDocumentMode(true)
// resets QTabBar::expanding to false during the .ui load — so the QTabWidget
// binding must re-assert expanding on apply or document-mode tabs silently
// stop stretching. Pins both halves of that sequence.
TEST(WidgetBindingTabWidget, DocumentModeSurvivesLoadAndApplyRestoresExpanding) {
  qapp();
  const QByteArray ui = R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>Root</class>
 <widget class="QWidget" name="Root">
  <layout class="QVBoxLayout">
   <item>
    <widget class="QTabWidget" name="filterTabs">
     <property name="documentMode"><bool>true</bool></property>
     <widget class="QWidget" name="basicTab">
      <attribute name="title"><string>Basic</string></attribute>
      <layout class="QVBoxLayout"/>
     </widget>
     <widget class="QWidget" name="advancedTab">
      <attribute name="title"><string>Advanced</string></attribute>
      <layout class="QVBoxLayout"/>
     </widget>
    </widget>
   </item>
  </layout>
 </widget>
</ui>)";
  QByteArray data(ui);
  QBuffer buffer(&data);
  buffer.open(QIODevice::ReadOnly);
  PJ::PjUiLoader loader;
  QWidget* root = loader.load(&buffer);
  ASSERT_NE(root, nullptr);
  auto* tabs = root->findChild<QTabWidget*>("filterTabs");
  ASSERT_NE(tabs, nullptr);
  EXPECT_TRUE(tabs->documentMode()) << "QUiLoader must honor the .ui documentMode property";
  EXPECT_FALSE(tabs->tabBar()->expanding()) << "precondition: setDocumentMode(true) resets expanding";
  EXPECT_TRUE(tabs->tabBar()->drawBase()) << "precondition: Qt defaults to drawing the tab-bar base";

  PJ::WidgetData wd;
  wd.setTabIndex("filterTabs", 1);
  PJ::applyWidgetData(root, PJ::WidgetDataView(wd.toJson()));

  EXPECT_TRUE(tabs->tabBar()->expanding()) << "apply must re-assert expanding after the documentMode reset";
  EXPECT_FALSE(tabs->tabBar()->drawBase())
      << "apply must drop the document-mode base line (stray line over the unselected tab)";
  EXPECT_EQ(tabs->currentIndex(), 1);
  delete root;
}

// --- adaptScrollAreas --------------------------------------------------------

// A QScrollArea under root gets one H + one V PJ::Scrollbar attached, the
// native bars are forced to AlwaysOff, and a second call produces no duplicates.
TEST(WidgetScrollAreaAdapter, AttachesHAndVScrollbarsAndHidesNativeBars) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* area = new QScrollArea(&root);
  auto* inner = new QWidget();
  inner->setMinimumSize(2000, 2000);
  area->setWidget(inner);
  layout->addWidget(area);

  PJ::adaptScrollAreas(&root);

  const auto scrollbars = root.findChildren<PJ::Scrollbar*>();
  ASSERT_EQ(scrollbars.size(), 2) << "expect one H + one V Scrollbar per area";

  EXPECT_EQ(area->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff)
      << "attach() must hide the native horizontal bar";
  EXPECT_EQ(area->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff) << "attach() must hide the native vertical bar";

  // Idempotent: a second call must not add more scrollbars.
  PJ::adaptScrollAreas(&root);
  EXPECT_EQ(root.findChildren<PJ::Scrollbar*>().size(), 2)
      << "pjScrollbarAttached marker must prevent duplicate overlays";
}

// A pjScrollbarAutoHide=false property on the area propagates to both pills,
// which must transition immediately to the shown state (full opacity).
TEST(WidgetScrollAreaAdapter, PropagatesAutoHideFalseConfig) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* area = new QScrollArea(&root);
  auto* inner = new QWidget();
  inner->setMinimumSize(2000, 2000);
  area->setWidget(inner);
  area->setProperty("pjScrollbarAutoHide", false);
  layout->addWidget(area);

  PJ::adaptScrollAreas(&root);

  const auto scrollbars = root.findChildren<PJ::Scrollbar*>();
  ASSERT_EQ(scrollbars.size(), 2);
  for (auto* sb : scrollbars) {
    EXPECT_TRUE(sb->isShown()) << "auto-hide=false must force the pill to the shown state immediately";
  }
}

// A plugin that pinned an axis to ScrollBarAlwaysOn wants a persistent native
// bar there; adaptScrollAreas must skip that axis (no pill) and leave its policy
// untouched, while still adapting the other (default) axis.
TEST(WidgetScrollAreaAdapter, RespectsAlwaysOnPolicyPerAxis) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* area = new QScrollArea(&root);
  auto* inner = new QWidget();
  inner->setMinimumSize(2000, 2000);
  area->setWidget(inner);
  area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);  // plugin wants the native V bar
  layout->addWidget(area);

  PJ::adaptScrollAreas(&root);

  EXPECT_EQ(root.findChildren<PJ::Scrollbar*>().size(), 1) << "only the horizontal (default) axis is adapted";
  EXPECT_EQ(area->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOn) << "pinned AlwaysOn vertical bar is left untouched";
  EXPECT_EQ(area->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff) << "default horizontal axis still gets a pill";
}

// setDateTime/setDateTimeRange land on a QDateTimeEdit (range first, so the
// value is not clamped by a stale default range).
TEST(WidgetBindingDateTime, AppliesValueAndRange) {
  qapp();
  QWidget root;
  auto* edit = new QDateTimeEdit(&root);
  edit->setObjectName("startTime");

  PJ::WidgetData wd;
  wd.setDateTime("startTime", "2026-05-21T13:45:00");
  wd.setDateTimeRange("startTime", "2026-05-01T00:00:00", "2026-06-01T00:00:00");
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  EXPECT_EQ(edit->dateTime(), QDateTime::fromString(u"2026-05-21T13:45:00"_s, Qt::ISODate));
  EXPECT_EQ(edit->minimumDateTime(), QDateTime::fromString(u"2026-05-01T00:00:00"_s, Qt::ISODate));
  EXPECT_EQ(edit->maximumDateTime(), QDateTime::fromString(u"2026-06-01T00:00:00"_s, Qt::ISODate));
}

// An edited QDateTimeEdit reports back as a datetime_iso event so the typed
// dispatcher routes it to onDateTimeChanged.
TEST(WidgetBindingDateTime, UserEditEmitsDateTimeIso) {
  qapp();
  recorder()->clear();
  QWidget root;
  auto* edit = new QDateTimeEdit(&root);
  edit->setObjectName("startTime");

  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  edit->setDateTime(QDateTime::fromString(u"2026-01-02T03:04:05"_s, Qt::ISODate));

  bool saw_datetime = false;
  for (const auto& ev : *recorder()) {
    if (ev.name != "startTime") {
      continue;
    }
    auto j = nlohmann::json::parse(ev.json, nullptr, false);
    if (!j.is_discarded() && j.contains("datetime_iso") && j["datetime_iso"] == "2026-01-02T03:04:05") {
      saw_datetime = true;
    }
  }
  EXPECT_TRUE(saw_datetime) << "QDateTimeEdit edit must emit a datetime_iso event";
}

// Editors whose display format carries milliseconds must round-trip them
// (the event serializes with ISODateWithMs; whole-second values stay bare).
TEST(WidgetBindingDateTime, MillisecondEditorEmitsFractionalSeconds) {
  qapp();
  recorder()->clear();
  QWidget root;
  auto* edit = new QDateTimeEdit(&root);
  edit->setObjectName("stamp");
  edit->setDisplayFormat(u"yyyy-MM-dd HH:mm:ss.zzz"_s);

  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  edit->setDateTime(QDateTime::fromString(u"2026-01-02T03:04:05.678"_s, Qt::ISODateWithMs));

  bool saw_ms = false;
  for (const auto& ev : *recorder()) {
    if (ev.name != "stamp") {
      continue;
    }
    auto j = nlohmann::json::parse(ev.json, nullptr, false);
    if (!j.is_discarded() && j.contains("datetime_iso") && j["datetime_iso"] == "2026-01-02T03:04:05.678") {
      saw_ms = true;
    }
  }
  EXPECT_TRUE(saw_ms) << "ms-precision editors must not truncate fractional seconds";
}

// --- Table sort keys (typed cells riding the dialog protocol) -----------------
//
// Every sort below runs through QTableWidgetItem::operator< — both
// QTableWidget::sortItems and a sorting-enabled header click end in
// QTableModel::sort's std::stable_sort. Qt implements DescendingOrder by
// swapping the operands of the SAME operator< (QTableModel::itemGreaterThan is
// `right < left`), so descending is the exact reverse of the ascending strict
// order — the keyless "text rank" flips from the bottom to the TOP of the view.

// Widget-data JSON for one table exactly as a plugin delivery produces it:
// display rows plus the sparse per-column sort keys. Returned dump()ed so every
// test round-trips the real C-ABI wire encoding (a string) — notably nlohmann's
// dump() serializes a NaN/Inf double as JSON null, so such a key ARRIVES at the
// host as a keyless cell.
std::string tableJson(
    const char* name, const std::vector<std::vector<std::string>>& rows,
    const nlohmann::json& column_values = nlohmann::json()) {
  nlohmann::json d;
  const std::size_t cols = rows.empty() ? 0 : rows.front().size();
  nlohmann::json headers = nlohmann::json::array();
  for (std::size_t c = 0; c < cols; ++c) {
    headers.push_back("H" + std::to_string(c));
  }
  d[name]["headers"] = headers;
  d[name]["rows"] = rows;
  if (!column_values.is_null()) {
    d[name]["column_values"] = column_values;
  }
  return d.dump();
}

// Top-to-bottom text of one column — the current view order after any sort.
std::vector<std::string> columnTexts(const QTableWidget* tw, int col) {
  std::vector<std::string> out;
  for (int r = 0; r < tw->rowCount(); ++r) {
    const QTableWidgetItem* item = tw->item(r, col);
    out.push_back(item != nullptr ? item->text().toStdString() : std::string());
  }
  return out;
}

// The reported repro: keyed cells must order by VALUE, not by rendered text —
// a text sort of "720","7","65" yields 65,7,720 ascending, which is the bug.
TEST(WidgetBindingTableSort, NumericKeysSortNumericallyBothDirections) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableJson("tbl", {{"720"}, {"7"}, {"65"}}, {{"0", {720, 7, 65}}})));
  ASSERT_EQ(tw->rowCount(), 3);

  tw->sortItems(0, Qt::AscendingOrder);
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"7", "65", "720"}));
  EXPECT_NE(columnTexts(tw, 0), (std::vector<std::string>{"65", "7", "720"})) << "must not be the string order";

  tw->sortItems(0, Qt::DescendingOrder);
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"720", "65", "7"}));
}

// An old plugin sends no column_values: legacy lexicographic text order must be
// unchanged (the escape hatch third-party text-only tables rely on).
TEST(WidgetBindingTableSort, NoColumnValuesKeepsLexicographicOrder) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableJson("tbl", {{"720"}, {"7"}, {"65"}})));
  ASSERT_EQ(tw->rowCount(), 3);

  tw->sortItems(0, Qt::AscendingOrder);
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"65", "7", "720"}));
}

// Hidden key: identical-looking date texts sort by their int64 keys, so a
// column can display one thing and order by another (Mosaico's Date column).
TEST(WidgetBindingTableSort, HiddenKeysOverrideTextOrder) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  // Keys deliberately disagree with the texts' lexicographic order.
  const std::string wire = tableJson("tbl", {{"2026-07-01"}, {"2026-06-30"}, {"2026-07-02"}}, {{"0", {300, 100, 200}}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wire));
  ASSERT_EQ(tw->rowCount(), 3);

  tw->sortItems(0, Qt::AscendingOrder);
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"2026-06-30", "2026-07-02", "2026-07-01"}));

  tw->sortItems(0, Qt::DescendingOrder);
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"2026-07-01", "2026-07-02", "2026-06-30"}));
}

// int64 exactness: these two keys collapse to the SAME double, so a comparator
// (or JSON decode) that coerces through double would tie them and stable_sort
// would keep the delivered a,b order — only an exact integer compare gives b,a.
TEST(WidgetBindingTableSort, Int64KeysCompareExactlyWithoutDoubleCoercion) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  const std::string wire = tableJson("tbl", {{"a"}, {"b"}}, {{"0", {1780000000000000124ULL, 1780000000000000123ULL}}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wire));
  ASSERT_EQ(tw->rowCount(), 2);

  tw->sortItems(0, Qt::AscendingOrder);
  // ...123 < ...124 exactly; a double tie (or a text compare "a" < "b") would both leave a,b.
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"b", "a"}));
}

// Cross-signedness: the wire decodes 2^64-1 as uint64 and -5 as int64 in ONE
// column; std::cmp_less must order them exactly (an int64 cast would wrap the
// max to -1, a uint64 cast would wrap -5 huge — both misplace a row).
TEST(WidgetBindingTableSort, CrossSignednessIntegerCompareIsExact) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  const std::string wire = tableJson("tbl", {{"umax"}, {"neg"}, {"three"}}, {{"0", {18446744073709551615ULL, -5, 3}}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wire));
  ASSERT_EQ(tw->rowCount(), 3);

  tw->sortItems(0, Qt::AscendingOrder);
  // Exact order -5 < 3 < 2^64-1. An int64-wrapped compare would give neg,umax,three;
  // a uint64-wrapped one three,neg,umax; a text compare max,neg,three.
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"neg", "three", "umax"}));
}

// Keyless cells among keyed ones (ulog "N/A"): null keys take the text rank,
// which sits after every number ascending — and FIRST descending, because Qt
// reverses by swapping operator<'s operands, flipping the rank order wholesale.
TEST(WidgetBindingTableSort, KeylessCellsRankAfterNumbersAscendingFirstDescending) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  const std::string wire = tableJson("tbl", {{"N/A"}, {"10"}, {"5"}, {"Aardvark"}}, {{"0", {nullptr, 10, 5, nullptr}}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wire));
  ASSERT_EQ(tw->rowCount(), 4);

  tw->sortItems(0, Qt::AscendingOrder);
  // Numbers by value first, then the keyless cells ordered among themselves by text.
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"5", "10", "Aardvark", "N/A"}));

  tw->sortItems(0, Qt::DescendingOrder);
  // Exact reverse of the ascending strict order: keyless (reverse text) first, then numbers descending.
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"N/A", "Aardvark", "10", "5"}));
}

// Wire fidelity: a NaN sort key does not survive dump() (it serializes as JSON
// null), so it must behave as a KEYLESS cell — interleaving with other keyless
// cells by text — not as the comparator's dedicated NaN rank.
TEST(WidgetBindingTableSort, NanKeyArrivesAsNullAndSortsInTextRank) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  const nlohmann::json cols = {{"0", {std::numeric_limits<double>::quiet_NaN(), nullptr, 5.0}}};
  const std::string wire = tableJson("tbl", {{"zz-nan"}, {"aa"}, {"5"}}, cols);
  // Premise: the dumped wire carries null where the NaN was — no NaN reaches the host.
  ASSERT_TRUE(nlohmann::json::parse(wire)["tbl"]["column_values"]["0"][0].is_null());

  PJ::applyWidgetData(&root, PJ::WidgetDataView(wire));
  ASSERT_EQ(tw->rowCount(), 3);

  tw->sortItems(0, Qt::AscendingOrder);
  // An in-process NaN would hold its own rank BEFORE the keyless cells ("5","zz-nan","aa");
  // off the wire it is keyless, so it orders by text among the keyless cells.
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"5", "aa", "zz-nan"}));
}

// A column mixing integer and float keys is rejected wholesale (no exact order
// exists across uint64 and double), so the WHOLE column falls back to text.
TEST(WidgetBindingTableSort, MixedIntAndFloatColumnFallsBackToTextOrder) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableJson("tbl", {{"2.5"}, {"10"}, {"3"}}, {{"0", {2.5, 10, 3}}})));
  ASSERT_EQ(tw->rowCount(), 3);

  tw->sortItems(0, Qt::AscendingOrder);
  // Text order, NOT the numeric 2.5,3,10 — the mixed column's keys must be ignored.
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"10", "2.5", "3"}));
}

// A values array whose length disagrees with the row count is dropped (a
// partial key column would sort some rows by number, others by text).
TEST(WidgetBindingTableSort, CountMismatchDropsColumnKeys) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableJson("tbl", {{"720"}, {"7"}, {"65"}}, {{"0", {720, 7}}})));
  ASSERT_EQ(tw->rowCount(), 3);

  tw->sortItems(0, Qt::AscendingOrder);
  // Two keys for three rows ⇒ column ignored ⇒ lexicographic text order.
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"65", "7", "720"}));
}

// Same-shape redelivery that ADDS keys must stamp them onto the existing items
// in place (no rebuild), flip the column to numeric ordering, and keep the
// per-row plugin-index role mapping index-keyed aspects through a sorted view.
TEST(WidgetBindingTableSort, SameShapeRedeliveryAddsKeysAndPreservesRowRoles) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  // First delivery: no keys — sorting is lexicographic.
  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableJson("tbl", {{"720"}, {"7"}, {"65"}})));
  ASSERT_EQ(tw->rowCount(), 3);
  tw->sortItems(0, Qt::AscendingOrder);
  ASSERT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"65", "7", "720"}));
  QTableWidgetItem* top_item = tw->item(0, 0);
  ASSERT_NE(top_item, nullptr);

  // Same shape redelivered WITH keys: cells rewritten in place (same item
  // pointers), so keys land on the items the user's selection/scroll live on.
  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableJson("tbl", {{"720"}, {"7"}, {"65"}}, {{"0", {720, 7, 65}}})));
  ASSERT_EQ(tw->rowCount(), 3);
  EXPECT_EQ(tw->item(0, 0), top_item) << "same-shape redelivery must reuse the existing items";
  // Sorting is off, so the rewrite leaves the table in plugin delivery order.
  ASSERT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"720", "7", "65"}));

  tw->sortItems(0, Qt::AscendingOrder);
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"7", "65", "720"})) << "keys must now drive the order";

  // Plugin row 0 ("720") sits at view row 2 after the sort; index-keyed
  // selection must land there via the kPluginRowRole tags restamped above.
  nlohmann::json sel;
  sel["tbl"]["selected_rows"] = {0};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(sel.dump()));
  EXPECT_FALSE(tw->item(0, 0)->isSelected());
  EXPECT_FALSE(tw->item(1, 0)->isSelected());
  EXPECT_TRUE(tw->item(2, 0)->isSelected());
}

// A .ui-declared table starts with PLAIN QTableWidgetItems; the first delivery
// must upgrade them to typed cells without losing the roles/flags they carried,
// and the upgraded items must sort by key and keep plugin-index translation.
TEST(WidgetBindingTableSort, PlainUiItemsUpgradeToTypedPreservingRolesAndFlags) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setRowCount(3);
  tw->setColumnCount(1);
  tw->setItem(0, 0, new QTableWidgetItem(u"b"_s));
  tw->setItem(1, 0, new QTableWidgetItem(u"c"_s));
  tw->setItem(2, 0, new QTableWidgetItem(u"a"_s));
  tw->item(0, 0)->setToolTip(u"keep"_s);
  tw->item(0, 0)->setFlags(tw->item(0, 0)->flags() & ~Qt::ItemIsDragEnabled);

  // Same shape as the pre-declared cells ⇒ the in-place upgrade path runs.
  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableJson("tbl", {{"b"}, {"c"}, {"a"}}, {{"0", {30, 10, 20}}})));
  ASSERT_EQ(tw->rowCount(), 3);
  EXPECT_EQ(tw->item(0, 0)->toolTip(), u"keep"_s) << "upgrade must copy the roles the plain item carried";
  EXPECT_FALSE(tw->item(0, 0)->flags().testFlag(Qt::ItemIsDragEnabled)) << "upgrade must copy the plain item's flags";

  tw->sortItems(0, Qt::AscendingOrder);
  // Keys 10,20,30 disagree with text order a,b,c — the upgraded cells must follow the keys.
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"c", "a", "b"}));

  // Plugin row 0 ("b") is at view row 2 after the sort; visible_rows is keyed by
  // plugin index, so only that view row may stay visible.
  nlohmann::json vis;
  vis["tbl"]["visible_rows"] = {0};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(vis.dump()));
  EXPECT_TRUE(tw->isRowHidden(0));
  EXPECT_TRUE(tw->isRowHidden(1));
  EXPECT_FALSE(tw->isRowHidden(2));
}

// sort_indicator is purely cosmetic: it draws the arrow for a plugin-sorted
// table (Qt only paints one when its own sorting is on) and must NOT reorder
// the delivered rows.
TEST(WidgetBindingTableSort, SortIndicatorAppliedCosmeticallyWithoutReordering) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  nlohmann::json d = nlohmann::json::parse(tableJson("tbl", {{"b", "2"}, {"a", "1"}, {"c", "3"}}));
  d["tbl"]["sort_indicator"] = {{"col", 1}, {"asc", false}};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));
  ASSERT_EQ(tw->rowCount(), 3);

  auto* header = tw->horizontalHeader();
  EXPECT_TRUE(header->isSortIndicatorShown());
  EXPECT_EQ(header->sortIndicatorSection(), 1);
  EXPECT_EQ(header->sortIndicatorOrder(), Qt::DescendingOrder);
  // The plugin's delivered order is the truth — a real descending sort of column
  // 1 would show c,b,a, so an unchanged b,a,c proves the arrow changed nothing.
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"b", "a", "c"}));
}

// A real user click on a header section must reach the plugin as a
// headerClicked(section) event — the wiring Mosaico's onHeaderClicked sorting
// depends on — while a sorting-disabled table's rows stay untouched.
TEST(WidgetBindingTableSort, HeaderClickEmitsHeaderClickedEvent) {
  qapp();
  recorder()->clear();
  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  layout->addWidget(tw);

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableJson("tbl", {{"b", "2"}, {"a", "1"}})));
  ASSERT_EQ(tw->rowCount(), 2);
  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  root.resize(500, 400);
  root.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&root));

  auto* header = tw->horizontalHeader();
  ASSERT_EQ(header->count(), 2);
  ASSERT_GT(header->sectionSize(1), 0);
  const QPoint pos(header->sectionViewportPosition(1) + header->sectionSize(1) / 2, header->height() / 2);
  ASSERT_TRUE(header->viewport()->rect().contains(pos)) << "click point must land inside section 1";
  QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier, pos);

  // The click also selects the column (Qt's default when sorting is off), so the
  // recorder may hold a selection event too — find the header event among them.
  bool saw_header_event = false;
  for (const auto& ev : *recorder()) {
    if (ev.name == "tbl" && ev.json == PJ::WidgetEventBuilder::headerClicked(1)) {
      saw_header_event = true;
    }
  }
  EXPECT_TRUE(saw_header_event) << "sectionClicked must reach the plugin as WidgetEventBuilder::headerClicked(1)";
  // Sorting is not enabled, so the click must not reorder the plugin's rows.
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"b", "a"}));
}

// Qt's REAL sorting path: setSortingEnabled(true) plus an actual header click
// must both route through TypedTableItem::operator< and order numerically.
TEST(WidgetBindingTableSort, RealHeaderClickSortsTypedColumnThroughQt) {
  qapp();
  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  layout->addWidget(tw);

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableJson("tbl", {{"720"}, {"7"}, {"65"}}, {{"0", {720, 7, 65}}})));
  ASSERT_EQ(tw->rowCount(), 3);

  auto* header = tw->horizontalHeader();
  // Pin a deterministic starting indicator: nothing is connected yet (sorting
  // still off), so this must not reorder anything by itself.
  header->setSortIndicator(0, Qt::DescendingOrder);
  ASSERT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"720", "7", "65"}));

  // Enabling sorting makes Qt sort immediately by the current indicator
  // (documented QTableView behavior) — numeric descending, not text descending.
  tw->setSortingEnabled(true);
  ASSERT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"720", "65", "7"}));

  root.resize(500, 400);
  root.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&root));
  ASSERT_GT(header->sectionSize(0), 0);
  const QPoint pos(header->sectionViewportPosition(0) + header->sectionSize(0) / 2, header->height() / 2);
  ASSERT_TRUE(header->viewport()->rect().contains(pos)) << "click point must land inside section 0";

  // A click on the already-indicated section flips the order (Qt's
  // flipSortIndicator), so Descending becomes Ascending and Qt re-sorts.
  QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier, pos);
  EXPECT_EQ(header->sortIndicatorOrder(), Qt::AscendingOrder);
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"7", "65", "720"}))
      << "the real click path must sort by the numeric keys, not the texts";
}

// A ragged typed delivery (a row shorter than the table) over .ui-declared
// plain cells must not leave the omitted cell as a PLAIN item beside typed
// neighbours: plain↔typed pairs compare by text while typed↔typed pairs compare
// by value, and one column mixing both is not a strict weak ordering (UB inside
// Qt's stable_sort). The omitted cell is blanked to a keyless typed cell, which
// sorts in the text rank.
TEST(WidgetBindingTableSort, RaggedRowsBlankOmittedCellsInsteadOfMixingComparators) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setRowCount(3);
  tw->setColumnCount(1);
  tw->setItem(0, 0, new QTableWidgetItem(u"5"_s));
  tw->setItem(1, 0, new QTableWidgetItem(u"3"_s));  // the cell the ragged delivery omits
  tw->setItem(2, 0, new QTableWidgetItem(u"20"_s));

  // Same shape (3 rows, first row 1 wide) ⇒ the in-place path; row 1 is empty.
  // The SDK emits null for a missing cell, so the key column stays 3 long.
  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableJson("tbl", {{"5"}, {}, {"20"}}, {{"0", {5, nullptr, 20}}})));
  ASSERT_EQ(tw->rowCount(), 3);
  ASSERT_NE(tw->item(1, 0), nullptr);
  EXPECT_EQ(tw->item(1, 0)->text(), QString()) << "the omitted cell is blanked, not left with stale .ui text";

  tw->sortItems(0, Qt::AscendingOrder);
  // Keyed 5 < 20 numerically; the blanked keyless cell sorts in the text rank after them.
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"5", "20", ""}));
}

// A delivery that changes the table's WIDTH with an unchanged row count takes
// the rebuild path, where setRowCount is a no-op — cells the new rows do not
// cover must be dropped, or they keep stale text, stale sort keys, and a stale
// plugin-row tag that corrupts the view<->plugin mapping every index-keyed
// aspect (selection, visibility, radio) depends on.
TEST(WidgetBindingTableSort, WidthChangeRebuildDropsStaleCellsAndRowTags) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableJson("tbl", {{"a", "1"}, {"b", "2"}})));
  ASSERT_EQ(tw->rowCount(), 2);
  ASSERT_EQ(tw->columnCount(), 2);

  // Rows only (no headers aspect): same row count but narrower rows — row 0
  // delivers nothing at all, row 1 a single cell.
  nlohmann::json d2;
  d2["tbl"]["rows"] = nlohmann::json::array({nlohmann::json::array(), nlohmann::json::array({"new"})});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d2.dump()));

  EXPECT_EQ(tw->item(0, 0), nullptr) << "cells from the wider shape must not survive the rebuild";
  EXPECT_EQ(tw->item(0, 1), nullptr);
  ASSERT_NE(tw->item(1, 0), nullptr);
  EXPECT_EQ(tw->item(1, 0)->text(), u"new"_s);
  EXPECT_EQ(tw->item(1, 1), nullptr);

  // Index-keyed aspects follow the surviving tags: plugin row 1 is view row 1;
  // the itemless row 0 falls back to its own index and is simply hidden.
  nlohmann::json vis;
  vis["tbl"]["visible_rows"] = {1};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(vis.dump()));
  EXPECT_TRUE(tw->isRowHidden(0));
  EXPECT_FALSE(tw->isRowHidden(1));
}

// Programmatic selection restore must not read back as user input: applying
// selected_rows / selected_items emits NO selectionChanged event to the plugin.
// applyToWidget's widget-wide QSignalBlocker is what guarantees this — without
// it, clearSelection() would fire a transiently EMPTY selection mid-apply, and
// a plugin that treats an empty selection as authoritative (Mosaico) would
// destroy its own selection state while its delivery is still being applied.
// This test pins that guarantee against the blocker ever being removed.
TEST(WidgetBindingTableSort, ProgrammaticSelectionRestoreEmitsNoEvents) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  recorder()->clear();
  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  nlohmann::json d = nlohmann::json::parse(tableJson("tbl", {{"a"}, {"b"}, {"c"}}));
  d["tbl"]["selected_rows"] = {1};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));
  ASSERT_EQ(tw->selectionModel()->selectedRows().size(), 1);
  EXPECT_TRUE(recorder()->empty()) << "index-keyed restore leaked selection events to the plugin";

  nlohmann::json d2 = nlohmann::json::parse(tableJson("tbl", {{"a"}, {"b"}, {"c"}}));
  d2["tbl"]["selected_items"] = {"c"};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d2.dump()));
  EXPECT_TRUE(recorder()->empty()) << "text-keyed restore leaked selection events to the plugin";
}

// Keys are sized by the WIDEST row, not the first: a delivery whose first row is
// a short spanning row ("Totals") must not silently drop every later column's
// keys and revert those columns to text order.
TEST(WidgetBindingTableSort, KeysBeyondFirstRowWidthStillApply) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  nlohmann::json d;
  d["tbl"]["headers"] = {"H0", "H1"};
  d["tbl"]["rows"] = nlohmann::json::array(
      {nlohmann::json::array({"Totals"}), nlohmann::json::array({"a", "9"}), nlohmann::json::array({"b", "10"})});
  d["tbl"]["column_values"]["1"] = {nullptr, 9, 10};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));
  ASSERT_EQ(tw->columnCount(), 2);

  tw->sortItems(1, Qt::AscendingOrder);
  // 9 < 10 numerically (text order would put "10" first); the "Totals" row has
  // no column-1 item at all, and Qt keeps itemless rows at the end of the sort.
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"a", "b", "Totals"}));
  EXPECT_EQ(columnTexts(tw, 1), (std::vector<std::string>{"9", "10", ""}));
}

// QTableWidget::setItem does not range-check the column: an overflow cell of a
// row wider than the table would land in the NEXT row via the flattened index
// (overwriting its first cell and plugin-row tag). Overflow cells are dropped.
TEST(WidgetBindingTableSort, RowsWiderThanTableAreClamped) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  nlohmann::json d;
  d["tbl"]["headers"] = {"H0", "H1"};
  d["tbl"]["rows"] =
      nlohmann::json::array({nlohmann::json::array({"a", "1", "x"}), nlohmann::json::array({"b", "2", "y"})});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));

  ASSERT_EQ(tw->columnCount(), 2);
  ASSERT_EQ(tw->rowCount(), 2);
  ASSERT_NE(tw->item(1, 0), nullptr);
  EXPECT_EQ(tw->item(1, 0)->text(), u"b"_s) << "an overflow write would have overwritten this with \"x\"";
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"a", "b"}));
}

// Qt's click handling flips the visible sort arrow before sectionClicked fires,
// even with sorting off. A click the plugin ignores must not leave the arrow on
// the clicked section: the binding re-asserts the plugin-delivered indicator.
TEST(WidgetBindingTableSort, IgnoredHeaderClickRestoresDeliveredIndicator) {
  qapp();
  recorder()->clear();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  nlohmann::json d = nlohmann::json::parse(tableJson("tbl", {{"a", "1"}, {"b", "2"}}));
  d["tbl"]["sort_indicator"] = {{"col", 1}, {"asc", false}};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));

  auto* header = tw->horizontalHeader();
  ASSERT_EQ(header->sortIndicatorSection(), 1);
  ASSERT_EQ(header->sortIndicatorOrder(), Qt::DescendingOrder);

  root.resize(400, 300);
  root.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&root));
  ASSERT_GT(header->sectionSize(0), 0);
  const QPoint pos(header->sectionViewportPosition(0) + header->sectionSize(0) / 2, header->height() / 2);
  QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier, pos);

  ASSERT_FALSE(recorder()->empty()) << "the click must still reach the plugin";
  EXPECT_EQ(header->sortIndicatorSection(), 1) << "an ignored click must not move the plugin-owned arrow";
  EXPECT_EQ(header->sortIndicatorOrder(), Qt::DescendingOrder);
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"a", "b"})) << "rows must not move";
}

}  // namespace

// --- Batch table deltas (SDK table_delta): seq-gated append/update/remove ---

TEST(WidgetBindingTableDelta, AppliesUpdateRemoveAppendOncePerSeq) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::WidgetData seed;
  seed.setTableHeaders("tbl", {"a", "b"});
  seed.setTableRows("tbl", std::vector<std::vector<std::string>>{{"r0a", "r0b"}, {"r1a", "r1b"}, {"r2a", "r2b"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(seed.toJson()));
  ASSERT_EQ(tw->rowCount(), 3);

  PJ::WidgetData wd;
  wd.updateTableCells("tbl", 1, {{0, 1, "UPD"}});
  wd.removeTableRows("tbl", 1, {1});
  wd.appendTableRows("tbl", 1, std::vector<std::vector<std::string>>{{"r3a", "r3b"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  ASSERT_EQ(tw->rowCount(), 3);  // 3 - 1 + 1
  EXPECT_EQ(tw->item(0, 1)->text(), u"UPD"_s);
  EXPECT_EQ(tw->item(1, 0)->text(), u"r2a"_s);  // row 1 removed, r2 shifted up
  EXPECT_EQ(tw->item(2, 0)->text(), u"r3a"_s);  // appended

  // Re-delivering the same seq (full-state rebuild still carrying it): no-op.
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));
  EXPECT_EQ(tw->rowCount(), 3);
  EXPECT_EQ(tw->item(0, 1)->text(), u"UPD"_s);

  // A different seq applies again, and plugin row space stayed consistent:
  // selecting plugin row 2 lands on the appended row.
  PJ::WidgetData wd2;
  wd2.appendTableRows("tbl", 2, std::vector<std::vector<std::string>>{{"r4a", "r4b"}});
  wd2.setSelectedRows("tbl", {2});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd2.toJson()));
  ASSERT_EQ(tw->rowCount(), 4);
  EXPECT_EQ(tw->item(3, 0)->text(), u"r4a"_s);
  ASSERT_NE(tw->item(2, 0), nullptr);
  EXPECT_TRUE(tw->item(2, 0)->isSelected());
}

TEST(WidgetBindingTableDelta, RowsInSameRefreshWinsAndConsumesSeq) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::WidgetData seed;
  seed.setTableHeaders("tbl", {"a", "b"});
  seed.setTableRows("tbl", std::vector<std::vector<std::string>>{{"r0a", "r0b"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(seed.toJson()));

  PJ::WidgetData wd;
  wd.setTableRows("tbl", std::vector<std::vector<std::string>>{{"x", "y"}});
  wd.appendTableRows("tbl", 5, std::vector<std::vector<std::string>>{{"z", "w"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));
  EXPECT_EQ(tw->rowCount(), 1);  // full replace won; delta consumed

  // The consumed seq must not fire later without rows.
  PJ::WidgetData wd2;
  wd2.appendTableRows("tbl", 5, std::vector<std::vector<std::string>>{{"z", "w"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd2.toJson()));
  EXPECT_EQ(tw->rowCount(), 1);
}

TEST(WidgetBindingTableDelta, EmptyAppendedRowStillCreatesTaggedItems) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::WidgetData seed;
  seed.setTableHeaders("tbl", {"a", "b"});
  seed.setTableRows("tbl", std::vector<std::vector<std::string>>{{"r0a", "r0b"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(seed.toJson()));

  PJ::WidgetData wd;
  wd.appendTableRows("tbl", 1, std::vector<std::vector<std::string>>{{}, {"r2a", "r2b"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  ASSERT_EQ(tw->rowCount(), 3);
  // The empty row must still carry items (empty text) so the plugin-row tag
  // exists and sorting cannot desync the row-identity mapping.
  ASSERT_NE(tw->item(1, 0), nullptr);
  EXPECT_TRUE(tw->item(1, 0)->text().isEmpty());
  EXPECT_EQ(tw->item(2, 0)->text(), u"r2a"_s);
}

TEST(WidgetBindingTableDelta, MalformedDeltaDoesNotConsumeSeq) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::WidgetData seed;
  seed.setTableHeaders("tbl", {"a", "b"});
  seed.setTableRows("tbl", std::vector<std::vector<std::string>>{{"r0a", "r0b"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(seed.toJson()));

  // Malformed op (negative index) with a fresh seq: rejected whole, and the
  // seq must NOT be recorded as consumed.
  PJ::applyWidgetData(&root, PJ::WidgetDataView(R"({"tbl": {"table_delta": {"seq": 9, "remove_rows": [-1]}}})"));
  EXPECT_EQ(tw->rowCount(), 1);

  // A corrected retransmission with the SAME seq must apply.
  PJ::WidgetData retry;
  retry.appendTableRows("tbl", 9, std::vector<std::vector<std::string>>{{"r1a", "r1b"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(retry.toJson()));
  EXPECT_EQ(tw->rowCount(), 2);
}

TEST(WidgetBindingTableDelta, UnresolvableOpRejectsWholeDeltaAndKeepsSeq) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::WidgetData seed;
  seed.setTableHeaders("tbl", {"a", "b"});
  seed.setTableRows("tbl", std::vector<std::vector<std::string>>{{"r0a", "r0b"}, {"r1a", "r1b"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(seed.toJson()));

  // Decode-valid delta whose remove targets a row the table does not have:
  // rejected whole — the valid update in the same delta must NOT land either.
  PJ::WidgetData wd;
  wd.updateTableCells("tbl", 7, {{0, 0, "UPD"}});
  wd.removeTableRows("tbl", 7, {5});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));
  EXPECT_EQ(tw->rowCount(), 2);
  EXPECT_EQ(tw->item(0, 0)->text(), u"r0a"_s);

  // The rejected seq was not consumed: a corrected retransmission of the SAME
  // seq must apply.
  PJ::WidgetData retry;
  retry.updateTableCells("tbl", 7, {{0, 0, "UPD"}});
  retry.removeTableRows("tbl", 7, {1});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(retry.toJson()));
  EXPECT_EQ(tw->rowCount(), 1);
  EXPECT_EQ(tw->item(0, 0)->text(), u"UPD"_s);
}

TEST(WidgetBindingTableDelta, RowsResyncResetsSeqGate) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::WidgetData seed;
  seed.setTableHeaders("tbl", {"a", "b"});
  seed.setTableRows("tbl", std::vector<std::vector<std::string>>{{"r0a", "r0b"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(seed.toJson()));

  PJ::WidgetData wd;
  wd.appendTableRows("tbl", 1, std::vector<std::vector<std::string>>{{"r1a", "r1b"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));
  ASSERT_EQ(tw->rowCount(), 2);

  // Full-rows resync (producer restarted), then its first delta reuses an
  // already-seen seq value: the resync must have reset the gate.
  PJ::WidgetData resync;
  resync.setTableRows("tbl", std::vector<std::vector<std::string>>{{"s0a", "s0b"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(resync.toJson()));
  ASSERT_EQ(tw->rowCount(), 1);

  PJ::WidgetData restarted;
  restarted.appendTableRows("tbl", 1, std::vector<std::vector<std::string>>{{"n1a", "n1b"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(restarted.toJson()));
  EXPECT_EQ(tw->rowCount(), 2);
  EXPECT_EQ(tw->item(1, 0)->text(), u"n1a"_s);
}

// A row appended via the delta path into an already-typed column must carry
// its own sort key, not become a plain (keyless) cell — the reported values
// (65 first, 7 appended) deliberately disagree with text order ("65" < "7"
// lexically) so a bug that drops the append's key and falls back to text
// would sort the rows the WRONG way and this test would fail either way.
TEST(WidgetBindingTableDelta, AppendedRowsCarryTypedSortKeys) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::WidgetData seed;
  seed.setTableHeaders("tbl", {"a", "b"});
  seed.setTableRows("tbl", std::vector<std::vector<PJ::TableItem>>{{"r0a", PJ::TableItem(65, "65")}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(seed.toJson()));

  PJ::WidgetData wd;
  wd.appendTableRows("tbl", 1, std::vector<std::vector<PJ::TableItem>>{{"r1a", PJ::TableItem(7, "7")}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));
  ASSERT_EQ(tw->rowCount(), 2);

  tw->sortItems(1, Qt::AscendingOrder);
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"r1a", "r0a"})) << "7 must sort before 65 numerically";
  EXPECT_EQ(columnTexts(tw, 1), (std::vector<std::string>{"7", "65"}));
}

// updateTableCells replaces the WHOLE cell (SDK contract): the sort key must
// move with the text, not stay pinned to whatever key the cell had before.
TEST(WidgetBindingTableDelta, UpdateCellsRewritesSortKeyNotJustText) {
  qapp();
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::WidgetData seed;
  seed.setTableHeaders("tbl", {"a", "b"});
  seed.setTableRows(
      "tbl",
      std::vector<std::vector<PJ::TableItem>>{{"r0a", PJ::TableItem(5, "5")}, {"r1a", PJ::TableItem(100, "100")}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(seed.toJson()));

  // Row 0's key moves from 5 to 200, which flips its relative order against
  // row 1 (100) — a stale key left at 5 would keep row 0 sorting first.
  PJ::WidgetData wd;
  wd.updateTableCells("tbl", 1, {{0, 1, PJ::TableItem(200, "200")}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  tw->sortItems(1, Qt::AscendingOrder);
  EXPECT_EQ(columnTexts(tw, 0), (std::vector<std::string>{"r1a", "r0a"})) << "100 must now sort before 200";
  EXPECT_EQ(columnTexts(tw, 1), (std::vector<std::string>{"100", "200"}));
}

// --- Tree-like header: name-column fill without sacrificing draggability -----

namespace {

// Builds a shown picker-style table (headers via widget data, so
// installTreeLikeHeader runs) inside a fixed-width window.
QTableWidget* makePickerTable(QWidget* root) {
  auto* layout = new QVBoxLayout(root);
  layout->setContentsMargins(0, 0, 0, 0);
  auto* tw = new QTableWidget(root);
  tw->setObjectName("tbl");
  layout->addWidget(tw);

  PJ::WidgetData wd;
  wd.setTableHeaders("tbl", {"Channel name", "Schema", "Msg Count"});
  wd.setTableRows(
      "tbl",
      std::vector<std::vector<PJ::TableItem>>{{"/imu", "sensor_msgs/Imu", "10"}, {"/tf", "tf2_msgs/TFMessage", "3"}});
  PJ::applyWidgetData(root, PJ::WidgetDataView(wd.toJson()));
  return tw;
}

// QTableView re-protects QAbstractItemView's public sizeHintForColumn.
int columnContentHint(const QTableWidget* tw, int col) {
  return static_cast<const QAbstractItemView*>(tw)->sizeHintForColumn(col);
}

int fillTarget(const QTableWidget* tw, int fill_col) {
  int others = 0;
  for (int i = 0; i < tw->columnCount(); ++i) {
    if (i != fill_col) {
      others += tw->horizontalHeader()->sectionSize(i);
    }
  }
  return tw->viewport()->width() - others;
}

}  // namespace

// The name column must be Interactive (Qt refuses to drag an auto-sized
// section's divider) while still absorbing the leftover viewport width.
TEST(WidgetBindingTreeLikeHeader, NameColumnIsDraggableAndFillsViewport) {
  qapp();
  QWidget root;
  auto* tw = makePickerTable(&root);
  root.resize(600, 300);
  root.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&root));
  QTest::qWait(50);  // let the deferred layout (scrollbar appearance) resize the viewport

  auto* header = tw->horizontalHeader();
  EXPECT_EQ(header->sectionResizeMode(0), QHeaderView::Interactive);
  EXPECT_EQ(header->sectionSize(0), fillTarget(tw, 0)) << "name column must absorb the leftover viewport width";
}

// Resizing a data column gives-and-takes from the name column.
TEST(WidgetBindingTreeLikeHeader, DataColumnResizeRefitsNameColumn) {
  qapp();
  QWidget root;
  auto* tw = makePickerTable(&root);
  root.resize(600, 300);
  root.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&root));

  auto* header = tw->horizontalHeader();
  header->resizeSection(1, 150);
  EXPECT_EQ(header->sectionSize(1), 150);
  EXPECT_EQ(header->sectionSize(0), fillTarget(tw, 0)) << "name column must re-absorb after a data-column resize";
}

// Once the user resizes the name column itself, their width wins: no more
// auto-refit on data-column drags or viewport growth.
TEST(WidgetBindingTreeLikeHeader, UserResizeOfNameColumnStopsTheFill) {
  qapp();
  QWidget root;
  auto* tw = makePickerTable(&root);
  root.resize(600, 300);
  root.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&root));

  // A user drag is a section resize while the left button is down ON the
  // header; programmatic/layout resizes (no header press) must NOT stop the
  // fill. The resize itself is driven programmatically between a real press
  // and release so the test does not depend on QTest's drag synthesis.
  auto* header = tw->horizontalHeader();
  const int y = header->height() / 2;
  QTest::mousePress(header->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(5, y));
  const int user_width = fillTarget(tw, 0) - 100;
  header->resizeSection(0, user_width);
  QTest::mouseRelease(header->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(5, y));

  header->resizeSection(1, 150);
  EXPECT_EQ(header->sectionSize(0), user_width) << "user-chosen width must survive data-column resizes";

  root.resize(760, 300);
  QTest::qWait(50);
  EXPECT_EQ(header->sectionSize(0), user_width) << "user-chosen width must survive viewport growth";
}

// With a Fixed radio column leading the table, the fill redirects to the first
// draggable column instead of fighting the radio's pinned width.
TEST(WidgetBindingTreeLikeHeader, RadioColumnRedirectsFillToFirstDataColumn) {
  qapp();
  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  layout->setContentsMargins(0, 0, 0, 0);
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  layout->addWidget(tw);

  PJ::WidgetData wd;
  wd.setTableHeaders("tbl", {"", "Name", "Count"});
  wd.setTableRows("tbl", std::vector<std::vector<PJ::TableItem>>{{"", "/imu", "10"}, {"", "/tf", "3"}});
  wd.setTableRadioColumn("tbl", 0, 0);
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));
  root.resize(600, 300);
  root.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&root));
  QTest::qWait(50);  // let the deferred layout (scrollbar appearance) resize the viewport

  auto* header = tw->horizontalHeader();
  EXPECT_EQ(header->sectionResizeMode(0), QHeaderView::Fixed);
  EXPECT_EQ(header->sectionSize(0), 36) << "radio column keeps its pinned width";
  EXPECT_EQ(header->sectionSize(1), fillTarget(tw, 1)) << "fill must target the first draggable column";
}

// Data columns default to their content width (Qt's double-click auto-fit
// formula) once rows arrive, so cell text is never clipped out of the box.
TEST(WidgetBindingTreeLikeHeader, DataColumnsDefaultToContentWidth) {
  qapp();
  QWidget root;
  auto* tw = makePickerTable(&root);
  root.resize(600, 300);
  root.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&root));
  QTest::qWait(50);  // one-shot content sizing is queued behind the row delivery

  auto* header = tw->horizontalHeader();
  const int schema_content = std::max(columnContentHint(tw, 1), header->sectionSizeHint(1));
  EXPECT_EQ(header->sectionSize(1), schema_content) << "schema column must fit its widest entry";
  EXPECT_GE(header->sectionSize(1), columnContentHint(tw, 1)) << "schema text must not be clipped";
}

// When the viewport is too narrow for everything, the name column floors at its
// own content width — the table grows a horizontal scrollbar instead of
// clipping the names.
TEST(WidgetBindingTreeLikeHeader, NameColumnFloorsAtContentWidthWhenNarrow) {
  qapp();
  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  layout->setContentsMargins(0, 0, 0, 0);
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  layout->addWidget(tw);

  PJ::WidgetData wd;
  wd.setTableHeaders("tbl", {"Channel name", "Schema", "Msg Count"});
  wd.setTableRows(
      "tbl", std::vector<std::vector<PJ::TableItem>>{
                 {"/really/long/namespace/imu_with_a_long_name", "sensor_msgs/Imu", "10"},
                 {"/tf", "tf2_msgs/TFMessage", "3"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));
  root.resize(260, 300);
  root.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&root));
  QTest::qWait(50);

  auto* header = tw->horizontalHeader();
  const int name_content = std::max(columnContentHint(tw, 0), header->sectionSizeHint(0));
  ASSERT_GT(name_content, fillTarget(tw, 0)) << "precondition: the window must be too narrow for the name column";
  EXPECT_EQ(header->sectionSize(0), name_content) << "name column must floor at its content width, not clip";
}

// Content widths follow the data: a rows re-delivery with longer entries
// re-fits the affected data columns.
TEST(WidgetBindingTreeLikeHeader, DataColumnsRefitWhenRowsChange) {
  qapp();
  QWidget root;
  auto* tw = makePickerTable(&root);
  root.resize(700, 300);
  root.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&root));
  QTest::qWait(50);
  const int before = tw->horizontalHeader()->sectionSize(1);

  PJ::WidgetData wd;
  wd.setTableRows(
      "tbl",
      std::vector<std::vector<PJ::TableItem>>{
          {"/camera", "sensor_msgs/CompressedImageWithAVeryLongTypeName", "77"}, {"/tf", "tf2_msgs/TFMessage", "3"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));
  QTest::qWait(50);

  auto* header = tw->horizontalHeader();
  const int schema_content = std::max(columnContentHint(tw, 1), header->sectionSizeHint(1));
  EXPECT_GT(header->sectionSize(1), before) << "longer schema entries must widen the column";
  EXPECT_EQ(header->sectionSize(1), schema_content);
}

// A data column the user dragged is user-owned: later rows re-deliveries must
// not re-fit it.
TEST(WidgetBindingTreeLikeHeader, UserResizedDataColumnSurvivesRowsChange) {
  qapp();
  QWidget root;
  auto* tw = makePickerTable(&root);
  root.resize(700, 300);
  root.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&root));
  QTest::qWait(50);

  auto* header = tw->horizontalHeader();
  const int y = header->height() / 2;
  QTest::mousePress(header->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(5, y));
  header->resizeSection(1, 55);
  QTest::mouseRelease(header->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(5, y));

  PJ::WidgetData wd;
  wd.setTableRows(
      "tbl",
      std::vector<std::vector<PJ::TableItem>>{
          {"/camera", "sensor_msgs/CompressedImageWithAVeryLongTypeName", "77"}, {"/tf", "tf2_msgs/TFMessage", "3"}});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));
  QTest::qWait(50);

  EXPECT_EQ(header->sectionSize(1), 55) << "content sizing must keep hands off a user-dragged column";
}
