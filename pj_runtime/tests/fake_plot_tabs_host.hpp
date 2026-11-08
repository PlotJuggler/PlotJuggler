// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstdint>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <set>
#include <string>
#include <vector>

namespace PJ::testing {

// Fake pj.plot_tabs.v1 host: a MODEL of the host's tab state, not a recorder —
// every test in tool_registry_test.cpp reads the outcome of a tool call back
// through this struct (via view().configOf()/list()), exactly as plotTabTool's
// own tabReadBack() does. That is deliberate: the tool never trusts its own
// request as proof of what landed, so neither should the test.
//
// The one invariant every plot_tab test leans on: a tab seeded via
// addForeignTab() (ours=false, standing in for one of the user's own tabs)
// must be unreachable through every one of the 7 vtable slots below. The
// product rule is that the HOST enforces this scoping, not the plugin — the
// plugin just hands over a tab name — so each slot below checks ownership for
// itself rather than trusting a client-side check upstream.
struct FakePlotTabsHost {
  struct Curve {
    std::string topic;
    std::string field;
    std::string dataset;
  };
  struct Topic {
    std::string topic;
    std::string dataset;
    std::string type;
    bool visible = true;
  };
  struct Tab {
    std::string id;
    std::string title;
    std::string kind;  // "" = plot tab; "3d"/"2d" = scene tab
    std::vector<Curve> curves;
    std::vector<Topic> topics;  // scene tabs only
    bool ours = true;
  };

  std::vector<Tab> tabs;
  // Curve TOPICS the host cannot resolve. add_curve silently drops these
  // (returns true, adds nothing), modelling the real host failing to resolve
  // a series it was handed by parts — the exact failure the tool's read-back
  // through configOf() exists to catch, since the model never sees an error
  // for it otherwise.
  std::set<std::string> unresolvable;
  // Counts an actual WRITE that reached a tab this plugin does not own. Every
  // mutating slot below checks ownership before it touches `curves`, `title`,
  // or the `tabs` vector itself, so this stays 0 for the life of a correctly
  // behaving fake. It exists so that removing or reordering one of those
  // checks turns into an immediate, loud test failure instead of a silent
  // leak that only a human would notice on screen.
  int foreign_mutations = 0;
  bool fail_create = false;
  // Topic NAMES the host cannot resolve: attach_topic silently drops these
  // (returns true, attaches nothing), the scene-tab twin of `unresolvable`.
  std::set<std::string> unresolvable_topics;
  // Topic NAMES a "2d" tab refuses outright with an error.
  std::set<std::string> kind_rejects;
  // Id the last focus_tab call brought to the front.
  std::string focused;
  // vtable.struct_size the fake advertises: 64 = a 0.35 host without the tail
  // slots, 96 = the full 0.38 layout.
  uint32_t struct_size = sizeof(PJ_plot_tab_host_vtable_t);
  // Leave the tail slots NULL even when struct_size covers them (a host with
  // no scene workspace).
  bool null_tail = false;

  // Seeds one of the USER's own tabs — not this plugin's. Every test that
  // exercises the ownership boundary starts from one of these.
  Tab* addForeignTab(std::string id) {
    Tab t;
    t.id = std::move(id);
    t.ours = false;
    tabs.push_back(std::move(t));
    return &tabs.back();
  }

  // Seeds one of the USER's own scene tabs.
  Tab* addForeignView(std::string id, std::string kind = "3d") {
    Tab* t = addForeignTab(std::move(id));
    t->kind = std::move(kind);
    return t;
  }

  Tab* find(const std::string& id) {
    for (auto& t : tabs) {
      if (t.id == id) {
        return &t;
      }
    }
    return nullptr;
  }

  // The shared gate for every slot but create_tab (which treats "unknown" as
  // "make a new one" instead). Unknown and foreign ids are refused with the
  // same shape of error, so a probing model cannot tell "no such tab" apart
  // from "not yours" — which is the point: neither should exist to it.
  Tab* owned(const std::string& id, PJ_error_t* err) {
    Tab* t = find(id);
    if (t == nullptr) {
      PJ::sdk::fillError(err, 2, "fake", "unknown tab '" + id + "'");
      return nullptr;
    }
    if (!t->ours) {
      PJ::sdk::fillError(err, 2, "fake", "tab '" + id + "' is not owned by this plugin");
      return nullptr;
    }
    return t;
  }

  static Curve* findCurve(Tab& t, const std::string& topic, const std::string& field, const std::string& dataset) {
    for (auto& c : t.curves) {
      if (c.topic == topic && c.field == field && c.dataset == dataset) {
        return &c;
      }
    }
    return nullptr;
  }

  // Shared body of create_tab / create_tab_v2 (`kind` "" = plot). Same id and kind: a
  // plot tab is replaced by one empty plot, a scene tab only gets its title updated;
  // a different kind resets the tab to an empty one of the new kind.
  static bool createOfKind(
      FakePlotTabsHost* self, PJ_string_view_t id, std::string kind, PJ_string_view_t title, PJ_error_t* err) {
    if (self->fail_create) {
      PJ::sdk::fillError(err, 2, "fake", "create refused");
      return false;
    }
    if (kind == "plot") {
      kind.clear();
    }
    const std::string id_s(PJ::sdk::toStringView(id));
    const std::string title_s(PJ::sdk::toStringView(title));
    Tab* existing = self->find(id_s);
    if (existing != nullptr && !existing->ours) {
      // Upserting onto a name that already names one of the user's tabs would
      // silently take it over: refuse instead of clobbering it.
      PJ::sdk::fillError(err, 2, "fake", "tab '" + id_s + "' is not owned by this plugin");
      return false;
    }
    if (existing != nullptr) {
      if (existing->kind != kind || kind.empty()) {
        existing->kind = kind;
        existing->curves.clear();
        existing->topics.clear();
      }
      existing->title = title_s;
      return true;
    }
    Tab t;
    t.id = id_s;
    t.kind = kind;
    t.title = title_s;
    t.ours = true;
    self->tabs.push_back(std::move(t));
    return true;
  }

  static bool tCreate(void* ctx, PJ_string_view_t id, PJ_string_view_t title, PJ_error_t* err) noexcept {
    return createOfKind(static_cast<FakePlotTabsHost*>(ctx), id, "plot", title, err);
  }

  static bool tCreateV2(
      void* ctx, PJ_string_view_t id, PJ_string_view_t kind, PJ_string_view_t title, PJ_error_t* err) noexcept {
    return createOfKind(static_cast<FakePlotTabsHost*>(ctx), id, std::string(PJ::sdk::toStringView(kind)), title, err);
  }

  static bool tClose(void* ctx, PJ_string_view_t id, PJ_error_t* err) noexcept {
    auto* self = static_cast<FakePlotTabsHost*>(ctx);
    const std::string id_s(PJ::sdk::toStringView(id));
    if (self->owned(id_s, err) == nullptr) {
      return false;
    }
    self->tabs.erase(
        std::remove_if(self->tabs.begin(), self->tabs.end(), [&](const Tab& t) { return t.id == id_s; }),
        self->tabs.end());
    return true;
  }

  static bool tListIds(
      void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t* /*err*/) noexcept {
    auto* self = static_cast<FakePlotTabsHost*>(ctx);
    // Rebuilt on every call — the borrowed views below point into this
    // storage, which the ABI contract only guarantees until the NEXT call.
    self->list_storage_.clear();
    for (const auto& t : self->tabs) {
      if (t.ours) {
        self->list_storage_.push_back(t.id);
      }
    }
    if (capacity == 0) {
      *out_count = self->list_storage_.size();
      return true;
    }
    const uint64_t n = std::min<uint64_t>(capacity, self->list_storage_.size());
    for (uint64_t i = 0; i < n; ++i) {
      out_ids[i] = PJ::sdk::toAbiString(self->list_storage_[i]);
    }
    *out_count = n;
    return true;
  }

  static bool tTabConfig(void* ctx, PJ_string_view_t id, PJ_string_view_t* out_config_json, PJ_error_t* err) noexcept {
    auto* self = static_cast<FakePlotTabsHost*>(ctx);
    const std::string id_s(PJ::sdk::toStringView(id));
    Tab* t = self->owned(id_s, err);
    if (t == nullptr) {
      return false;
    }
    // Hand-built JSON: the test data carries no characters that need
    // escaping, so a JSON library buys nothing here.
    std::string body;
    if (t->kind.empty()) {
      body = "{\"title\":\"" + t->title + "\",\"curves\":[";
      for (std::size_t i = 0; i < t->curves.size(); ++i) {
        if (i != 0) {
          body += ",";
        }
        const Curve& c = t->curves[i];
        body += "{\"topic\":\"" + c.topic + "\",\"field\":\"" + c.field + "\",\"dataset\":\"" + c.dataset + "\"}";
      }
      body += "]}";
    } else {
      body = "{\"kind\":\"" + t->kind + "\",\"title\":\"" + t->title + "\",\"topics\":[";
      for (std::size_t i = 0; i < t->topics.size(); ++i) {
        if (i != 0) {
          body += ",";
        }
        const Topic& tp = t->topics[i];
        body += "{\"topic\":\"" + tp.topic + "\",\"dataset\":\"" + tp.dataset + "\",\"type\":\"" + tp.type +
                "\",\"visible\":" + (tp.visible ? "true" : "false") + "}";
      }
      body += "]}";
    }
    self->config_storage_ = std::move(body);
    *out_config_json = PJ::sdk::toAbiString(self->config_storage_);
    return true;
  }

  static bool tAddCurve(
      void* ctx, PJ_string_view_t id, PJ_string_view_t topic, PJ_string_view_t field, PJ_string_view_t dataset,
      PJ_error_t* err) noexcept {
    auto* self = static_cast<FakePlotTabsHost*>(ctx);
    const std::string id_s(PJ::sdk::toStringView(id));
    Tab* t = self->owned(id_s, err);
    if (t == nullptr) {
      return false;
    }
    const std::string topic_s(PJ::sdk::toStringView(topic));
    if (self->unresolvable.count(topic_s) != 0) {
      return true;  // silently dropped, as the real host would
    }
    const std::string field_s(PJ::sdk::toStringView(field));
    const std::string dataset_s(PJ::sdk::toStringView(dataset));
    if (findCurve(*t, topic_s, field_s, dataset_s) == nullptr) {
      t->curves.push_back(Curve{topic_s, field_s, dataset_s});
    }
    return true;
  }

  static bool tRemoveCurve(
      void* ctx, PJ_string_view_t id, PJ_string_view_t topic, PJ_string_view_t field, PJ_string_view_t dataset,
      PJ_error_t* err) noexcept {
    auto* self = static_cast<FakePlotTabsHost*>(ctx);
    const std::string id_s(PJ::sdk::toStringView(id));
    Tab* t = self->owned(id_s, err);
    if (t == nullptr) {
      return false;
    }
    const std::string topic_s(PJ::sdk::toStringView(topic));
    const std::string field_s(PJ::sdk::toStringView(field));
    const std::string dataset_s(PJ::sdk::toStringView(dataset));
    Curve* c = findCurve(*t, topic_s, field_s, dataset_s);
    if (c == nullptr) {
      PJ::sdk::fillError(err, 2, "fake", "curve not present on tab '" + id_s + "'");
      return false;
    }
    t->curves.erase(t->curves.begin() + (c - t->curves.data()));
    return true;
  }

  static bool tClearTab(void* ctx, PJ_string_view_t id, PJ_error_t* err) noexcept {
    auto* self = static_cast<FakePlotTabsHost*>(ctx);
    const std::string id_s(PJ::sdk::toStringView(id));
    Tab* t = self->owned(id_s, err);
    if (t == nullptr) {
      return false;
    }
    t->curves.clear();
    return true;
  }

  static Topic* findTopic(Tab& t, const std::string& topic, const std::string& dataset) {
    for (auto& tp : t.topics) {
      if (tp.topic == topic && tp.dataset == dataset) {
        return &tp;
      }
    }
    return nullptr;
  }

  static bool tAttachTopic(
      void* ctx, PJ_string_view_t id, PJ_string_view_t topic, PJ_string_view_t dataset, PJ_error_t* err) noexcept {
    auto* self = static_cast<FakePlotTabsHost*>(ctx);
    const std::string id_s(PJ::sdk::toStringView(id));
    Tab* t = self->owned(id_s, err);
    if (t == nullptr) {
      return false;
    }
    if (t->kind.empty()) {
      PJ::sdk::fillError(err, 2, "fake", "tab '" + id_s + "' is a plot tab: use add_curve");
      return false;
    }
    const std::string topic_s(PJ::sdk::toStringView(topic));
    if (self->unresolvable_topics.count(topic_s) != 0) {
      return true;  // silently dropped, as the real host would
    }
    if (t->kind == "2d" && self->kind_rejects.count(topic_s) != 0) {
      PJ::sdk::fillError(err, 2, "fake", "'" + topic_s + "' is not a type the \"2d\" tab accepts");
      return false;
    }
    const std::string dataset_s(PJ::sdk::toStringView(dataset));
    if (findTopic(*t, topic_s, dataset_s) == nullptr) {
      t->topics.push_back(Topic{topic_s, dataset_s, "kPointCloud", true});
    }
    return true;
  }

  static bool tDetachTopic(
      void* ctx, PJ_string_view_t id, PJ_string_view_t topic, PJ_string_view_t dataset, PJ_error_t* err) noexcept {
    auto* self = static_cast<FakePlotTabsHost*>(ctx);
    const std::string id_s(PJ::sdk::toStringView(id));
    Tab* t = self->owned(id_s, err);
    if (t == nullptr) {
      return false;
    }
    if (t->kind.empty()) {
      PJ::sdk::fillError(err, 2, "fake", "tab '" + id_s + "' is a plot tab: use remove_curve");
      return false;
    }
    const std::string topic_s(PJ::sdk::toStringView(topic));
    const std::string dataset_s(PJ::sdk::toStringView(dataset));
    Topic* tp = findTopic(*t, topic_s, dataset_s);
    if (tp == nullptr) {
      PJ::sdk::fillError(err, 2, "fake", "topic not present on tab '" + id_s + "'");
      return false;
    }
    t->topics.erase(t->topics.begin() + (tp - t->topics.data()));
    return true;
  }

  static bool tFocus(void* ctx, PJ_string_view_t id, PJ_error_t* err) noexcept {
    auto* self = static_cast<FakePlotTabsHost*>(ctx);
    const std::string id_s(PJ::sdk::toStringView(id));
    if (self->owned(id_s, err) == nullptr) {
      return false;
    }
    self->focused = id_s;
    return true;
  }

  // Built per call so struct_size/null_tail set by the test take effect; the
  // vtable lives in this object, which outlives the view in every test.
  PJ::sdk::PlotTabHostView view() {
    PJ_plot_tab_host_vtable_t vt{};
    vt.protocol_version = 1;
    vt.struct_size = struct_size;
    vt.create_tab = &FakePlotTabsHost::tCreate;
    vt.close_tab = &FakePlotTabsHost::tClose;
    vt.list_tab_ids = &FakePlotTabsHost::tListIds;
    vt.tab_config = &FakePlotTabsHost::tTabConfig;
    vt.add_curve = &FakePlotTabsHost::tAddCurve;
    vt.remove_curve = &FakePlotTabsHost::tRemoveCurve;
    vt.clear_tab = &FakePlotTabsHost::tClearTab;
    if (!null_tail) {
      vt.create_tab_v2 = &FakePlotTabsHost::tCreateV2;
      vt.attach_topic = &FakePlotTabsHost::tAttachTopic;
      vt.detach_topic = &FakePlotTabsHost::tDetachTopic;
      vt.focus_tab = &FakePlotTabsHost::tFocus;
    }
    vtable_ = vt;
    return PJ::sdk::PlotTabHostView(PJ_plot_tab_host_t{this, &vtable_});
  }

 private:
  PJ_plot_tab_host_vtable_t vtable_{};
  std::vector<std::string> list_storage_;
  std::string config_storage_;
};

}  // namespace PJ::testing
