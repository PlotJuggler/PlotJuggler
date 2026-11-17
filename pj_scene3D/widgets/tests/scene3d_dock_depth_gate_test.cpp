// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Dock-level "droppable into 3D" gate: Scene3DDockWidget::addTopic accepts a DEPTH-encoded sdk::Image (kImage) as a
// DepthCloud layer but refuses a COLOUR image sharing the kImage type, so a depth topic dropped on a 3D dock becomes a
// layer while a colour image is rejected (the host's "committed object dock refused the topic" path fires).
//
// Exercised through the public IObjectViewer surface against a real SessionManager/ObjectStore; the view is created
// lazily (a queued singleShot in the base ctor) and never shown, so no GPU is required (addTopic touches the view only
// behind `view_ != nullptr`, and a DepthCloudLayer is pure CPU).

#include <gtest/gtest.h>

#include <QApplication>
#include <QString>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TransformService.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
using namespace Qt::StringLiterals;

namespace {

using namespace pj::scene3d::test;

constexpr std::string_view kImageSchema = "mock/image";

// 2x2 32FC1-shaped bytes; reinterpreted as the chosen encoding's pixels. Only the
// encoding string matters to the gate (firstSampleIsDepthEncoded peeks it).
const std::array<float, 4> kPixels = {1.0f, 2.0f, 3.0f, 4.0f};

PJ::Expected<PJ::sdk::ObjectRecord> emitImageWithEncoding(PJ::Timestamp ts, std::string_view encoding) {
  PJ::sdk::Image img;
  img.width = 2;
  img.height = 2;
  img.encoding = std::string(encoding);
  img.frame_id = "cam";
  img.timestamp_ns = ts;
  img.data = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(kPixels.data()), kPixels.size() * 4U);
  return PJ::sdk::ObjectRecord{.ts = ts, .object = img};
}
PJ::Expected<PJ::sdk::ObjectRecord> emitDepth(PJ::Timestamp ts, PJ::sdk::PayloadView /*p*/) {
  return emitImageWithEncoding(ts, "32FC1");
}
PJ::Expected<PJ::sdk::ObjectRecord> emitColor(PJ::Timestamp ts, PJ::sdk::PayloadView /*p*/) {
  return emitImageWithEncoding(ts, "rgb8");
}

PJ::ObjectTopicId registerImageTopic(PJ::ObjectStore& store, const std::string& name) {
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = 1;
  desc.topic_name = name;
  desc.metadata_json = R"({"builtin_object_type":"kImage"})";
  const auto id = store.registerTopic(desc);
  EXPECT_TRUE(id.has_value());
  EXPECT_TRUE(store.pushOwned(id.value(), 100, std::vector<uint8_t>{0x01}).has_value());
  return id.has_value() ? id.value() : PJ::ObjectTopicId{};
}

TEST(Scene3DDockDepthGate, AcceptsDepthEncodedImageAsDepthCloud) {
  PJ::SessionManager session;
  PJ::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();

  const PJ::ObjectTopicId depth = registerImageTopic(store, "/cam/depth/image");
  session.registerObjectTopicParser(depth, makeBoundHandle(kImageSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kImageSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr, &emitDepth);
                                    }));

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);

  EXPECT_TRUE(PJ::Scene3DDockWidget::handlesObjectType(PJ::sdk::BuiltinObjectType::kImage));
  EXPECT_TRUE(dock.canAcceptObjectTopic(depth, PJ::sdk::BuiltinObjectType::kImage));
  EXPECT_TRUE(dock.layers().empty()) << "hover must not create a layer";
  ASSERT_TRUE(dock.addTopic(depth, PJ::sdk::BuiltinObjectType::kImage, u"depth"_s));
  EXPECT_EQ(dock.layers().size(), 1U) << "a depth image must create a DepthCloud layer";
}

TEST(Scene3DDockDepthGate, RefusesColorImage) {
  PJ::SessionManager session;
  PJ::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();

  const PJ::ObjectTopicId color = registerImageTopic(store, "/cam/color/image");
  session.registerObjectTopicParser(color, makeBoundHandle(kImageSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kImageSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr, &emitColor);
                                    }));

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);

  EXPECT_FALSE(dock.canAcceptObjectTopic(color, PJ::sdk::BuiltinObjectType::kImage));
  EXPECT_FALSE(dock.addTopic(color, PJ::sdk::BuiltinObjectType::kImage, u"color"_s));
  EXPECT_TRUE(dock.layers().empty()) << "a color image must NOT become a 3D layer";
}

}  // namespace
