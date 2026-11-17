// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Liveness of ExtensionCatalogService's cross-thread parser-catalog access.
//
// The demand-driven streaming path resolves parsers on a plugin's poll thread (advertise-time classification, per-topic
// delegated-ingest binding) while the GUI thread can reload() the catalog (Marketplace install/uninstall). reload()
// clears/reallocates the parser vector, so an unsynchronized reader holding a raw catalog pointer would dangle. A
// shared_mutex guards it: reload() takes the exclusive lock; resolveParserRoutes()/parserEncodings() take a shared lock
// and never leak a raw catalog pointer past it.
//
// This test hammers both sides concurrently and asserts the run completes: a mis-ordered or up-graded lock (a reader
// taking the exclusive lock, a lock-ordering inversion) deadlocks and is caught by the ctest per-test timeout. It is a
// liveness guard, not a data-race detector: the raced vector is owned by pj_plugins, which the --tsan build does not
// instrument (its add_subdirectory precedes the sanitizer flags), and an empty catalog never reallocates, so TSan
// cannot see the writer side. A decisive race test would instrument pj_plugins under TSan and use a populated,
// churning catalog.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "pj_runtime/ExtensionCatalogService.h"
using namespace Qt::StringLiterals;

namespace {

TEST(ExtensionCatalogServiceThreadSafety, ConcurrentReloadAndParserResolutionStayLive) {
  // Default (empty) extensions dir: no plugins load, but reload() and the accessors still take their locks and touch
  // the shared catalog.
  PJ::ExtensionCatalogService catalog;

  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&catalog, &stop]() {
      while (!stop.load(std::memory_order_relaxed)) {
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
