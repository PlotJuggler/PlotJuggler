// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include "support/stream_manager_fixture.h"

#ifndef PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH
#error "PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH must be defined"
#endif

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

using StreamingPluginLifetimeTest = pj_app_test::StreamManagerFixture;

TEST_F(StreamingPluginLifetimeTest, AStreamReachesItsServicesFromStartAndItsDestructor) {
  QString error;
  const DatasetId dataset = startFromLayout(uR"({"probe_services":true})"_s, error);
  ASSERT_NE(dataset, 0U) << error.toStdString();
  ASSERT_TRUE(manager_->stopDatasetAndWait(dataset, QString{}));

  EXPECT_EQ(
      probeLines().filter(QRegularExpression(u"^services "_s)),
      (QStringList{u"services start:ok"_s, u"services destroy:ok"_s}));
}

}  // namespace
}  // namespace PJ
