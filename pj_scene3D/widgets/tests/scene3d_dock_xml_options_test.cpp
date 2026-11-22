// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Scene3DDockWidget::xmlValidateState is self-describing: validating the dock's own saved state under a
// PJ::XmlOptionsRecorder reports every attribute the state carries, every reported token validates, and every reported
// bound is enforced with a rejection naming the attribute. The dock holds one layer of each type plus the standalone
// and the pose-owned trail, a local robot model and the TF config topic.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_base/dataset.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TransformService.h"
#include "pj_runtime/XmlStateOptions.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/layers/poses_in_frame_layer.h"
#include "pj_scene3d_widgets/layers/trail_layer.h"

using namespace Qt::StringLiterals;
using pj::scene3d::test::CountingObjectParser;
using pj::scene3d::test::makeBoundHandle;
using BT = PJ::sdk::BuiltinObjectType;

namespace {

constexpr std::string_view kImageSchema = "mock/depth_image";
const std::array<float, 4> kDepthPixels = {1.0f, 2.0f, 3.0f, 4.0f};

PJ::Expected<PJ::sdk::ObjectRecord> emitDepthImage(PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) {
  PJ::sdk::Image image;
  image.width = 2;
  image.height = 2;
  image.encoding = "32FC1";
  image.frame_id = "cam";
  image.timestamp_ns = ts;
  image.data = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(kDepthPixels.data()), kDepthPixels.size() * 4U);
  return PJ::sdk::ObjectRecord{.ts = ts, .object = image};
}

// An element of `root` (itself included) with tag `tag` carrying `attribute`, in document order.
std::vector<QDomElement> elementsWith(const QDomElement& root, const QString& tag, const QString& attribute) {
  std::vector<QDomElement> found;
  const auto visit = [&](const auto& self, const QDomElement& element) -> void {
    if (element.tagName() == tag && element.hasAttribute(attribute)) {
      found.push_back(element);
    }
    for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
      self(self, child);
    }
  };
  visit(visit, root);
  return found;
}

class Scene3DDockXmlOptionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto dataset =
        session_.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "options.dat", .time_domain_id = 0});
    ASSERT_TRUE(dataset.has_value());
    dataset_id_ = *dataset;

    const PJ::ObjectTopicId tf = registerTopic("/tf", "kFrameTransforms");
    ASSERT_TRUE(session_.objectStore().pushOwned(tf, 100, std::vector<uint8_t>{0x00}).has_value());
    dock_.setSessionManager(&session_);
    dock_.setTransformService(&transform_service_);
    ASSERT_TRUE(dock_.addTopic(tf, BT::kFrameTransforms, u"tf"_s));

    for (const auto& [topic, type, name] : {
             std::tuple{"/cloud", BT::kPointCloud, "kPointCloud"},
             std::tuple{"/map", BT::kOccupancyGrid, "kOccupancyGrid"},
             std::tuple{"/markers", BT::kSceneEntities, "kSceneEntities"},
             std::tuple{"/poses", BT::kPosesInFrame, "kPosesInFrame"},
             std::tuple{"/voxels", BT::kVoxelGrid, "kVoxelGrid"},
             std::tuple{"/grid_map", BT::kGridMap, "kGridMap"},
             std::tuple{"/mesh_map", BT::kMeshMap, "kMeshMap"},
         }) {
      ASSERT_TRUE(dock_.addTopic(registerTopic(topic, name), type, QString::fromLatin1(topic))) << topic;
    }
    const PJ::ObjectTopicId depth = registerTopic("/depth", "kImage");
    session_.registerObjectTopicParser(depth, makeBoundHandle(kImageSchema, []() noexcept -> void* {
                                         return new CountingObjectParser(
                                             kImageSchema, BT::kImage, nullptr, &emitDepthImage);
                                       }));
    ASSERT_TRUE(session_.objectStore().pushOwned(depth, 100, std::vector<uint8_t>{0x01}).has_value());
    ASSERT_TRUE(dock_.addTopic(depth, BT::kImage, u"depth"_s));

    ASSERT_NE(dock_.addRobotModelLayer().id, 0U);
    ASSERT_NE(dock_.addTrailLayer(pj::scene3d::TrailSource::tfFrame(u"base_link"_s)).id, 0U);
    for (const PJ::SceneLayerInfo& info : dock_.layers()) {
      if (auto* poses = dynamic_cast<pj::scene3d::PosesInFrameLayer*>(dock_.layerFor(info.topic_id))) {
        poses->setTrailEnabled(true);
        ASSERT_NE(dock_.addTrailLayer(pj::scene3d::TrailSource::poseTopic(info.topic_id)).id, 0U);
      }
    }

    // A save/replay realizes the lazy view, so the camera and <scene_controls> attributes are saved too.
    QDomDocument seed_doc;
    ASSERT_TRUE(dock_.xmlLoadState(dock_.xmlSaveState(seed_doc)));
    saved_ = dock_.xmlSaveState(doc_);
    doc_.appendChild(saved_);
  }

  PJ::ObjectTopicId registerTopic(const char* topic_name, const char* type_name) {
    PJ::ObjectTopicDescriptor desc;
    desc.dataset_id = dataset_id_;
    desc.topic_name = topic_name;
    desc.metadata_json = std::string(R"({"builtin_object_type":")") + type_name + "\"}";
    const auto topic = session_.objectStore().registerTopic(desc);
    EXPECT_TRUE(topic.has_value());
    return topic.has_value() ? *topic : PJ::ObjectTopicId{};
  }

  std::vector<PJ::XmlOption> savedOptions() {
    PJ::XmlOptionsRecorder recorder;
    EXPECT_TRUE(dock_.xmlValidateState(saved_));
    EXPECT_EQ(recorder.firstRejection(), QString{}) << "widget_state refuses a state with any noted rejection";
    return recorder.options();
  }

  // Validates a copy of the saved state with `attribute` of `target` (an element of saved_) set to `value`: accepted
  // as widget_state accepts it, valid with no rejection noted by a tolerant reader.
  bool acceptsWith(const QDomElement& target, const QString& attribute, const QString& value, QString* why) {
    QDomElement original = target;
    const bool had = original.hasAttribute(attribute);
    const QString before = original.attribute(attribute);
    original.setAttribute(attribute, value);
    QDomDocument copy;
    copy.appendChild(copy.importNode(saved_, /*deep=*/true));
    if (had) {
      original.setAttribute(attribute, before);
    } else {
      original.removeAttribute(attribute);
    }
    PJ::XmlOptionsRecorder recorder;
    const bool valid = dock_.xmlValidateState(copy.documentElement());
    *why = recorder.firstRejection();
    return valid && why->isEmpty();
  }

  PJ::SessionManager session_;
  PJ::TransformService transform_service_{session_};
  PJ::Scene3DDockWidget dock_;
  PJ::DatasetId dataset_id_ = 0;
  QDomDocument doc_;
  QDomElement saved_;
};

TEST_F(Scene3DDockXmlOptionsTest, SavedStateValidatesAndReportsEveryAttribute) {
  for (const char* tag :
       {"scene_controls", "config_topic", "pointcloud", "occupancy_grid", "markers", "poses_in_frame", "voxel_grid",
        "grid_map", "mesh_map", "depthcloud", "robot_model", "trail"}) {
    EXPECT_FALSE(saved_.elementsByTagName(QString::fromLatin1(tag)).isEmpty())
        << "the fixture saves no <" << tag << ">";
  }
  EXPECT_EQ(saved_.elementsByTagName(u"trail"_s).size(), 3) << "two standalone trails and the pose-owned one";

  // Tokens that pick a restore path or must agree with another attribute: substituting one alone is refused by design.
  const PJ::XmlSelfDescriptionRules rules{
      .skip_tokens = {
          u"layer.object_type"_s, u"layer.role"_s, u"layer.local"_s, u"trail.source_kind"_s, u"pointcloud.color_type"_s,
          u"pointcloud.color_choice_explicit"_s}};
  EXPECT_EQ(
      PJ::xmlSelfDescriptionProblems([this](const QDomElement& e) { return dock_.xmlValidateState(e); }, saved_, rules),
      QStringList{});
}

TEST_F(Scene3DDockXmlOptionsTest, LayerWaitingForItsTopicIsNotAFailure) {
  QDomElement deferred = doc_.createElement(u"layer"_s);
  deferred.setAttribute(u"dataset_source"_s, u"not_loaded.dat"_s);
  deferred.setAttribute(u"topic_name"_s, u"/not_loaded"_s);
  deferred.setAttribute(u"object_type"_s, u"kPointCloud"_s);
  deferred.appendChild(doc_.createElement(u"pointcloud"_s));
  saved_.appendChild(deferred);
  PJ::XmlOptionsRecorder recorder;
  EXPECT_TRUE(dock_.xmlValidateState(saved_)) << recorder.firstRejection().toStdString();
}

TEST_F(Scene3DDockXmlOptionsTest, ToleratedBadTokenStaysValidButIsNoted) {
  // The loader reads any role other than "trail" as a plain layer; a strict caller still learns the value is wrong.
  const QDomElement layer = saved_.firstChildElement(u"layer"_s);
  QDomElement edited = layer;
  edited.setAttribute(u"role"_s, u"trial"_s);
  PJ::XmlOptionsRecorder recorder;
  EXPECT_TRUE(dock_.xmlValidateState(saved_));
  EXPECT_TRUE(recorder.firstRejection().contains(u" role=\"trial\""_s)) << recorder.firstRejection().toStdString();
}

TEST_F(Scene3DDockXmlOptionsTest, EveryReportedBoundIsEnforcedNamingTheAttribute) {
  int checked = 0;
  for (const PJ::XmlOption& option : savedOptions()) {
    if ((option.type != u"int"_s && option.type != u"float"_s) || !option.max.has_value()) {
      continue;
    }
    const double span = option.min.has_value() ? *option.max - *option.min : 1.0;
    const double beyond = *option.max + std::max(1.0, span);
    const QString text = option.type == u"int"_s ? QString::number(beyond, 'f', 0) : QString::number(beyond, 'g', 9);
    // Only what the saved state carries can be substituted, and some elements carry an attribute their path ignores (a
    // local layer's dataset_id, a pose-owned trail's source): one refusal suffices.
    const std::vector<QDomElement> targets = elementsWith(saved_, option.element, option.attribute);
    if (targets.empty()) {
      continue;
    }
    bool refused = false;
    QString why;
    for (const QDomElement& target : targets) {
      if (!acceptsWith(target, option.attribute, text, &why)) {
        refused = true;
        break;
      }
    }
    EXPECT_TRUE(refused) << option.element.toStdString() << '.' << option.attribute.toStdString() << '='
                         << text.toStdString();
    EXPECT_TRUE(why.contains(u' ' + option.attribute + u"=\"")) << why.toStdString();
    ++checked;
  }
  EXPECT_GT(checked, 20);
}

TEST_F(Scene3DDockXmlOptionsTest, TopicLayerDatasetIdIsPositive) {
  const std::vector<PJ::XmlOption> options = savedOptions();
  const auto dataset_id = std::find_if(options.begin(), options.end(), [](const PJ::XmlOption& option) {
    return option.element == u"layer"_s && option.attribute == u"dataset_id"_s;
  });
  ASSERT_NE(dataset_id, options.end());
  EXPECT_EQ(dataset_id->min, std::optional<double>(1.0)) << "the first saved <layer> is a topic layer";

  const QDomElement topic_layer = saved_.firstChildElement(u"layer"_s);
  ASSERT_FALSE(topic_layer.hasAttribute(u"local"_s));
  ASSERT_FALSE(topic_layer.hasAttribute(u"role"_s));
  QString why;
  EXPECT_FALSE(acceptsWith(topic_layer, u"dataset_id"_s, u"0"_s, &why));
  EXPECT_TRUE(why.contains(u" dataset_id=\"0\""_s)) << why.toStdString();
}

TEST_F(Scene3DDockXmlOptionsTest, FloatAtItsBoundAsQtWritesItValidates) {
  // QDomElement::setAttribute writes a float with 8 significant digits and a double with 17, so a float saved at a
  // bound like 0.01f reads back just below the decimal 0.01.
  const QDomElement poses = saved_.elementsByTagName(u"poses_in_frame"_s).item(0).toElement();
  const QDomElement controls = saved_.firstChildElement(u"scene_controls"_s);
  ASSERT_FALSE(poses.isNull());
  ASSERT_FALSE(controls.isNull());
  QDomDocument scratch;
  QDomElement written = scratch.createElement(u"w"_s);
  written.setAttribute(u"double"_s, static_cast<double>(0.01f));
  written.setAttribute(u"float"_s, 0.01f);
  QString why;
  EXPECT_TRUE(acceptsWith(poses, u"gizmo_size"_s, written.attribute(u"double"_s), &why)) << why.toStdString();
  EXPECT_TRUE(acceptsWith(controls, u"grid_extent_m"_s, written.attribute(u"float"_s), &why)) << why.toStdString();
}

}  // namespace
