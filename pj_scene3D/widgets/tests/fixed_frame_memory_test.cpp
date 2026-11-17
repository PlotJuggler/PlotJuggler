// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// FixedFrameMemory: the last fixed frame the user picked by hand per dataset.
// In-session by DatasetId, cross-restart by the dataset's source path + name.

#include <gtest/gtest.h>

#include <QSettings>
#include <QString>

#include "pj_base/dataset.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/FixedFrameMemory.h"
using namespace Qt::StringLiterals;

namespace {

using pj::scene3d::FixedFrameMemory;

constexpr char kFixedFrameGroup[] = "pj_scene3d/fixed_frame_by_source";

void clearPersistedFixedFrames() {
  QSettings settings;
  settings.beginGroup(QLatin1String(kFixedFrameGroup));
  settings.remove(QString());
}

TEST(FixedFrameMemory, RemembersFixedFramePerDatasetInSession) {
  PJ::SessionManager session;
  FixedFrameMemory memory(session);
  memory.remember(/*dataset_id=*/7, u"odom"_s);
  EXPECT_EQ(memory.remembered(7), u"odom"_s);
  EXPECT_EQ(memory.remembered(8), QString()) << "a different dataset has no remembered frame";
}

// Cross-session: the choice persists keyed by the dataset's source path + name, so a fresh memory (a restart) with a
// NEW DatasetId for the SAME file resolves it.
TEST(FixedFrameMemory, RememberedFixedFramePersistsAcrossSessionsBySource) {
  clearPersistedFixedFrames();
  PJ::SessionManager session_a;
  const auto id_a =
      session_a.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "robot_log.mcap", .time_domain_id = 0});
  ASSERT_TRUE(id_a.has_value());
  session_a.setDatasetSourcePath(*id_a, u"/data/run_a/robot_log.mcap"_s);
  {
    FixedFrameMemory memory_a(session_a);
    memory_a.remember(*id_a, u"map"_s);
  }

  // A separate session/memory with its own empty in-session cache stands in for an
  // app restart; the same file is now a DIFFERENT DatasetId but the SAME source.
  PJ::SessionManager session_b;
  const auto id_b =
      session_b.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "robot_log.mcap", .time_domain_id = 0});
  ASSERT_TRUE(id_b.has_value());
  session_b.setDatasetSourcePath(*id_b, u"/data/run_a/robot_log.mcap"_s);
  FixedFrameMemory memory_b(session_b);
  EXPECT_EQ(memory_b.remembered(*id_b), u"map"_s)
      << "the manual choice must persist across sessions, keyed by source identity (not the DatasetId)";
}

// Two files sharing a basename in different folders must NOT share a remembered frame (name-only keying would).
TEST(FixedFrameMemory, SameNameDifferentPathDoesNotShareRememberedFrame) {
  clearPersistedFixedFrames();
  PJ::SessionManager session;
  const auto id_a =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "robot_log.mcap", .time_domain_id = 0});
  const auto id_b =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "robot_log.mcap", .time_domain_id = 0});
  ASSERT_TRUE(id_a.has_value());
  ASSERT_TRUE(id_b.has_value());
  session.setDatasetSourcePath(*id_a, u"/data/run_a/robot_log.mcap"_s);
  session.setDatasetSourcePath(*id_b, u"/data/run_b/robot_log.mcap"_s);
  {
    FixedFrameMemory memory(session);
    memory.remember(*id_a, u"odom"_s);
  }
  FixedFrameMemory fresh(session);  // empty in-session cache -> both resolve via QSettings
  EXPECT_EQ(fresh.remembered(*id_a), u"odom"_s);
  EXPECT_EQ(fresh.remembered(*id_b), QString())
      << "a same-named file in a different folder must not inherit the other's frame";
}

// Fan-out members share the file path but carry distinct source names: the composite key keeps their frames separate.
TEST(FixedFrameMemory, FanOutSiblingsSamePathStayDistinct) {
  clearPersistedFixedFrames();
  PJ::SessionManager session;
  const auto id_a =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "member_a", .time_domain_id = 0});
  const auto id_b =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "member_b", .time_domain_id = 0});
  ASSERT_TRUE(id_a.has_value());
  ASSERT_TRUE(id_b.has_value());
  session.setDatasetSourcePath(*id_a, u"/data/bundle.mcap"_s);
  session.setDatasetSourcePath(*id_b, u"/data/bundle.mcap"_s);
  {
    FixedFrameMemory memory(session);
    memory.remember(*id_a, u"base_link"_s);
  }
  FixedFrameMemory fresh(session);
  EXPECT_EQ(fresh.remembered(*id_a), u"base_link"_s);
  EXPECT_EQ(fresh.remembered(*id_b), QString())
      << "fan-out siblings share a path but must not share a remembered frame";
}

// A pathless source with a stable name (e.g. a live stream) still persists by name alone.
TEST(FixedFrameMemory, PathlessNamedSourcePersistsByNameAlone) {
  clearPersistedFixedFrames();
  PJ::SessionManager session_a;
  const auto id_a =
      session_a.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "ros2_stream", .time_domain_id = 0});
  ASSERT_TRUE(id_a.has_value());
  {
    FixedFrameMemory memory_a(session_a);
    memory_a.remember(*id_a, u"map"_s);
  }
  PJ::SessionManager session_b;
  const auto id_b =
      session_b.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "ros2_stream", .time_domain_id = 0});
  ASSERT_TRUE(id_b.has_value());
  FixedFrameMemory memory_b(session_b);
  EXPECT_EQ(memory_b.remembered(*id_b), u"map"_s) << "a named but pathless source must keep its cross-restart memory";
}

// A dataset with no stable source name is remembered for this session only.
TEST(FixedFrameMemory, SourcelessDatasetIsSessionOnly) {
  PJ::SessionManager session;
  {
    FixedFrameMemory memory_a(session);
    memory_a.remember(/*dataset_id=*/424242, u"odom"_s);  // no engine dataset -> no source
    EXPECT_EQ(memory_a.remembered(424242), u"odom"_s) << "remembered within the session";
  }
  FixedFrameMemory memory_b(session);  // fresh in-session cache == a restart
  EXPECT_EQ(memory_b.remembered(424242), QString()) << "a source-less dataset must not persist across sessions";
}

}  // namespace
