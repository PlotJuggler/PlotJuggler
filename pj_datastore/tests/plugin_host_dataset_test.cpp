// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Dataset-creation contract of the plugin C-ABI write bridge. The per-source display offset is stored on the
// time domain, so a plugin-created dataset left on the default domain (id 0) cannot carry one.

#include <gtest/gtest.h>

#include <string>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"

namespace PJ {
namespace {

using namespace PJ::sdk;

struct Fixture {
  DataEngine engine;
  ObjectStore object_store;
  DatastoreToolboxHost toolbox_impl{engine, object_store};
  ToolboxHostView toolbox{toolbox_impl.raw()};
};

TEST(PluginHostDatasetTest, CreateDataSourceGivesEachSourceItsOwnTimeDomain) {
  Fixture f;
  const auto source_a = *f.toolbox.createDataSource("alpha");
  const auto source_b = *f.toolbox.createDataSource("beta");

  const DatasetInfo* dataset_a = f.engine.getDataset(source_a.id);
  const DatasetInfo* dataset_b = f.engine.getDataset(source_b.id);
  ASSERT_NE(dataset_a, nullptr);
  ASSERT_NE(dataset_b, nullptr);

  EXPECT_NE(dataset_a->time_domain.id, 0U) << "plugin source landed on the default domain";
  EXPECT_NE(dataset_b->time_domain.id, 0U) << "plugin source landed on the default domain";
  EXPECT_NE(dataset_a->time_domain.id, dataset_b->time_domain.id);
}

TEST(PluginHostDatasetTest, CreateDataSourceNamesTheDomainAfterTheSource) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("gamma");
  const DatasetInfo* dataset = f.engine.getDataset(source.id);
  ASSERT_NE(dataset, nullptr);

  const TimeDomain* domain = f.engine.getTimeDomain(dataset->time_domain.id);
  ASSERT_NE(domain, nullptr);
  EXPECT_EQ(domain->name, "gamma");
}

// The exact write the Source Timeline's bar drag performs.
TEST(PluginHostDatasetTest, PluginCreatedDatasetAcceptsADisplayOffset) {
  Fixture f;
  const auto source = *f.toolbox.createDataSource("delta");
  const DatasetInfo* dataset = f.engine.getDataset(source.id);
  ASSERT_NE(dataset, nullptr);
  ASSERT_NE(dataset->time_domain.id, 0U);

  f.engine.setDisplayOffset(dataset->time_domain.id, 1'500'000'000LL);

  const TimeDomain* domain = f.engine.getTimeDomain(dataset->time_domain.id);
  ASSERT_NE(domain, nullptr);
  EXPECT_EQ(domain->display_offset, 1'500'000'000LL);
}

TEST(PluginHostDatasetTest, DisplayOffsetIsIndependentPerSource) {
  Fixture f;
  const auto source_a = *f.toolbox.createDataSource("alpha");
  const auto source_b = *f.toolbox.createDataSource("beta");
  const DatasetInfo* dataset_a = f.engine.getDataset(source_a.id);
  const DatasetInfo* dataset_b = f.engine.getDataset(source_b.id);
  ASSERT_NE(dataset_a, nullptr);
  ASSERT_NE(dataset_b, nullptr);

  f.engine.setDisplayOffset(dataset_a->time_domain.id, 42);

  EXPECT_EQ(f.engine.getTimeDomain(dataset_a->time_domain.id)->display_offset, 42);
  EXPECT_EQ(f.engine.getTimeDomain(dataset_b->time_domain.id)->display_offset, 0);
}

}  // namespace
}  // namespace PJ
