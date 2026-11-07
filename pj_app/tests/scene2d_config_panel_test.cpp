// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Scene2DConfigPanel's layer rows mirror the dock's layers, the warning included.

#include <gtest/gtest.h>

#include <QListWidget>
#include <QListWidgetItem>
#include <QObject>

#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#include "pj_scene_common/scene_layer.h"
#include "ui/Scene2DConfigPanel.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

// A row carries its layer's warning from the start: one that stood when the panel bound (a list rebuild), and one
// the dock set before announcing the layer (a derived topic whose node already failed).
TEST(Scene2DConfigPanel, RowsCarryTheLayersWarning) {
  SessionManager session;
  ASSERT_TRUE(session.dataEngine().createDataset(DatasetDescriptor{.source_name = "log"}, 1).has_value());
  const auto first = session.objectStore().registerTopic({1, "/camera/a", R"({"builtin_object_type":"kImage"})"});
  const auto second = session.objectStore().registerTopic({1, "/camera/b", R"({"builtin_object_type":"kImage"})"});
  ASSERT_TRUE(first.has_value() && second.has_value());
  Scene2DDockWidget dock;
  dock.setSessionManager(&session);
  ASSERT_TRUE(dock.addTopic(*first, sdk::BuiltinObjectType::kImage, u"a"_s));
  dock.layerFor(*first)->setDerivedWarning(u"error: boom"_s);
  // Connected before the panel's own slot, so it runs first: what the dock's addTopic does for a failed node.
  QObject::connect(&dock, &SceneDockWidget::layerAdded, [&dock](ObjectTopicId topic) {
    dock.layerFor(topic)->setDerivedWarning(u"missing input 'cloud'"_s);
  });

  Scene2DConfigPanel panel;
  panel.bindDock(&dock);
  auto* list = panel.findChild<QListWidget*>();
  ASSERT_NE(list, nullptr);
  ASSERT_EQ(list->count(), 1);
  EXPECT_EQ(list->item(0)->toolTip(), u"error: boom"_s);

  ASSERT_TRUE(dock.addTopic(*second, sdk::BuiltinObjectType::kImage, u"b"_s));
  ASSERT_EQ(list->count(), 2);
  EXPECT_EQ(list->item(1)->toolTip(), u"missing input 'cloud'"_s);
}

}  // namespace
}  // namespace PJ
