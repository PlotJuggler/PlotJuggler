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
#include <utility>
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
  struct CreateV2Call {
    std::string id;
    std::string kind;
    std::string title;
  };
  struct TopicCall {
    std::string id;
    std::string topic;
    std::string dataset_source;
  };

  std::vector<CreateCall> create_calls;
  std::vector<std::string> close_calls;
  int list_calls = 0;
  std::vector<std::string> tab_config_calls;
  std::vector<CurveCall> add_calls;
  std::vector<CurveCall> remove_calls;
  std::vector<std::string> clear_calls;
  std::vector<CreateV2Call> create_v2_calls;
  std::vector<TopicCall> attach_calls;
  std::vector<TopicCall> detach_calls;
  std::vector<std::string> focus_calls;

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
        .create_tab_v2 = [this](std::string_view id, std::string_view kind, std::string_view title) -> PJ::Status {
          if (should_fail) {
            return PJ::unexpected("boom");
          }
          create_v2_calls.push_back({std::string(id), std::string(kind), std::string(title)});
          return PJ::okStatus();
        },
        .attach_topic =
            [this](std::string_view id, std::string_view topic, std::string_view dataset_source) -> PJ::Status {
          if (should_fail) {
            return PJ::unexpected("boom");
          }
          attach_calls.push_back({std::string(id), std::string(topic), std::string(dataset_source)});
          return PJ::okStatus();
        },
        .detach_topic =
            [this](std::string_view id, std::string_view topic, std::string_view dataset_source) -> PJ::Status {
          if (should_fail) {
            return PJ::unexpected("boom");
          }
          detach_calls.push_back({std::string(id), std::string(topic), std::string(dataset_source)});
          return PJ::okStatus();
        },
        .focus_tab = [this](std::string_view id) -> PJ::Status {
          if (should_fail) {
            return PJ::unexpected("boom");
          }
          focus_calls.push_back(std::string(id));
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

// GAP-44: a host fills each tail slot it implements, independently of the others, so
// a plugin probing one with PJ_HAS_TAIL_SLOT learns exactly what is served.
TEST(PlotTabsRuntimeHostTest, EachTailSlotIsPresentOnItsOwn) {
  RecordingCallbacks none_cb;
  auto none = none_cb.make();
  none.create_tab_v2 = nullptr;
  none.attach_topic = nullptr;
  none.detach_topic = nullptr;
  none.focus_tab = nullptr;
  PJ::PlotTabsRuntimeHost bare(std::move(none));
  const PJ_plot_tab_host_vtable_t* bare_vtable = bare.raw().vtable;
  EXPECT_EQ(bare_vtable->struct_size, sizeof(PJ_plot_tab_host_vtable_t));
  EXPECT_NE(bare_vtable->create_tab, nullptr);
  EXPECT_EQ(bare_vtable->create_tab_v2, nullptr);
  EXPECT_EQ(bare_vtable->attach_topic, nullptr);
  EXPECT_EQ(bare_vtable->detach_topic, nullptr);
  EXPECT_EQ(bare_vtable->focus_tab, nullptr);
  const auto refused = PJ::sdk::PlotTabHostView(bare.raw()).createTabV2("view", "3d");
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().find("create_tab_v2"), std::string::npos) << refused.error();

  for (int only = 0; only < 4; ++only) {
    RecordingCallbacks cb;
    auto callbacks = cb.make();
    if (only != 0) {
      callbacks.create_tab_v2 = nullptr;
    }
    if (only != 1) {
      callbacks.attach_topic = nullptr;
    }
    if (only != 2) {
      callbacks.detach_topic = nullptr;
    }
    if (only != 3) {
      callbacks.focus_tab = nullptr;
    }
    PJ::PlotTabsRuntimeHost host(std::move(callbacks));
    const PJ_plot_tab_host_vtable_t* vtable = host.raw().vtable;
    EXPECT_EQ(vtable->create_tab_v2 != nullptr, only == 0) << only;
    EXPECT_EQ(vtable->attach_topic != nullptr, only == 1) << only;
    EXPECT_EQ(vtable->detach_topic != nullptr, only == 2) << only;
    EXPECT_EQ(vtable->focus_tab != nullptr, only == 3) << only;
  }
}

TEST(PlotTabsRuntimeHostTest, V1CreateStillForwardsToCreateTab) {
  RecordingCallbacks cb;
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ::sdk::PlotTabHostView view(host.raw());

  ASSERT_TRUE(view.create("tab1", "T"));
  ASSERT_EQ(cb.create_calls.size(), 1u);
  EXPECT_EQ(cb.create_calls[0].id, "tab1");
  EXPECT_TRUE(cb.create_v2_calls.empty());
}

TEST(PlotTabsRuntimeHostTest, CreateV2ForwardsKindAndTitle) {
  RecordingCallbacks cb;
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ::sdk::PlotTabHostView view(host.raw());

  ASSERT_TRUE(view.createTabV2("view1", "3d", "My View"));
  ASSERT_EQ(cb.create_v2_calls.size(), 1u);
  EXPECT_EQ(cb.create_v2_calls[0].id, "view1");
  EXPECT_EQ(cb.create_v2_calls[0].kind, "3d");
  EXPECT_EQ(cb.create_v2_calls[0].title, "My View");
  EXPECT_TRUE(cb.create_calls.empty());

  ASSERT_TRUE(view.createTabV2("view2", "plot"));
  ASSERT_TRUE(view.createTabV2("view3", "2d"));
  EXPECT_EQ(cb.create_v2_calls.size(), 3u);
}

TEST(PlotTabsRuntimeHostTest, CreateV2IdWithSlashIsRejectedBeforeCallbacks) {
  RecordingCallbacks cb;
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ::sdk::PlotTabHostView view(host.raw());

  const auto status = view.createTabV2("plugin/view1", "3d", "title");
  ASSERT_FALSE(status);
  EXPECT_NE(status.error().find("'/'"), std::string::npos) << status.error();
  EXPECT_TRUE(cb.create_v2_calls.empty());
}

// The bridge knows the kinds of the contract; anything else is refused naming it,
// before the shell is asked. Whether this build can serve a known kind is the shell's
// answer (MainWindowPluginViewsTest.KnownUnservableKindIsRejectedByName).
TEST(PlotTabsRuntimeHostTest, CreateV2UnknownKindIsRejectedNamingIt) {
  RecordingCallbacks cb;
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ::sdk::PlotTabHostView view(host.raw());

  const auto unknown = view.createTabV2("view1", "4d", "title");
  ASSERT_FALSE(unknown);
  EXPECT_NE(unknown.error().find("'4d'"), std::string::npos) << unknown.error();
  const auto empty = view.createTabV2("view1", "", "title");
  ASSERT_FALSE(empty);
  EXPECT_NE(empty.error().find("''"), std::string::npos) << empty.error();
  EXPECT_TRUE(cb.create_v2_calls.empty());
}

TEST(PlotTabsRuntimeHostTest, SceneSlotsRejectEmptyIdBeforeTheCallback) {
  RecordingCallbacks cb;
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ::sdk::PlotTabHostView view(host.raw());

  for (const auto& status :
       {view.createTabV2("", "3d", "title"), view.attachTopic("", "topic"), view.detachTopic("", "topic"),
        view.focusTab("")}) {
    ASSERT_FALSE(status);
    EXPECT_NE(status.error().find("tab id must not be empty"), std::string::npos) << status.error();
  }
  for (const auto& status : {view.attachTopic("view1", ""), view.detachTopic("view1", "")}) {
    ASSERT_FALSE(status);
    EXPECT_NE(status.error().find("topic must not be empty"), std::string::npos) << status.error();
  }

  EXPECT_TRUE(cb.create_v2_calls.empty());
  EXPECT_TRUE(cb.attach_calls.empty());
  EXPECT_TRUE(cb.detach_calls.empty());
  EXPECT_TRUE(cb.focus_calls.empty());
}

TEST(PlotTabsRuntimeHostTest, SceneTabConfigIsBorrowedUntilNextCall) {
  RecordingCallbacks cb;
  cb.next_config_json = R"({"kind":"3d","title":"T","topics":[]})";
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ_plot_tab_host_t raw = host.raw();

  PJ_error_t err{};
  PJ_string_view_t out{};
  ASSERT_TRUE(raw.vtable->tab_config(raw.ctx, PJ::sdk::toAbiString("view1"), &out, &err));
  EXPECT_EQ(PJ::sdk::toStringView(out), cb.next_config_json);
  ASSERT_EQ(cb.tab_config_calls.size(), 1u);
  EXPECT_EQ(cb.tab_config_calls[0], "view1");
}

TEST(PlotTabsRuntimeHostTest, AttachDetachForwardDatasetSource) {
  RecordingCallbacks cb;
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ::sdk::PlotTabHostView view(host.raw());

  ASSERT_TRUE(view.attachTopic("view1", "topic", "source_a"));
  ASSERT_EQ(cb.attach_calls.size(), 1u);
  EXPECT_EQ(cb.attach_calls[0].id, "view1");
  EXPECT_EQ(cb.attach_calls[0].topic, "topic");
  EXPECT_EQ(cb.attach_calls[0].dataset_source, "source_a");

  ASSERT_TRUE(view.detachTopic("view1", "topic", "source_a"));
  ASSERT_EQ(cb.detach_calls.size(), 1u);
  EXPECT_EQ(cb.detach_calls[0].id, "view1");
  EXPECT_EQ(cb.detach_calls[0].topic, "topic");
  EXPECT_EQ(cb.detach_calls[0].dataset_source, "source_a");

  // Empty dataset_source is legal (means "unique across loaded datasets").
  ASSERT_TRUE(view.attachTopic("view1", "topic2"));
  ASSERT_EQ(cb.attach_calls.size(), 2u);
  EXPECT_EQ(cb.attach_calls[1].dataset_source, "");
}

TEST(PlotTabsRuntimeHostTest, FocusForwards) {
  RecordingCallbacks cb;
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ::sdk::PlotTabHostView view(host.raw());

  ASSERT_TRUE(view.focusTab("view1"));
  ASSERT_EQ(cb.focus_calls.size(), 1u);
  EXPECT_EQ(cb.focus_calls[0], "view1");
}

TEST(PlotTabsRuntimeHostTest, SceneCallbackFailureSurfacesMessage) {
  RecordingCallbacks cb;
  cb.should_fail = true;
  PJ::PlotTabsRuntimeHost host(cb.make());
  PJ::sdk::PlotTabHostView view(host.raw());

  for (const auto& status :
       {view.createTabV2("view1", "3d", "title"), view.attachTopic("view1", "topic"),
        view.detachTopic("view1", "topic"), view.focusTab("view1")}) {
    ASSERT_FALSE(status);
    EXPECT_NE(status.error().find("boom"), std::string::npos) << status.error();
  }
}

}  // namespace
