// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Drives ViewportRuntimeHost's raw C-ABI vtable through the SDK's
// ViewportHostView with recording callbacks: argument pass-through, argument
// validation BEFORE the callback runs, and unbound-callback errors.

#include <gtest/gtest.h>

#include <limits>
#include <string>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_runtime/ViewportRuntimeHost.h"

namespace {

struct RecordingCallbacks {
  int zoom_calls = 0;
  double last_t0 = 0.0;
  double last_t1 = 0.0;
  int reset_calls = 0;
  bool should_fail = false;

  PJ::ViewportRuntimeHost::Callbacks make() {
    return {
        .zoom_to_time_range = [this](double t0, double t1) -> PJ::Status {
          if (should_fail) {
            return PJ::unexpected("no open time plot");
          }
          ++zoom_calls;
          last_t0 = t0;
          last_t1 = t1;
          return PJ::okStatus();
        },
        .zoom_reset = [this]() -> PJ::Status {
          if (should_fail) {
            return PJ::unexpected("no open plot");
          }
          ++reset_calls;
          return PJ::okStatus();
        },
    };
  }
};

TEST(ViewportRuntimeHostTest, ZoomForwardsRange) {
  RecordingCallbacks cb;
  PJ::ViewportRuntimeHost host(cb.make());
  PJ::sdk::ViewportHostView view(host.raw());

  ASSERT_TRUE(view.zoomToTimeRange(2.0, 8.5));
  EXPECT_EQ(cb.zoom_calls, 1);
  EXPECT_DOUBLE_EQ(cb.last_t0, 2.0);
  EXPECT_DOUBLE_EQ(cb.last_t1, 8.5);
}

TEST(ViewportRuntimeHostTest, ZoomRejectsBadRangeBeforeCallback) {
  RecordingCallbacks cb;
  PJ::ViewportRuntimeHost host(cb.make());
  PJ::sdk::ViewportHostView view(host.raw());

  EXPECT_FALSE(view.zoomToTimeRange(5.0, 5.0));                                       // empty
  EXPECT_FALSE(view.zoomToTimeRange(9.0, 1.0));                                       // inverted
  EXPECT_FALSE(view.zoomToTimeRange(std::numeric_limits<double>::quiet_NaN(), 1.0));  // NaN
  EXPECT_FALSE(view.zoomToTimeRange(0.0, std::numeric_limits<double>::infinity()));   // inf
  EXPECT_EQ(cb.zoom_calls, 0);  // validation happens before the callback
}

TEST(ViewportRuntimeHostTest, ResetForwards) {
  RecordingCallbacks cb;
  PJ::ViewportRuntimeHost host(cb.make());
  PJ::sdk::ViewportHostView view(host.raw());

  ASSERT_TRUE(view.zoomReset());
  EXPECT_EQ(cb.reset_calls, 1);
}

TEST(ViewportRuntimeHostTest, CallbackFailureSurfacesError) {
  RecordingCallbacks cb;
  cb.should_fail = true;
  PJ::ViewportRuntimeHost host(cb.make());
  PJ::sdk::ViewportHostView view(host.raw());

  auto status = view.zoomToTimeRange(0.0, 1.0);
  EXPECT_FALSE(status);
  EXPECT_NE(status.error().find("no open time plot"), std::string::npos);
}

TEST(ViewportRuntimeHostTest, UnboundCallbacksReportUnsupported) {
  PJ::ViewportRuntimeHost host({});  // no callbacks wired
  PJ::sdk::ViewportHostView view(host.raw());

  auto zoom = view.zoomToTimeRange(0.0, 1.0);
  EXPECT_FALSE(zoom);
  EXPECT_NE(zoom.error().find("does not support"), std::string::npos);
  auto reset = view.zoomReset();
  EXPECT_FALSE(reset);
  EXPECT_NE(reset.error().find("does not support"), std::string::npos);
}

}  // namespace
