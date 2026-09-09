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

// Regression: the timeline bar extent (and thus the needle's clamp) must match the
// authoritative PlaybackEngine range, which is recomputed comprehensively over both
// scalar AND lazily-ingested object topics. Catalog itemsAdded fires only when a
// topic first appears, and samplesIngested is scalar-only — so an extent that grows
// after the topic is catalogued (progressive file ingest, an object/image topic
// filling in, a streaming live edge) is NOT seen by either. recomputeRange catches
// it and emits rangeChanged; the controller rebuilds the bars off THAT signal so the
// bars never fall short of the playback range (which caused the needle to max out at
// ~70% of the handle and playback to run past the bars).
TEST(SourceTimelineController, BarExtentTracksRecomputedRange) {
  AppSession session;
  DataEngine& engine = session.sessionManager().dataEngine();
  const auto domain = engine.createTimeDomain("src");
  ASSERT_TRUE(domain.has_value()) << domain.error();
  const auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "drive.mcap", .time_domain_id = *domain});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  // One writer/handle kept alive so the second phase GROWS the same topic rather
  // than minting a new one (registerScalarSeries creates a fresh topic each call).
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(*dataset, "/x", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();

  // Phase 1: data spans [0, 10] s. Build the catalog so the topic is visible, and
  // seed the authoritative range.
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

  // Phase 2: the SAME topic grows to 20 s WITHOUT a catalog change. recomputeRange
  // (the file-load completion / live-edge path) picks it up and emits rangeChanged,
  // which must drive the bars to follow — synchronously, no event loop needed.
  writer.appendScalar(*handle, 20'000'000'000LL, 1.0);
  std::ignore = session.sessionManager().commitChunks(writer.flushAll());
  session.recomputeRange();

  const TimeSpan grown = widget.sceneExtentForTest();
  EXPECT_EQ(grown.max, 20'000'000'000LL) << "timeline bars did not follow the recomputed range";
}

// Regression: with "Use time offset" ON, a dataset loaded AFTER the controller is
// wired moves the global reference from 0 to the epoch. The widget's time-frame
// offset must follow (refreshed on rangeChanged) AND re-frame the playhead, or the
// needle strands at the small playback value (~0) while the bars sit at epoch ns —
// "the needle doesn't appear at the right place, with its own min/max".
TEST(SourceTimelineController, NeedleFollowsFrameOnLoadWithOffsetOn) {
  constexpr Timestamp kEpoch = 1'700'000'000'000'000'000LL;
  AppSession session;
  session.sessionManager().setUseTimeOffset(true);  // global frame on; no data yet -> ref 0

  Timeline widget;
  SourceTimelineController controller(&widget, &session);  // seeds with ref 0

  // Load an epoch-scale dataset now — the global reference jumps 0 -> kEpoch.
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

  // Bars live at raw epoch ns; the needle must too (snapped to the data start), NOT
  // at the small playback value under a stale 0 frame offset.
  EXPECT_EQ(widget.sceneExtentForTest().min, kEpoch);
  EXPECT_EQ(widget.playheadNsForTest(), kEpoch);
}

}  // namespace
}  // namespace PJ
