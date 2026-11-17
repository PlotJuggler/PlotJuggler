// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A lazy ObjectStore payload anchor carries the plugin's `release` function pointer, which must not be invoked after
// the plugin .so is dlclosed: the host wrapper keeps the producing DSO's token alive until the anchor is gone.

#include "pj_runtime/detail/payload_anchor.h"

#include <gtest/gtest.h>

#include <memory>

namespace {

struct AnchorProbe {
  bool* dso_mapped = nullptr;
  bool release_ran = false;
  bool released_after_unload = false;
};

// Simulates the plugin's C-ABI anchor.release: records when it ran and whether
// the "DSO" was already unloaded at that point (which, with a real .so, would be
// a jump into unmapped memory — the crash).
void probeRelease(void* ctx) noexcept {
  auto* probe = static_cast<AnchorProbe*>(ctx);
  probe->release_ran = true;
  if (probe->dso_mapped != nullptr && !*probe->dso_mapped) {
    probe->released_after_unload = true;
  }
}

}  // namespace

TEST(PayloadAnchorKeepalive, ReleaseRunsWhileDsoStillMapped) {
  bool dso_mapped = true;
  // The plugin DSO token: when its last copy drops, the ".so" unloads.
  auto library_keepalive =
      std::shared_ptr<void>(reinterpret_cast<void*>(0x1), [&dso_mapped](void*) { dso_mapped = false; });

  AnchorProbe probe{&dso_mapped};
  PJ_payload_anchor_t anchor{};
  anchor.ctx = &probe;
  anchor.release = &probeRelease;

  PJ::sdk::BufferAnchor wrapped = PJ::detail::wrapPayloadAnchor(anchor, library_keepalive);

  // The extension catalog tears down and drops its DSO token. The wrapped anchor
  // must hold its OWN copy, so the ".so" stays mapped.
  library_keepalive.reset();
  EXPECT_TRUE(dso_mapped) << "wrapped anchor must keep the producing plugin DSO token alive";
  EXPECT_FALSE(probe.release_ran);

  // Destroying the anchor runs release — and it must run BEFORE the DSO unloads.
  wrapped.reset();
  EXPECT_TRUE(probe.release_ran);
  EXPECT_FALSE(probe.released_after_unload) << "plugin release fn was called after the DSO unloaded (UAF)";
  EXPECT_FALSE(dso_mapped) << "the captured keepalive drops once the anchor is destroyed";
}

TEST(PayloadAnchorKeepalive, NullReleaseYieldsEmptyAnchor) {
  PJ_payload_anchor_t anchor{};  // release == nullptr → no ownership to track.
  auto wrapped = PJ::detail::wrapPayloadAnchor(anchor, nullptr);
  EXPECT_EQ(wrapped, nullptr);
}
