// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Scene3DConfigPanel: the Settings area and its params toolbar follow the last row clicked, the Topics list's or a
// robot model row's.

#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QToolButton>

#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_widgets/ConfigPanelHost.h"
#include "ui/Scene3DConfigPanel.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

void click(QWidget* target, const QPoint& pos) {
  QMouseEvent press(QEvent::MouseButtonPress, pos, target->mapToGlobal(pos), Qt::LeftButton, Qt::LeftButton, {});
  QApplication::sendEvent(target, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, pos, target->mapToGlobal(pos), Qt::LeftButton, Qt::NoButton, {});
  QApplication::sendEvent(target, &release);
}

// One point cloud topic and one robot model row on a bound panel.
class Scene3DConfigPanelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(session_.dataEngine().createDataset(DatasetDescriptor{.source_name = "log"}, 1).has_value());
    const auto cloud = session_.objectStore().registerTopic({1, "/cloud", R"({"builtin_object_type":"kPointCloud"})"});
    ASSERT_TRUE(cloud.has_value());
    ASSERT_TRUE(dir_.isValid());
    const QString urdf = dir_.filePath(u"arm.urdf"_s);
    QFile out(urdf);
    ASSERT_TRUE(out.open(QIODevice::WriteOnly));
    out.write(R"(<robot name="arm"><link name="base_link"/></robot>)");
    out.close();

    dock_.setSessionManager(&session_);
    ASSERT_TRUE(dock_.addTopic(*cloud, sdk::BuiltinObjectType::kPointCloud, u"cloud"_s));
    ASSERT_NE(dock_.addRobotModelLayer(urdf).id, 0U);
    panel_.resize(400, 900);
    panel_.bindDock(&dock_);
    panel_.show();

    host_ = panel_.findChild<ConfigPanelHost*>();
    list_ = panel_.findChild<QListWidget*>();
    ASSERT_NE(host_, nullptr);
    ASSERT_NE(list_, nullptr);
    ASSERT_EQ(list_->count(), 1);
    for (QLineEdit* edit : panel_.findChildren<QLineEdit*>()) {
      if (edit->property("robot_topic_id").isValid()) {
        robot_row_ = edit;
      }
    }
    ASSERT_NE(robot_row_, nullptr);
  }

  void clickRobotRow() {
    click(robot_row_, robot_row_->rect().center());
  }
  void clickTopicRow() {
    click(list_->viewport(), list_->visualItemRect(list_->item(0)).center());
  }

  [[nodiscard]] bool settingsShow(const QString& label) const {
    for (const QLabel* shown : host_->findChildren<QLabel*>()) {
      if (shown->text() == label) {
        return true;
      }
    }
    return false;
  }
  // Only the point cloud's settings have a point size; only the robot's have a frame prefix.
  [[nodiscard]] bool showsTopicSettings() const {
    return settingsShow(u"Point size:"_s);
  }
  [[nodiscard]] bool showsRobotSettings() const {
    return settingsShow(u"Frame prefix"_s);
  }

  [[nodiscard]] bool copyParamsEnabled() const {
    for (const QToolButton* button : panel_.findChildren<QToolButton*>()) {
      if (button->toolTip() == u"Copy parameters"_s) {
        return button->isEnabled();
      }
    }
    ADD_FAILURE() << "no Copy parameters button";
    return false;
  }

  SessionManager session_;
  QTemporaryDir dir_;
  Scene3DDockWidget dock_;
  Scene3DConfigPanel panel_;
  ConfigPanelHost* host_ = nullptr;
  QListWidget* list_ = nullptr;
  QLineEdit* robot_row_ = nullptr;
};

// The topic stays the list's current row when the robot row is clicked; a click on it again must still reach the
// Settings area.
TEST_F(Scene3DConfigPanelTest, TopicClickAfterRobotRowShowsTopicSettings) {
  ASSERT_TRUE(showsTopicSettings());
  clickRobotRow();
  ASSERT_TRUE(showsRobotSettings());
  ASSERT_FALSE(showsTopicSettings());

  clickTopicRow();
  EXPECT_TRUE(showsTopicSettings());
  EXPECT_FALSE(showsRobotSettings());
}

// The params toolbar copies the selected topic's parameters; the robot's settings are not a topic's.
TEST_F(Scene3DConfigPanelTest, ParamsToolbarFollowsTheTopicSelection) {
  ASSERT_TRUE(copyParamsEnabled());
  clickRobotRow();
  EXPECT_FALSE(copyParamsEnabled());
  clickTopicRow();
  EXPECT_TRUE(copyParamsEnabled());
}

}  // namespace
}  // namespace PJ
