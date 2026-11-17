// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <tuple>

#include "SourceTimelineController.h"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/Timeline.h"

namespace PJ {
namespace {

// The timeline bars follow the PlaybackEngine range, which is recomputed over scalar and lazily-ingested object
// topics. An extent that grows after its topic is catalogued fires neither the catalog's itemsAdded nor
// samplesIngested (scalar-only), so the controller rebuilds the bars off rangeChanged; otherwise they fall short of
// what playback traverses.
TEST(SourceTimelineController, BarExtentTracksRecomputedRange) {
  AppSession session;
  DataEngine& engine = session.sessionManager().dataEngine();
  const auto domain = engine.createTimeDomain("src");
  ASSERT_TRUE(domain.has_value()) << domain.error();
  const auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "drive.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  // One writer/handle kept alive so the second step grows the same topic rather than registering another.
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(*dataset, "/x", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();

  // Data spans [0, 10] s: build the catalog so the topic is visible and seed the authoritative range.
  writer.appendScalar(*handle, 0, 1.0);
  writer.appendScalar(*handle, 10'000'000'000LL, 1.0);
  std::ignore = session.sessionManager().commitChunks(writer.flushAll());
  session.catalogModel().rebuildFromDatastore();
  session.recomputeRange();

  Timeline widget;
  SourceTimelineController controller(&widget, &session);  // ctor builds the initial tracks

  const TimeSpan initial = widget.sceneExtentForTest();
  EXPECT_EQ(initial.min, 0);
  EXPECT_EQ(initial.max, 10'000'000'000LL);

  // The same topic grows to 20 s without a catalog change. recomputeRange (the file-load completion / live-edge
  // path) emits rangeChanged, which must drive the bars synchronously, with no event loop.
  writer.appendScalar(*handle, 20'000'000'000LL, 1.0);
  std::ignore = session.sessionManager().commitChunks(writer.flushAll());
  session.recomputeRange();

  const TimeSpan grown = widget.sceneExtentForTest();
  EXPECT_EQ(grown.max, 20'000'000'000LL) << "timeline bars did not follow the recomputed range";
}

// With "Use time offset" on, a dataset loaded after the controller is wired moves the global reference from 0 to
// the epoch. The widget's time-frame offset must follow (refreshed on rangeChanged) and re-frame the playhead, or
// the needle strands at the small playback value while the bars sit at epoch ns.
TEST(SourceTimelineController, NeedleFollowsFrameOnLoadWithOffsetOn) {
  constexpr Timestamp kEpoch = 1'700'000'000'000'000'000LL;
  AppSession session;
  session.sessionManager().setUseTimeOffset(true);  // global frame on; no data yet -> ref 0

  Timeline widget;
  SourceTimelineController controller(&widget, &session);  // seeds with ref 0

  // An epoch-scale dataset: the global reference jumps 0 -> kEpoch.
  DataEngine& engine = session.sessionManager().dataEngine();
  const auto domain = engine.createTimeDomain("a");
  ASSERT_TRUE(domain.has_value());
  const auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "a.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value());
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(*dataset, "/x", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value());
  writer.appendScalar(*handle, kEpoch, 1.0);
  writer.appendScalar(*handle, kEpoch + 10'000'000'000LL, 2.0);
  std::ignore = session.sessionManager().commitChunks(writer.flushAll());
  session.catalogModel().rebuildFromDatastore();
  session.seedPlaybackFromSession();  // recompute range + snap currentTime to the data min

  // Bars live at raw epoch ns; the needle must too (snapped to the data start), not at the small playback value
  // under a stale 0 frame offset.
  EXPECT_EQ(widget.sceneExtentForTest().min, kEpoch);
  EXPECT_EQ(widget.playheadNsForTest(), kEpoch);
}

}  // namespace
}  // namespace PJ
