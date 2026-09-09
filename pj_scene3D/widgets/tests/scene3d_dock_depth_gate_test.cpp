// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Dock-level "droppable into 3D" gate. Scene3DDockWidget::addTopic accepts a
// DEPTH-encoded sdk::Image (kImage) and builds a DepthCloud layer, but refuses a
// COLOR image that shares the kImage type. This is the integration point behind
// drag-drop reachability: a depth topic absorbed into an existing 3D dock becomes
// a layer, while a color image dropped on a 3D view is rejected (so the host's
// "committed object dock refused the topic" path fires instead of a broken layer).
//
// The dock is exercised through its public IObjectViewer surface against a real
// SessionManager/ObjectStore. The GL view is created lazily (a queued singleShot
// in the base ctor) and never realized here, so no OpenGL context is required —
// addTopic only touches the view behind a `view_ != nullptr` guard, and building
// a DepthCloudLayer is pure CPU until initializeGL().

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
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/transform_service.h"
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
  pj::scene3d::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();

  const PJ::ObjectTopicId depth = registerImageTopic(store, "/cam/depth/image");
  session.registerObjectTopicParser(depth, makeBoundHandle(kImageSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kImageSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr, &emitDepth);
                                    }));

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);

  // kImage is handled at the type level; the dock then peeks the encoding.
  EXPECT_TRUE(PJ::Scene3DDockWidget::handlesObjectType(PJ::sdk::BuiltinObjectType::kImage));
  ASSERT_TRUE(dock.addTopic(depth, PJ::sdk::BuiltinObjectType::kImage, u"depth"_s));
  EXPECT_EQ(dock.layers().size(), 1U) << "a depth image must create a DepthCloud layer";
}

TEST(Scene3DDockDepthGate, RefusesColorImage) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();

  const PJ::ObjectTopicId color = registerImageTopic(store, "/cam/color/image");
  session.registerObjectTopicParser(color, makeBoundHandle(kImageSchema, []() noexcept -> void* {
                                      return new CountingObjectParser(
                                          kImageSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr, &emitColor);
                                    }));

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);

  // The encoding gate refuses a color image even though kImage is "handled".
  EXPECT_FALSE(dock.addTopic(color, PJ::sdk::BuiltinObjectType::kImage, u"color"_s));
  EXPECT_TRUE(dock.layers().empty()) << "a color image must NOT become a 3D layer";
}

}  // namespace
