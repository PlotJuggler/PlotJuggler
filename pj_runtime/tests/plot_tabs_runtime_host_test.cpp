// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Drives PlotTabsRuntimeHost's raw C-ABI vtable through the SDK's
// PlotTabHostView (and, where the count-then-fill contract itself is under
// test, the raw vtable slots from host.raw()) with recording callbacks:
// argument pass-through, argument validation BEFORE the callback runs, the
// list/config borrow-lifetime contract, and unbound-callback errors.

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_runtime/PlotTabsRuntimeHost.h"

namespace {

struct RecordingCallbacks {
  struct CreateCall {
    std::string id;
    std::string title;
  };
  struct CurveCall {
    std::string id;
    std::string topic;
    std::string field;
    std::string dataset_source;
  };

  std::vector<CreateCall> create_calls;
  std::vector<std::string> close_calls;
  int list_calls = 0;
  std::vector<std::string> tab_config_calls;
  std::vector<CurveCall> add_calls;
  std::vector<CurveCall> remove_calls;
  std::vector<std::string> clear_calls;

  std::vector<std::string> tabs;
  std::string next_config_json = "{}";
  bool should_fail = false;

  PJ::PlotTabsRuntimeHost::Callbacks make() {
    return {
        .create_tab = [this](std::string_view id, std::string_view title) -> PJ::Status {
          if (should_fail) {
            return PJ::unexpected("boom");
          }
          create_calls.push_back({std::string(id), std::string(title)});
          return PJ::okStatus();
        },
        .close_tab = [this](std::string_view id) -> PJ::Status {
          if (should_fail) {
            return PJ::unexpected("boom");
          }
          close_calls.push_back(std::string(id));
          return PJ::okStatus();
        },
        .list_tab_ids = [this]() -> PJ::Expected<std::vector<std::string>> {
          if (should_fail) {
            return PJ::unexpected("boom");
          }
          ++list_calls;
          return tabs;
        },
        .tab_config = [this](std::string_view id) -> PJ::Expected<std::string> {
          if (should_fail) {
            return PJ::unexpected("boom");
          }
          tab_config_calls.push_back(std::string(id));
          return next_config_json;
        },
        .add_curve = [this](
                         std::string_view id, std::string_view topic, std::string_view field,
                         std::string_view dataset_source) -> PJ::Status {
          if (should_fail) {
            return PJ::unexpected("boom");
          }
          add_calls.push_back({std::string(id), std::string(topic), std::string(field), std::string(dataset_source)});
          return PJ::okStatus();
        },
        .remove_curve = [this](
                            std::string_view id, std::string_view topic, std::string_view field,
                            std::string_view dataset_source) -> PJ::Status {
          if (should_fail) {
            return PJ::unexpected("boom");
          }
          remove_calls.push_back(
              {std::string(id), std::string(topic), std::string(field), std::string(dataset_source)});
          return PJ::okStatus();
        },
        .clear_tab = [this](std::string_view id) -> PJ::Status {
          if (should_fail) {
            return PJ::unexpected("boom");
          }
          clear_calls.push_back(std::string(id));
          return PJ::okStatus();
        },
    };
  }
};

TEST(PlotTabsRuntimeHostTest, CreateForwardsIdAndTitle) {
  RecordingCallbacks cb;
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ::sdk::PlotTabHostView view(host.raw());

  ASSERT_TRUE(view.create("tab1", "My Tab"));
  ASSERT_EQ(cb.create_calls.size(), 1u);
  EXPECT_EQ(cb.create_calls[0].id, "tab1");
  EXPECT_EQ(cb.create_calls[0].title, "My Tab");
}

TEST(PlotTabsRuntimeHostTest, EmptyIdIsRejectedBeforeTheCallback) {
  RecordingCallbacks cb;
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ::sdk::PlotTabHostView view(host.raw());

  EXPECT_FALSE(view.create("", "title"));
  EXPECT_FALSE(view.close(""));
  EXPECT_FALSE(view.configOf(""));
  EXPECT_FALSE(view.addCurve("", "topic", "field"));
  EXPECT_FALSE(view.clear(""));

  EXPECT_TRUE(cb.create_calls.empty());
  EXPECT_TRUE(cb.close_calls.empty());
  EXPECT_TRUE(cb.tab_config_calls.empty());
  EXPECT_TRUE(cb.add_calls.empty());
  EXPECT_TRUE(cb.clear_calls.empty());
}

TEST(PlotTabsRuntimeHostTest, AddCurveRequiresTopicAndField) {
  RecordingCallbacks cb;
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ::sdk::PlotTabHostView view(host.raw());

  EXPECT_FALSE(view.addCurve("tab1", "", "field"));
  EXPECT_FALSE(view.addCurve("tab1", "topic", ""));
  EXPECT_TRUE(cb.add_calls.empty());

  ASSERT_TRUE(view.addCurve("tab1", "topic", "field"));  // empty dataset_source is legal
  ASSERT_EQ(cb.add_calls.size(), 1u);
  EXPECT_EQ(cb.add_calls[0].dataset_source, "");
}

TEST(PlotTabsRuntimeHostTest, ListIsCountThenFill) {
  RecordingCallbacks cb;
  cb.tabs = {"a", "b", "c"};
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ_plot_tab_host_t raw = host.raw();

  PJ_error_t err{};
  uint64_t total = 0;
  ASSERT_TRUE(raw.vtable->list_tab_ids(raw.ctx, nullptr, 0, &total, &err));
  EXPECT_EQ(total, 3u);

  std::vector<PJ_string_view_t> buffer(total);
  uint64_t filled = 0;
  ASSERT_TRUE(raw.vtable->list_tab_ids(raw.ctx, buffer.data(), buffer.size(), &filled, &err));
  ASSERT_EQ(filled, 3u);
  EXPECT_EQ(PJ::sdk::toStringView(buffer[0]), "a");
  EXPECT_EQ(PJ::sdk::toStringView(buffer[1]), "b");
  EXPECT_EQ(PJ::sdk::toStringView(buffer[2]), "c");

  // A capacity smaller than the total fills exactly `capacity` and reports it.
  std::vector<PJ_string_view_t> small(2);
  uint64_t small_filled = 0;
  ASSERT_TRUE(raw.vtable->list_tab_ids(raw.ctx, small.data(), small.size(), &small_filled, &err));
  EXPECT_EQ(small_filled, 2u);
}

TEST(PlotTabsRuntimeHostTest, ListWithNoTabsSucceeds) {
  RecordingCallbacks cb;
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ::sdk::PlotTabHostView view(host.raw());

  auto ids = view.list();
  ASSERT_TRUE(ids);
  EXPECT_TRUE(ids.value().empty());
}

TEST(PlotTabsRuntimeHostTest, ListedStringsSurviveUntilTheNextCall) {
  RecordingCallbacks cb;
  cb.tabs = {"first", "second"};
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ_plot_tab_host_t raw = host.raw();

  PJ_error_t err{};
  uint64_t total = 0;
  ASSERT_TRUE(raw.vtable->list_tab_ids(raw.ctx, nullptr, 0, &total, &err));
  std::vector<PJ_string_view_t> buffer(total);
  uint64_t filled = 0;
  ASSERT_TRUE(raw.vtable->list_tab_ids(raw.ctx, buffer.data(), buffer.size(), &filled, &err));

  // No other call on this vtable happens between fill and read: the views
  // must still read correctly.
  ASSERT_EQ(filled, 2u);
  EXPECT_EQ(PJ::sdk::toStringView(buffer[0]), "first");
  EXPECT_EQ(PJ::sdk::toStringView(buffer[1]), "second");
}

TEST(PlotTabsRuntimeHostTest, TabConfigReturnsTheShellsJson) {
  RecordingCallbacks cb;
  cb.next_config_json = R"({"title":"T","curves":[]})";
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ::sdk::PlotTabHostView view(host.raw());

  auto config = view.configOf("tab1");
  ASSERT_TRUE(config);
  EXPECT_EQ(config.value(), cb.next_config_json);
  ASSERT_EQ(cb.tab_config_calls.size(), 1u);
  EXPECT_EQ(cb.tab_config_calls[0], "tab1");
}

TEST(PlotTabsRuntimeHostTest, CallbackFailureSurfacesTheMessage) {
  RecordingCallbacks cb;
  cb.should_fail = true;
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ::sdk::PlotTabHostView view(host.raw());

  auto status = view.create("tab1", "title");
  EXPECT_FALSE(status);
  EXPECT_NE(status.error().find("boom"), std::string::npos);
}

TEST(PlotTabsRuntimeHostTest, UnwiredCallbacksReportUnsupported) {
  PJ::PlotTabsRuntimeHost host({});  // no callbacks wired
  PJ::sdk::PlotTabHostView view(host.raw());

  auto create = view.create("tab1", "title");
  EXPECT_FALSE(create);
  EXPECT_NE(create.error().find("does not support"), std::string::npos);

  auto close = view.close("tab1");
  EXPECT_FALSE(close);
  EXPECT_NE(close.error().find("does not support"), std::string::npos);

  auto list = view.list();
  EXPECT_FALSE(list);
  EXPECT_NE(list.error().find("does not support"), std::string::npos);

  auto config = view.configOf("tab1");
  EXPECT_FALSE(config);
  EXPECT_NE(config.error().find("does not support"), std::string::npos);

  auto add = view.addCurve("tab1", "topic", "field");
  EXPECT_FALSE(add);
  EXPECT_NE(add.error().find("does not support"), std::string::npos);

  auto remove = view.removeCurve("tab1", "topic", "field");
  EXPECT_FALSE(remove);
  EXPECT_NE(remove.error().find("does not support"), std::string::npos);

  auto clear = view.clear("tab1");
  EXPECT_FALSE(clear);
  EXPECT_NE(clear.error().find("does not support"), std::string::npos);
}

TEST(PlotTabsRuntimeHostTest, NullOutParamsAreRejected) {
  RecordingCallbacks cb;
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ_plot_tab_host_t raw = host.raw();

  PJ_error_t err{};
  EXPECT_FALSE(raw.vtable->list_tab_ids(raw.ctx, nullptr, 0, nullptr, &err));
  EXPECT_FALSE(raw.vtable->tab_config(raw.ctx, PJ::sdk::toAbiString("tab1"), nullptr, &err));
}

}  // namespace
