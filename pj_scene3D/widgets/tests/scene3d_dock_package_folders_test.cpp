// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <functional>

#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/layers/robot_model_layer.h"
using namespace Qt::StringLiterals;

namespace {

constexpr char kArmUrdf[] = R"(<?xml version="1.0"?>
<robot name="arm">
  <link name="base_link">
    <visual><geometry><mesh filename="package://arm_description/meshes/link.stl"/></geometry></visual>
  </link>
</robot>
)";

bool pumpUntil(const std::function<bool()>& done) {
  QElapsedTimer timer;
  timer.start();
  while (!done() && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return done();
}

bool writeFile(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return QDir().mkpath(QFileInfo(path).absolutePath()) && file.open(QIODevice::WriteOnly) &&
         file.write(bytes) == bytes.size();
}

// A folder added in Preferences reaches open views: the robot's pending package resolves without reloading the layout.
TEST(Scene3DDockPackageFolders, ReloadResolvesPendingRobotMeshes) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString workspace = dir.filePath(u"ws"_s);
  ASSERT_TRUE(writeFile(workspace + u"/src/robot_repo/arm_description/meshes/link.stl"_s, {}));
  const QString urdf = dir.filePath(u"robot/arm.urdf"_s);
  ASSERT_TRUE(writeFile(urdf, kArmUrdf));
  QSettings settings(dir.filePath(u"settings.ini"_s), QSettings::IniFormat);

  PJ::SessionManager session;
  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setSettings(&settings);
  const PJ::ObjectTopicId id = dock.addRobotModelLayer(urdf);
  ASSERT_NE(id.id, 0U);
  auto* robot = dynamic_cast<pj::scene3d::RobotModelLayer*>(dock.layerFor(id));
  ASSERT_NE(robot, nullptr);
  ASSERT_TRUE(pumpUntil([robot] { return robot->robotModel() != nullptr; })) << robot->statusText().toStdString();
  ASSERT_EQ(robot->unresolvedMeshCount(), 1);

  PJ::Scene3DDockWidget::setPackageSearchFolders(settings, {workspace});
  dock.reloadPackageSearchFolders();
  EXPECT_TRUE(pumpUntil([robot] { return robot->robotModel() != nullptr && robot->unresolvedMeshCount() == 0; }))
      << robot->statusText().toStdString();
}

}  // namespace
