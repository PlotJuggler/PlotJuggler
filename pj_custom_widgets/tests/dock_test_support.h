#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Datastore and catalog scaffolding shared by the Custom Widget tests.

#include <gtest/gtest.h>

#include <QString>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_datastore/writer.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"

namespace pj_cw_test {

inline constexpr std::int64_t kSecond = 1'000'000'000;

inline PJ::DatasetId createDataset(PJ::AppSession& app, const char* source_name = "test") {
  auto dataset = app.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = source_name});
  if (!dataset.has_value()) {
    ADD_FAILURE() << dataset.error();
    return 0;
  }
  return *dataset;
}

/// Catalog key of the scalar field `field` of `topic` in `dataset` ("" when absent).
inline QString keyFor(PJ::AppSession& app, PJ::DatasetId dataset, const QString& topic, const QString& field) {
  for (const PJ::CatalogItem& item : app.catalogModel().items()) {
    const auto* scalar = PJ::asScalarField(item);
    if (scalar != nullptr && item.dataset_id == dataset && item.topic_name == topic &&
        (scalar->field_path == field || scalar->field_name == field)) {
      return item.key;
    }
  }
  return {};
}

/// A float topic with one sample per (time in seconds, value); returns its catalog key.
inline QString addNumberTopic(
    PJ::AppSession& app, PJ::DatasetId dataset, std::string_view topic,
    const std::vector<std::pair<double, double>>& samples) {
  PJ::DataWriter writer = app.sessionManager().dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset, topic, PJ::NumericType::kFloat64);
  if (!handle.has_value()) {
    ADD_FAILURE() << handle.error();
    return {};
  }
  for (const auto& [seconds, value] : samples) {
    writer.appendScalar(*handle, static_cast<std::int64_t>(seconds * static_cast<double>(kSecond)), value);
  }
  if (app.sessionManager().commitChunks(writer.flushAll()).empty()) {
    ADD_FAILURE() << "commit produced no changed topics";
    return {};
  }
  app.catalogModel().rebuildFromDatastore();
  return keyFor(
      app, dataset, QString::fromUtf8(topic.data(), static_cast<qsizetype>(topic.size())), QStringLiteral("value"));
}

/// A string topic (one column "state") with one sample per (time, text); returns its catalog key.
inline QString addTextTopic(
    PJ::AppSession& app, PJ::DatasetId dataset, std::string_view topic,
    const std::vector<std::pair<double, std::string>>& samples) {
  PJ::DataWriter writer = app.sessionManager().dataEngine().createWriter();
  auto id = writer.registerTopic(dataset, PJ::TopicDescriptor{.name = std::string(topic)});
  if (!id.has_value() || !writer.ensureColumn(*id, "state", PJ::PrimitiveType::kString).has_value()) {
    ADD_FAILURE() << "string topic registration failed";
    return {};
  }
  for (const auto& [seconds, text] : samples) {
    EXPECT_TRUE(writer.beginRow(*id, static_cast<std::int64_t>(seconds * static_cast<double>(kSecond))).has_value());
    writer.set<std::string_view>(*id, 0, text);
    EXPECT_TRUE(writer.finishRow(*id).has_value());
  }
  if (app.sessionManager().commitChunks(writer.flushAll()).empty()) {
    ADD_FAILURE() << "commit produced no changed topics";
    return {};
  }
  app.catalogModel().rebuildFromDatastore();
  return keyFor(
      app, dataset, QString::fromUtf8(topic.data(), static_cast<qsizetype>(topic.size())), QStringLiteral("state"));
}

}  // namespace pj_cw_test
