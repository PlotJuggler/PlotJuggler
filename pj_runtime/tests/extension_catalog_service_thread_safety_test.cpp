// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Liveness of ExtensionCatalogService's cross-thread parser-catalog access.
//
// The demand-driven streaming path resolves parsers on a plugin's poll thread
// (advertise-time classification + per-topic delegated-ingest binding), while
// the GUI thread can reload() the catalog (Marketplace install/uninstall).
// reload() clears/reallocates the underlying parser vector, so an unsynchronized
// reader holding a raw catalog pointer would dangle (use-after-free). The fix
// guards that vector with a shared_mutex: reload() takes the exclusive lock;
// resolveParserRoutes()/parserEncodings() take a shared lock and never
// leak a raw catalog pointer past it (correctness rests on that mutual
// exclusion, verifiable by inspection).
//
// What THIS test covers: it hammers both sides concurrently and asserts the run
// completes — a regression that mis-orders or up-grades the locks (e.g. a
// reader taking the exclusive lock, or a lock-ordering inversion) deadlocks and
// is caught by the ctest per-test timeout. It is a LIVENESS/deadlock guard, not
// a data-race detector: the race lives on a vector owned by pj_plugins, which
// is not instrumented in the --tsan build (its add_subdirectory precedes the
// sanitizer flags), and an empty catalog never reallocates — so TSan cannot
// observe the writer side here. A decisive race test would require instrumenting
// pj_plugins under TSan and a populated, churning catalog.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "pj_runtime/ExtensionCatalogService.h"
using namespace Qt::StringLiterals;

namespace {

TEST(ExtensionCatalogServiceThreadSafety, ConcurrentReloadAndParserResolutionStayLive) {
  // Default (empty) extensions dir: no plugins load, but reload() and the
  // accessors still take their locks and touch the shared catalog, which is all
  // this liveness check needs (see the file header for why it is not a race
  // detector).
  PJ::ExtensionCatalogService catalog;

  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&catalog, &stop]() {
      while (!stop.load(std::memory_order_relaxed)) {
        // Both cross-thread-safe accessors: one resolves+instantiates parsers
        // under the routing + shared catalog locks, the other snapshots the
        // encoding set.
        const auto selection = catalog.resolveParserRoutes("json", "any/Type", {}, {});
        (void)selection.scalar.has_value();
        const auto encodings = catalog.parserEncodings();
        (void)encodings.size();
      }
    });
  }

  // Writer: hammer reload() (exclusive lock) while the readers run.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  int reloads = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    catalog.reload();
    ++reloads;
  }

  stop.store(true, std::memory_order_relaxed);
  for (auto& reader : readers) {
    reader.join();
  }

  EXPECT_GT(reloads, 0);  // the writer made progress (no deadlock)
}

}  // namespace

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
