// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QSpinBox>
#include <QWidget>
#include <limits>
#include <vector>

#include "pj_plotting/ParameterForm.h"
#include "pj_scripting/filter_class.h"

using PJ::ParameterForm;
using PJ::scripting::EnumValue;
using PJ::scripting::ParamSpec;
using PJ::scripting::ParamType;

namespace {

ParamSpec num(const std::string& name, double def) {
  ParamSpec p;
  p.name = name;
  p.type = ParamType::kNumber;
  p.default_value = def;
  return p;
}
ParamSpec integer(const std::string& name, int def) {
  ParamSpec p;
  p.name = name;
  p.type = ParamType::kInteger;
  p.default_value = def;
  p.min = 1.0;
  p.max = 1000.0;
  return p;
}
ParamSpec boolean(const std::string& name, bool def) {
  ParamSpec p;
  p.name = name;
  p.type = ParamType::kBoolean;
  p.default_value = def;
  return p;
}

}  // namespace

TEST(ParameterForm, EmptySchema) {
  ParameterForm form;
  form.setSchema({});
  EXPECT_TRUE(form.isEmpty());
  EXPECT_TRUE(form.values().empty());
}

// [k] A kEnum with no selectable value (empty `values`) must still emit its key — a create()
// reading params[name] should never silently get a missing key.
TEST(ParameterForm, EnumWithNoSelectionStillEmitsKey) {
  ParamSpec e;
  e.name = "mode";
  e.type = ParamType::kEnum;  // values left empty -> combobox is empty, currentIndex() == -1
  ParameterForm form;
  form.setSchema({e});
  EXPECT_TRUE(form.values().contains("mode"));  // was dropped before the fix
}

// [k] An int64-documented min/max beyond the 32-bit range must be CLAMPED into the QSpinBox
// range, not truncated/wrapped by a blind static_cast<int>.
TEST(ParameterForm, IntegerSpinBoxClampsLargeRange) {
  ParamSpec big;
  big.name = "huge";
  big.type = ParamType::kInteger;
  big.default_value = 0;
  big.min = 0.0;
  big.max = 5e9;  // > INT_MAX
  ParameterForm form;
  form.setSchema({big});
  auto* spin = form.findChild<QSpinBox*>();
  ASSERT_NE(spin, nullptr);
  EXPECT_EQ(spin->maximum(), std::numeric_limits<int>::max());
}

TEST(ParameterForm, DefaultsSeededIntoValues) {
  ParameterForm form;
  form.setSchema({integer("window", 10), boolean("flag", true), num("k", 2.5)});
  const auto v = form.values();
  EXPECT_EQ(v.at("window"), 10);
  EXPECT_EQ(v.at("flag"), true);
  EXPECT_DOUBLE_EQ(v.at("k").get<double>(), 2.5);
}

TEST(ParameterForm, ValuesRoundTripPerType) {
  ParameterForm form;
  form.setSchema({integer("window", 10), boolean("flag", false), num("k", 0.0)});
  form.setValues({{"window", 42}, {"flag", true}, {"k", 1e-9}});  // 1e-9 precision
  const auto v = form.values();
  EXPECT_EQ(v.at("window"), 42);
  EXPECT_EQ(v.at("flag"), true);
  EXPECT_DOUBLE_EQ(v.at("k").get<double>(), 1e-9);
}

TEST(ParameterForm, EnumStoresTypedValue) {
  ParamSpec op;
  op.name = "binary_op";
  op.type = ParamType::kEnum;
  op.default_value = 3;
  op.values = {EnumValue{0, "=="}, EnumValue{3, ">"}, EnumValue{5, "range"}};
  ParameterForm form;
  form.setSchema({op});
  EXPECT_EQ(form.values().at("binary_op"), 3);  // typed int, not "3"
  form.setValues({{"binary_op", 5}});
  EXPECT_EQ(form.values().at("binary_op"), 5);
}

TEST(ParameterForm, ConditionalVisibility) {
  ParamSpec use;
  use.name = "use_custom";
  use.type = ParamType::kBoolean;
  use.default_value = false;
  ParamSpec dt = num("custom_dt", 0.01);
  dt.visible_when_param = "use_custom";
  dt.visible_when_equals = true;

  ParameterForm form;
  form.setSchema({use, dt});
  auto* dt_editor = form.findChild<QWidget*>("custom_dt");
  ASSERT_NE(dt_editor, nullptr);
  EXPECT_TRUE(dt_editor->isHidden());  // use_custom=false → hidden

  form.setValues({{"use_custom", true}});
  EXPECT_FALSE(dt_editor->isHidden());  // now shown
}

TEST(ParameterForm, ChangedSilentOnSetValuesButFiresOnEdit) {
  ParameterForm form;
  form.setSchema({integer("window", 10)});
  int changed = 0;
  QObject::connect(&form, &ParameterForm::changed, &form, [&changed]() { ++changed; });

  form.setValues({{"window", 20}});
  EXPECT_EQ(changed, 0);  // programmatic set is silent

  auto* sb = qobject_cast<QSpinBox*>(form.findChild<QWidget*>("window"));
  ASSERT_NE(sb, nullptr);
  sb->setValue(33);  // a real user-style edit
  EXPECT_EQ(changed, 1);
}

TEST(ParameterForm, SchemaSwapClearsOldRows) {
  ParameterForm form;
  form.setSchema({integer("a", 1), num("b", 2.0)});
  EXPECT_EQ(form.values().size(), 2u);
  form.setSchema({boolean("c", true)});
  const auto v = form.values();
  EXPECT_EQ(v.size(), 1u);
  EXPECT_TRUE(v.contains("c"));
  EXPECT_FALSE(v.contains("a"));
}
