// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The maximum zoom-out extent (PlotWidgetBase::maxZoomRect, which also feeds the
// magnifier's wheel-zoom-out clamp) must track per-dataset display-offset edits.
// A Source Timeline drag emits SessionManager::displayOffsetChanged(DatasetId);
// the plot deliberately keeps its CURRENT zoom so the curve slides in place, but
// the max zoom-out rect is view-independent data state and must follow the
// curves' new X extent. Without that refresh the max range freezes at the
// pre-drag union: after aligning two far-apart recordings the user can still
// zoom out to the huge stale range (and, dragging the other way, canNOT zoom
// out far enough to reach the moved curve).

#include <gtest/gtest.h>

#include <QApplication>
#include <QRectF>
#include <QtGlobal>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace {

constexpr PJ::Timestamp kT0Ns = 1'600'000'000'000'000'000;  // ~2020 epoch, in ns
constexpr PJ::Timestamp kOneSecondNs = 1'000'000'000;
// Dataset B is recorded 100 s after dataset A — the "two MCAPs from different
// runs" gap the Source Timeline exists to align away.
constexpr PJ::Timestamp kGapNs = 100 * kOneSecondNs;

PJ::TopicId addScalarTopic(
    PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name, PJ::Timestamp start_ns) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle.has_value()) << handle.error();
  if (!handle.has_value()) {
    return 0;
  }
  writer.appendScalar(*handle, start_ns, 1.0);
  writer.appendScalar(*handle, start_ns + kOneSecondNs, 2.0);
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  return handle->topic_id;
}

QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name); descriptor && descriptor->topic_id == topic_id) {
      return curve.name;
    }
  }
  return {};
}

// One plot showing a curve from each of two datasets whose raw recordings sit
// kGapNs apart. Each dataset owns its TimeDomain (mirrors FileLoader), so the
// Source Timeline's per-source offset writes have somewhere to land.
struct TwoDatasetFixture {
  PJ::SessionManager session;
  PJ::CatalogModel catalog{&session};
  PJ::PlotWidget plot{&session, &catalog};
  PJ::DatasetId dataset_b = 0;

  TwoDatasetFixture() {
    const auto make_dataset = [this](const char* domain_name, const char* source_name) -> PJ::DatasetId {
      auto domain = session.dataEngine().createTimeDomain(domain_name);
      EXPECT_TRUE(domain.has_value()) << domain.error();
      auto dataset = session.dataEngine().createDataset(
          PJ::DatasetDescriptor{.source_name = source_name, .time_domain_id = *domain});
      EXPECT_TRUE(dataset.has_value()) << dataset.error();
      return *dataset;
    };

    const PJ::DatasetId dataset_a = make_dataset("run_a", "run_a.mcap");
    dataset_b = make_dataset("run_b", "run_b.mcap");
    const PJ::TopicId topic_a = addScalarTopic(session, dataset_a, "/pose/x", kT0Ns);
    const PJ::TopicId topic_b = addScalarTopic(session, dataset_b, "/pose/x", kT0Ns + kGapNs);

    EXPECT_NE(plot.addCurve(keyForTopic(catalog, topic_a)), nullptr);
    EXPECT_NE(plot.addCurve(keyForTopic(catalog, topic_b)), nullptr);
  }
};

}  // namespace

// A per-dataset offset edit (Timeline drag) must refresh maxZoomRect in BOTH
// directions: aligning B onto A shrinks the union [0, 101] -> [0, 1]; dragging B
// the other way grows it to [0, 201], which zoom-out must be able to reach.
TEST(PlotWidgetMaxZoomOffset, MaxZoomRectTracksPerDatasetOffsetChange) {
  TwoDatasetFixture fixture;
  fixture.session.setUseTimeOffset(true);  // display frame starts at A's first sample

  // Seed the max zoom area at the unaligned union: A at [0, 1], B at [100, 101].
  fixture.plot.zoomOut(/*emit_signal=*/false);
  ASSERT_NEAR(fixture.plot.maxZoomRect().left(), 0.0, 1e-6);
  ASSERT_NEAR(fixture.plot.maxZoomRect().right(), 100.0 + 1.0, 1e-6);

  // Timeline drag: align B's start onto A's (per-source shift = the raw gap).
  fixture.session.setDisplayOffset(fixture.dataset_b, PJ::DisplayOffset{PJ::Duration{kGapNs}});
  EXPECT_NEAR(fixture.plot.maxZoomRect().left(), 0.0, 1e-6);
  EXPECT_NEAR(fixture.plot.maxZoomRect().right(), 1.0, 1e-6) << "max zoom-out extent must shrink to the aligned union";

  // Drag B the other way: display [200, 201]. The max extent must grow so
  // zoom-out can reach the moved curve.
  fixture.session.setDisplayOffset(fixture.dataset_b, PJ::DisplayOffset{PJ::Duration{-kGapNs}});
  EXPECT_NEAR(fixture.plot.maxZoomRect().right(), 200.0 + 1.0, 1e-6)
      << "max zoom-out extent must grow to include the moved curve";
}
