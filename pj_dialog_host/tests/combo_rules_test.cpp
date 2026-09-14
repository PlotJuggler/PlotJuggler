// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Tests for the shared ComboRule parser behind the "pj_enable_when" and
// "pj_visible_when" declarative .ui properties (widget_binding.hpp): the
// ';'-joined compound-clause syntax (AND across clauses), the visible-rule
// wins-over-plugin-push guarantee applyWidgetData gives enable rules too, and
// that hiding both halves of a QFormLayout row actually collapses it.

#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <pj_plugins/host/widget_data_view.hpp>
#include <pj_plugins/host_qt/pj_ui_loader.hpp>
#include <pj_plugins/host_qt/widget_binding.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include "gtest/gtest.h"

namespace {

// The runner's shared main (cmake/test_mains/pj_test_main_gui.cpp) owns the
// QApplication; this only returns the instance.
QApplication* qapp() {
  return qobject_cast<QApplication*>(QCoreApplication::instance());
}

// Two combos (backendCombo: A, B; modeCombo: X, Y) and a QFormLayout row per
// rule shape under test: single-clause visibility (a/b rows), a compound
// two-clause visibility (customEdit), a single-clause enable (enEdit), and an
// unresolvable combo name (badEdit) that must be left exactly as authored.
QDialog* buildDialog() {
  const QByteArray ui = R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>ComboRulesDialog</class>
 <widget class="QDialog" name="ComboRulesDialog">
  <layout class="QFormLayout" name="formLayout">
   <item row="0" column="0"><widget class="QComboBox" name="backendCombo">
     <item><property name="text"><string>A</string></property></item>
     <item><property name="text"><string>B</string></property></item>
   </widget></item>
   <item row="1" column="0"><widget class="QComboBox" name="modeCombo">
     <item><property name="text"><string>X</string></property></item>
     <item><property name="text"><string>Y</string></property></item>
   </widget></item>
   <item row="2" column="0"><widget class="QLabel" name="aLabel">
     <property name="text"><string>A field</string></property>
     <property name="pj_visible_when" stdset="0"><string>backendCombo:0</string></property>
   </widget></item>
   <item row="2" column="1"><widget class="QLineEdit" name="aEdit">
     <property name="pj_visible_when" stdset="0"><string>backendCombo:0</string></property>
   </widget></item>
   <item row="3" column="0"><widget class="QLabel" name="bLabel">
     <property name="text"><string>B field, with enough text to give this row real height</string></property>
     <property name="pj_visible_when" stdset="0"><string>backendCombo:1</string></property>
   </widget></item>
   <item row="3" column="1"><widget class="QLineEdit" name="bEdit">
     <property name="pj_visible_when" stdset="0"><string>backendCombo:1</string></property>
   </widget></item>
   <item row="4" column="0"><widget class="QLabel" name="customLabel">
     <property name="text"><string>Custom</string></property>
     <property name="pj_visible_when" stdset="0"><string>backendCombo:0;modeCombo:1</string></property>
   </widget></item>
   <item row="4" column="1"><widget class="QLineEdit" name="customEdit">
     <property name="pj_visible_when" stdset="0"><string>backendCombo:0;modeCombo:1</string></property>
   </widget></item>
   <item row="5" column="1"><widget class="QLineEdit" name="enEdit">
     <property name="pj_enable_when" stdset="0"><string>backendCombo:1</string></property>
   </widget></item>
   <item row="6" column="1"><widget class="QLineEdit" name="badEdit">
     <property name="pj_visible_when" stdset="0"><string>nosuchcombo:0</string></property>
   </widget></item>
  </layout>
 </widget>
</ui>)";
  QByteArray data(ui);
  QBuffer buffer(&data);
  buffer.open(QIODevice::ReadOnly);
  PJ::PjUiLoader loader;
  return qobject_cast<QDialog*>(loader.load(&buffer));
}

}  // namespace

TEST(ComboRules, InitialStateAtBackendAShowsAHidesBAndLeavesBadEditAlone) {
  qapp();
  QDialog* dlg = buildDialog();
  ASSERT_NE(dlg, nullptr);
  PJ::installDeclarativeRules(dlg);

  auto* aLabel = dlg->findChild<QLabel*>("aLabel");
  auto* aEdit = dlg->findChild<QLineEdit*>("aEdit");
  auto* bLabel = dlg->findChild<QLabel*>("bLabel");
  auto* bEdit = dlg->findChild<QLineEdit*>("bEdit");
  auto* enEdit = dlg->findChild<QLineEdit*>("enEdit");
  auto* badEdit = dlg->findChild<QLineEdit*>("badEdit");

  EXPECT_FALSE(aLabel->isHidden());
  EXPECT_FALSE(aEdit->isHidden());
  EXPECT_TRUE(bLabel->isHidden());
  EXPECT_TRUE(bEdit->isHidden());
  EXPECT_FALSE(enEdit->isEnabled()) << "backendCombo sits on 0, rule requires 1";
  EXPECT_FALSE(badEdit->isHidden()) << "unresolvable combo name: the whole rule is rejected, widget untouched";
  delete dlg;
}

TEST(ComboRules, SwitchingBackendFlipsVisibilityAndEnableLive) {
  qapp();
  QDialog* dlg = buildDialog();
  ASSERT_NE(dlg, nullptr);
  PJ::installDeclarativeRules(dlg);

  auto* backend = dlg->findChild<QComboBox*>("backendCombo");
  auto* aEdit = dlg->findChild<QLineEdit*>("aEdit");
  auto* bLabel = dlg->findChild<QLabel*>("bLabel");
  auto* bEdit = dlg->findChild<QLineEdit*>("bEdit");
  auto* enEdit = dlg->findChild<QLineEdit*>("enEdit");

  backend->setCurrentIndex(1);

  EXPECT_TRUE(aEdit->isHidden());
  EXPECT_FALSE(bLabel->isHidden());
  EXPECT_FALSE(bEdit->isHidden());
  EXPECT_TRUE(enEdit->isEnabled()) << "backendCombo now sits on 1";
  delete dlg;
}

TEST(ComboRules, CompoundClauseIsAnAndAcrossBothCombos) {
  qapp();
  QDialog* dlg = buildDialog();
  ASSERT_NE(dlg, nullptr);
  PJ::installDeclarativeRules(dlg);

  auto* backend = dlg->findChild<QComboBox*>("backendCombo");
  auto* mode = dlg->findChild<QComboBox*>("modeCombo");
  auto* customEdit = dlg->findChild<QLineEdit*>("customEdit");

  // backendCombo:0 (default) + modeCombo:0 (default) — only one clause holds.
  EXPECT_TRUE(customEdit->isHidden());

  mode->setCurrentIndex(1);
  EXPECT_FALSE(customEdit->isHidden()) << "both clauses now hold";

  backend->setCurrentIndex(1);
  EXPECT_TRUE(customEdit->isHidden()) << "backendCombo clause broke again";
  delete dlg;
}

// A hidden QFormLayout row (label + field both hidden) must actually shrink
// the dialog — Qt's layout skips fully-hidden items when sizing — not just
// leave an invisible gap. Proves pj_visible_when is wired to real widgets,
// not just a flag nobody reads. Toggles modeCombo only (backendCombo stays at
// 0) so the customEdit row is the single thing that changes state — a and b
// rows don't confound the comparison by swapping which of them is visible.
TEST(ComboRules, HidingARowShrinksTheDialog) {
  qapp();
  QDialog* dlg = buildDialog();
  ASSERT_NE(dlg, nullptr);
  PJ::installDeclarativeRules(dlg);
  dlg->show();

  auto* mode = dlg->findChild<QComboBox*>("modeCombo");
  dlg->layout()->invalidate();
  const int height_custom_hidden = dlg->sizeHint().height();  // modeCombo:0 — the clause doesn't hold

  mode->setCurrentIndex(1);
  dlg->layout()->invalidate();
  const int height_custom_visible = dlg->sizeHint().height();

  EXPECT_LT(height_custom_hidden, height_custom_visible) << "showing the compound-rule row must reflow the form";
  delete dlg;
}

// applyWidgetData wraps every apply in a QSignalBlocker and re-asserts the
// combo rules afterward (see its doc comment) — a plugin-pushed `visible`
// must lose to a "pj_visible_when" rule exactly as it does for `enabled`.
TEST(ComboRules, PluginPushedVisibleLosesToTheRule) {
  qapp();
  QDialog* dlg = buildDialog();
  ASSERT_NE(dlg, nullptr);
  PJ::installDeclarativeRules(dlg);
  // backendCombo stays at 0: bEdit's rule ("backendCombo:1") does not hold.

  PJ::WidgetData data;
  data.setVisible("bEdit", true);
  PJ::applyWidgetData(dlg, PJ::WidgetDataView(data.toJson()));

  auto* bEdit = dlg->findChild<QLineEdit*>("bEdit");
  EXPECT_TRUE(bEdit->isHidden()) << "the rule re-asserts after the apply and wins";
  delete dlg;
}

// Regression guard: InitialStateAtBackendA... and SwitchingBackendFlips...
// above already exercise the single-clause "pj_enable_when" on enEdit
// unchanged from before compound clauses existed. This covers the other
// half: a compound "pj_enable_when" ANDs across its clauses the same way
// "pj_visible_when" does.
TEST(ComboRules, EnableWhenCompoundClauseAndsAcrossCombos) {
  qapp();
  QDialog* dlg = buildDialog();
  ASSERT_NE(dlg, nullptr);
  auto* enEdit = dlg->findChild<QLineEdit*>("enEdit");
  enEdit->setProperty("pj_enable_when", QStringLiteral("backendCombo:1;modeCombo:1"));
  PJ::installDeclarativeRules(dlg);

  auto* backend = dlg->findChild<QComboBox*>("backendCombo");
  auto* mode = dlg->findChild<QComboBox*>("modeCombo");

  EXPECT_FALSE(enEdit->isEnabled()) << "backendCombo:0, modeCombo:0 — neither clause holds";
  backend->setCurrentIndex(1);
  EXPECT_FALSE(enEdit->isEnabled()) << "backendCombo now holds, modeCombo still doesn't";
  mode->setCurrentIndex(1);
  EXPECT_TRUE(enEdit->isEnabled()) << "both clauses hold";
  delete dlg;
}

// Claim: a rule authored on the loaded root obeys the same apply-time precedence as a child's rule.
TEST(ComboRules, LoadedRootRuleIsInstalledAndReassertedAfterComboData) {
  qapp();
  QWidget root;
  auto* combo = new QComboBox(&root);
  combo->setObjectName("mode");
  combo->addItems({"Off", "On"});
  root.setProperty("pj_enable_when", QStringLiteral("mode:1"));
  PJ::installDeclarativeRules(&root);
  EXPECT_FALSE(root.isEnabled()) << "the loaded root's declarative rule was skipped";

  PJ::WidgetData enabled;
  enabled.setCurrentIndex("mode", 1);
  PJ::applyWidgetData(&root, PJ::WidgetDataView(enabled.toJson()));
  EXPECT_TRUE(root.isEnabled());
  PJ::WidgetData disabled;
  disabled.setCurrentIndex("mode", 0);
  PJ::applyWidgetData(&root, PJ::WidgetDataView(disabled.toJson()));
  EXPECT_FALSE(root.isEnabled());
}
