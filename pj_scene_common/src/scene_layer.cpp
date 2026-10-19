// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene_common/scene_layer.h"

#include <algorithm>
#include <limits>
#include <string>

#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace PJ {

PJ::Range<PJ::Timepoint> liveTopicTimeRange(const ObjectStore* store, ObjectTopicId topic_id) {
  if (store == nullptr || store->entryCount(topic_id) == 0) {
    return {PJ::Timepoint::max(), PJ::Timepoint::min()};  // inverted: empty
  }
  const auto [lo, hi] = store->timeRange(topic_id);
  return {PJ::fromRaw(lo), PJ::fromRaw(hi)};
}

uint64_t latestEntryRenderKey(const ObjectStore* store, ObjectTopicId topic_id, PJ::Timepoint time) {
  const auto entry = store != nullptr ? store->latestEntryIdAt(topic_id, PJ::toRaw(time)) : std::nullopt;
  return entry.has_value() ? entry->uid.value : kNoSampleRenderKey;
}

bool SceneWarningParts::set(Part part, QString text) {
  QString& slot = parts_[static_cast<std::size_t>(part)];
  if (slot == text) {
    return false;
  }
  slot = std::move(text);
  QString joined;
  for (const QString& each : parts_) {
    if (!each.isEmpty()) {
      joined += (joined.isEmpty() ? QString() : QStringLiteral("\n")) + each;
    }
  }
  if (joined == combined_) {
    return false;
  }
  combined_ = std::move(joined);
  return true;
}

namespace {

/// Seconds as a row reads them: "0.4", "15", "125".
QString secondsText(double seconds) {
  return seconds >= 100.0 ? QString::number(seconds, 'f', 0) : QString::number(seconds, 'g', 3);
}

}  // namespace

DerivedTopicPresentation derivedTopicPresentation(SessionManager* session, ObjectTopicId topic_id) {
  if (session == nullptr) {
    return {};
  }
  OnDemandService& service = session->onDemandService();
  const std::optional<std::string> key = service.keyOfTopic(topic_id);
  if (!key) {
    return {};
  }
  using NodeStatus = NodeScheduler::NodeState::Status;
  const NodeScheduler::NodeState state = service.state(*key).value_or(NodeScheduler::NodeState{});
  DerivedTopicPresentation out;
  if (state.status == NodeStatus::kMissingInput) {
    out.warning = QString::fromStdString(state.detail);
  } else if (state.status == NodeStatus::kError) {
    out.warning = QObject::tr("error: %1").arg(QString::fromStdString(state.detail));
  }
  out.computing = state.status == NodeStatus::kPending;
  if (state.lag && *state.lag > 0) {
    out.lag = state.lag;
    out.status = QObject::tr("behind %1 s").arg(secondsText(timestampDifferenceSeconds(*state.lag, 0)));
  } else if (out.computing && !state.lag) {
    out.status = QObject::tr("computing…");
  }
  const RecipeGraph::Node* node = service.graph().find(*key);
  if (node == nullptr || !node->recipe.pinned_t) {
    return out;
  }
  const std::string& topic_name = session->objectStore().descriptor(topic_id).topic_name;
  const std::string output = topic_name.substr(std::min(topic_name.size(), key->size() + 1));
  // A finding's single result serves every instant at or after its real stamp: ask at the end of time.
  const auto shown = service.history().resultAt(*key, output, std::numeric_limits<Timestamp>::max());
  if (shown && !shown->unavailable) {
    out.finding_stamp = shown->stamp;
  }
  if (out.status.isEmpty() && out.warning.isEmpty()) {
    const double at = rawToDisplaySeconds(*node->recipe.pinned_t, session->displayOffset(node->dataset)).value;
    out.status = QObject::tr("computed at %1 s").arg(secondsText(at));
  }
  return out;
}

ISceneLayer::ISceneLayer(QObject* parent) : QObject(parent) {}

void ISceneLayer::setWarningPart(SceneWarningParts::Part part, QString text) {
  if (warning_parts_.set(part, std::move(text))) {
    onWarningChanged();
  }
}

void ISceneLayer::onWarningChanged() {
  emit warningChanged(!warning().isEmpty(), warning());
}

ISceneLayer::~ISceneLayer() = default;

void ISceneLayer::setFixedFrame(const QString& /*frame*/) {}

QDomElement ISceneLayer::xmlSaveState(QDomDocument& /*doc*/) const {
  return {};
}

bool ISceneLayer::xmlLoadState(const QDomElement& /*element*/) {
  return true;
}

}  // namespace PJ
