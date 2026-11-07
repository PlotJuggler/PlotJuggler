// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// WidgetBindings: reconciliation by name, input picking, slots, persistence and re-resolution.

#include <gtest/gtest.h>

#include <memory>

#include "dock_test_support.h"
#include "pj_custom_widgets/WidgetBindings.h"

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

class WidgetBindingsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    app_ = std::make_unique<AppSession>();
    dataset_ = pj_cw_test::createDataset(*app_, "robot.mcap");
    bindings_ = std::make_unique<WidgetBindings>(&app_->sessionManager(), &app_->catalogModel());
    number_ = pj_cw_test::addNumberTopic(*app_, dataset_, "/battery", {{1.0, 12.0}});
    text_ = pj_cw_test::addTextTopic(*app_, dataset_, "/mode", {{1.0, "IDLE"}});
  }

  static std::vector<WidgetInput> gaugeInputs() {
    return {WidgetInput("value"), WidgetInput("state", "", "", WidgetInputKind::kText)};
  }

  std::unique_ptr<AppSession> app_;
  std::unique_ptr<WidgetBindings> bindings_;
  DatasetId dataset_ = 0;
  QString number_;
  QString text_;
};

TEST_F(WidgetBindingsTest, KindOfKeySeparatesNumbersAndText) {
  EXPECT_EQ(bindings_->kindOfKey(number_), WidgetInputKind::kNumber);
  EXPECT_EQ(bindings_->kindOfKey(text_), WidgetInputKind::kText);
  EXPECT_FALSE(bindings_->kindOfKey(u"no/such/key"_s).has_value());
}

TEST_F(WidgetBindingsTest, BindRejectsAKeyOfTheWrongKind) {
  bindings_->setInputs(gaugeInputs());
  EXPECT_FALSE(bindings_->bindKey(0, 0, text_));
  EXPECT_FALSE(bindings_->bindKey(1, 0, number_));
  EXPECT_TRUE(bindings_->bindKey(0, 0, number_));
  EXPECT_TRUE(bindings_->bindKey(1, 0, text_));
  ASSERT_EQ(bindings_->wires().size(), 2U);
  EXPECT_EQ(bindings_->wires()[0].label, "/battery/value");
  EXPECT_TRUE(bindings_->wires()[0].resolved);
}

TEST_F(WidgetBindingsTest, ANormalInputReplacesItsWireAndAMultipleOneAppendsSlots) {
  bindings_->setInputs({WidgetInput("one"), WidgetInput("many", "", "", WidgetInputKind::kNumber, true)});
  const QString other = pj_cw_test::addNumberTopic(*app_, dataset_, "/other", {{1.0, 1.0}});
  ASSERT_TRUE(bindings_->bindKey(0, WidgetBindings::kNewSlot, number_));
  ASSERT_TRUE(bindings_->bindKey(0, WidgetBindings::kNewSlot, other));
  EXPECT_EQ(bindings_->wires().size(), 1U);
  EXPECT_EQ(bindings_->wires()[0].label, "/other/value");

  ASSERT_TRUE(bindings_->bindKey(1, WidgetBindings::kNewSlot, number_));
  ASSERT_TRUE(bindings_->bindKey(1, WidgetBindings::kNewSlot, other));
  ASSERT_EQ(bindings_->wires().size(), 3U);
  EXPECT_EQ(bindings_->wires()[1].slot, 0U);
  EXPECT_EQ(bindings_->wires()[2].slot, 1U);
  EXPECT_TRUE(bindings_->unbind(u"many"_s, 0));
  EXPECT_FALSE(bindings_->unbind(u"many"_s, 0));
}

TEST_F(WidgetBindingsTest, PickInputPrefersEmptyThenMultipleThenReplace) {
  bindings_->setInputs(
      {WidgetInput("a"), WidgetInput("b"), WidgetInput("many", "", "", WidgetInputKind::kNumber, true)});
  EXPECT_EQ(bindings_->pickInput(WidgetInputKind::kNumber), 0U);
  ASSERT_TRUE(bindings_->bindKey(0, 0, number_));
  EXPECT_EQ(bindings_->pickInput(WidgetInputKind::kNumber), 1U);
  ASSERT_TRUE(bindings_->bindKey(1, 0, number_));
  EXPECT_EQ(bindings_->pickInput(WidgetInputKind::kNumber), 2U) << "no empty input left: the multiple one appends";
  EXPECT_FALSE(bindings_->pickInput(WidgetInputKind::kText).has_value());
  EXPECT_FALSE(bindings_->pickInput(WidgetInputKind::kNumber, {0, 1, 2}).has_value());

  bindings_->setInputs({WidgetInput("a")});
  ASSERT_TRUE(bindings_->bindKey(0, 0, number_));
  EXPECT_EQ(bindings_->pickInput(WidgetInputKind::kNumber), 0U) << "replace";
  EXPECT_FALSE(bindings_->pickInput(WidgetInputKind::kNumber, {0}).has_value());
}

TEST_F(WidgetBindingsTest, SetInputsKeepsSurvivorsByNameAndDropsTheRest) {
  bindings_->setInputs(gaugeInputs());
  ASSERT_TRUE(bindings_->bindKey(0, 0, number_));
  ASSERT_TRUE(bindings_->bindKey(1, 0, text_));

  // "state" vanishes, "extra" is new, "value" moves to another position.
  const auto delta = bindings_->setInputs({WidgetInput("extra"), WidgetInput("value")});
  EXPECT_TRUE(delta.inputs_changed);
  EXPECT_TRUE(delta.wires_changed);
  ASSERT_EQ(bindings_->wires().size(), 1U);
  EXPECT_EQ(bindings_->wires()[0].input, u"value"_s);
  EXPECT_EQ(bindings_->wires()[0].input_index, 1U) << "the wire follows its name to the new position";

  const auto same = bindings_->setInputs({WidgetInput("extra"), WidgetInput("value")});
  EXPECT_FALSE(same.inputs_changed);
  EXPECT_FALSE(same.wires_changed);

  // A kind change under the same name drops the wire; a normal input loses extra slots.
  const auto kind = bindings_->setInputs({WidgetInput("value", "", "", WidgetInputKind::kText)});
  EXPECT_TRUE(kind.wires_changed);
  EXPECT_TRUE(bindings_->wires().empty());
}

TEST_F(WidgetBindingsTest, SavedWiresRoundTripAndPendingOnesAreKeptVerbatim) {
  bindings_->setInputs(gaugeInputs());
  ASSERT_TRUE(bindings_->bindKey(0, 0, number_));
  app_->sessionManager().setDatasetSourcePath(dataset_, u"/data/robot.mcap"_s);

  QDomDocument doc;
  QDomElement root = doc.createElement(u"CustomWidget"_s);
  bindings_->save(doc, root);
  const QDomElement input = root.firstChildElement(u"input"_s);
  ASSERT_FALSE(input.isNull());
  EXPECT_EQ(input.attribute(u"name"_s), u"value"_s);
  EXPECT_EQ(input.attribute(u"slot"_s), u"0"_s);
  EXPECT_EQ(input.attribute(u"kind"_s), u"number"_s);
  EXPECT_EQ(input.attribute(u"dataset_source"_s), u"robot.mcap"_s);
  EXPECT_EQ(input.attribute(u"topic"_s), u"/battery"_s);
  EXPECT_EQ(input.attribute(u"field"_s), u"value"_s);

  WidgetBindings restored(&app_->sessionManager(), &app_->catalogModel());
  restored.setInputs(gaugeInputs());
  restored.load(root);
  ASSERT_EQ(restored.wires().size(), 1U);
  EXPECT_TRUE(restored.wires()[0].resolved);
  EXPECT_EQ(restored.wires()[0].key, number_);

  // An input that resolves to nothing is pending and survives the next save unchanged.
  QDomDocument other_doc;
  QDomElement other = other_doc.createElement(u"CustomWidget"_s);
  QDomElement ghost = other_doc.createElement(u"input"_s);
  ghost.setAttribute(u"name"_s, u"value"_s);
  ghost.setAttribute(u"slot"_s, u"0"_s);
  ghost.setAttribute(u"kind"_s, u"number"_s);
  ghost.setAttribute(u"dataset_id"_s, u"7"_s);
  ghost.setAttribute(u"dataset_source"_s, u"absent.csv"_s);
  ghost.setAttribute(u"dataset_path"_s, u"/x/absent.csv"_s);
  ghost.setAttribute(u"topic"_s, u"/gone"_s);
  ghost.setAttribute(u"field"_s, u"f"_s);
  other.appendChild(ghost);
  restored.load(other);
  ASSERT_EQ(restored.wires().size(), 1U);
  EXPECT_FALSE(restored.wires()[0].resolved);
  EXPECT_TRUE(restored.displayedTopics().empty());

  QDomDocument out_doc;
  QDomElement out = out_doc.createElement(u"CustomWidget"_s);
  restored.save(out_doc, out);
  const QDomElement kept = out.firstChildElement(u"input"_s);
  EXPECT_EQ(kept.attribute(u"dataset_id"_s), u"7"_s);
  EXPECT_EQ(kept.attribute(u"dataset_path"_s), u"/x/absent.csv"_s);
  EXPECT_EQ(kept.attribute(u"topic"_s), u"/gone"_s);
  EXPECT_EQ(kept.attribute(u"field"_s), u"f"_s);
}

// A saved dataset id that no DatasetId can hold is not wrapped into another dataset: it decays to an
// unqualified identity, as a plot curve's does.
TEST_F(WidgetBindingsTest, AnOutOfRangeSavedDatasetIdLoadsUnqualified) {
  bindings_->setInputs({WidgetInput("value")});
  QDomDocument doc;
  QDomElement root = doc.createElement(u"CustomWidget"_s);
  for (const QString& raw : {u"4294967297"_s, u"-1"_s, u"x"_s}) {
    QDomElement e = doc.createElement(u"input"_s);
    e.setAttribute(u"name"_s, u"value"_s);
    e.setAttribute(u"kind"_s, u"number"_s);
    e.setAttribute(u"dataset_id"_s, raw);
    e.setAttribute(u"topic"_s, u"/absent"_s);
    e.setAttribute(u"field"_s, u"f"_s);
    root.appendChild(e);
    bindings_->load(root);
    ASSERT_EQ(bindings_->wires().size(), 1U) << raw.toStdString();
    EXPECT_EQ(bindings_->wires()[0].identity.dataset_id, 0U) << raw.toStdString();
    root.removeChild(e);
  }
}

TEST_F(WidgetBindingsTest, LoadDropsInputsTheWidgetDoesNotHaveOrOfAnotherKind) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"CustomWidget"_s);
  const auto add = [&](const QString& name, const QString& kind, const QString& slot) {
    QDomElement e = doc.createElement(u"input"_s);
    e.setAttribute(u"name"_s, name);
    e.setAttribute(u"kind"_s, kind);
    e.setAttribute(u"slot"_s, slot);
    e.setAttribute(u"topic"_s, u"/battery"_s);
    e.setAttribute(u"field"_s, u"value"_s);
    root.appendChild(e);
  };
  add(u"value"_s, u"number"_s, u"0"_s);
  add(u"nope"_s, u"number"_s, u"0"_s);
  add(u"state"_s, u"number"_s, u"0"_s);  // wrong kind
  add(u"value"_s, u"number"_s, u"1"_s);  // extra slot on a normal input
  bindings_->setInputs(gaugeInputs());
  bindings_->load(root);
  ASSERT_EQ(bindings_->wires().size(), 1U);
  EXPECT_EQ(bindings_->wires()[0].input, u"value"_s);
}

TEST_F(WidgetBindingsTest, LoadDropsASlotEqualToTheAppendSentinel) {
  QDomDocument doc;
  QDomElement root = doc.createElement(u"CustomWidget"_s);
  QDomElement input = doc.createElement(u"input"_s);
  input.setAttribute(u"name"_s, u"many"_s);
  input.setAttribute(u"kind"_s, u"number"_s);
  input.setAttribute(u"slot"_s, QString::number(WidgetBindings::kNewSlot));
  input.setAttribute(u"topic"_s, u"/battery"_s);
  input.setAttribute(u"field"_s, u"value"_s);
  root.appendChild(input);
  bindings_->setInputs({WidgetInput("many", "", "", WidgetInputKind::kNumber, true)});
  bindings_->load(root);
  EXPECT_TRUE(bindings_->wires().empty());
}

TEST_F(WidgetBindingsTest, PendingWireResolvesWhenTheDatasetAppearsAndRevalidateUnbindsAVanishedOne) {
  bindings_->setInputs({WidgetInput("value")});
  QDomDocument doc;
  QDomElement root = doc.createElement(u"CustomWidget"_s);
  QDomElement e = doc.createElement(u"input"_s);
  e.setAttribute(u"name"_s, u"value"_s);
  e.setAttribute(u"kind"_s, u"number"_s);
  e.setAttribute(u"dataset_source"_s, u"robot.mcap"_s);
  e.setAttribute(u"topic"_s, u"/late"_s);
  e.setAttribute(u"field"_s, u"value"_s);
  root.appendChild(e);
  bindings_->load(root);
  ASSERT_EQ(bindings_->wires().size(), 1U);
  EXPECT_FALSE(bindings_->wires()[0].resolved);
  EXPECT_FALSE(bindings_->retryPending());

  const QString late = pj_cw_test::addNumberTopic(*app_, dataset_, "/late", {{1.0, 5.0}});
  ASSERT_FALSE(late.isEmpty());
  EXPECT_TRUE(bindings_->retryPending());
  EXPECT_TRUE(bindings_->wires()[0].resolved);
  EXPECT_EQ(bindings_->displayedTopics().size(), 1U);
  EXPECT_FALSE(bindings_->revalidate()) << "nothing moved";

  app_->catalogModel().removeDataset(dataset_);
  EXPECT_TRUE(bindings_->revalidate());
  EXPECT_FALSE(bindings_->wires()[0].resolved);
  EXPECT_EQ(bindings_->wires()[0].identity.topic, u"/late"_s) << "the identity survives for a later retry";
}

}  // namespace
}  // namespace PJ
