// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/LogDockWidget.h"

#include <QStringList>
#include <QVBoxLayout>
#include <algorithm>

#include "pj_base/builtin/log.hpp"
#include "pj_runtime/CanonicalObjectPayload.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_widgets/LogView.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

constexpr auto kPayloadTag = "log"_L1;
constexpr auto kTopicTag = "topic"_L1;
constexpr auto kHiddenTopicTag = "hidden_topic"_L1;

bool isLogTopic(const ObjectStore& objects, ObjectTopicId topic_id) {
  return objectTypeFromMetadata(objects.descriptor(topic_id).metadata_json) == sdk::BuiltinObjectType::kLog;
}

LogLevel levelOf(sdk::Log::Level level) {
  return level <= sdk::Log::Level::kFatal ? static_cast<LogLevel>(level) : LogLevel::kUnknown;
}

}  // namespace

LogDockWidget::LogDockWidget(SessionManager* session, CatalogModel* catalog, PlaybackEngine* playback, QWidget* parent)
    : QWidget(parent),
      session_(session),
      catalog_(catalog),
      playback_(playback),
      entries_landed_(kEntriesLandedIntervalMs, [this]() { refresh(); }) {
  view_ = new LogView(this);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(view_);
  connect(view_, &LogView::stateEdited, this, &LogDockWidget::workspaceChanged);

  if (playback_ != nullptr) {
    view_->setPlaying(playback_->isPlaying());
    connect(playback_, &PlaybackEngine::playingChanged, view_, &LogView::setPlaying);
    connect(view_, &LogView::seekRequested, playback_, [this](double time) {
      playback_->setCurrentTime(displaySeconds(time));
    });
  }
  if (catalog_ != nullptr) {
    // A topic saved against a dataset that had not loaded yet binds the moment its items surface.
    connect(catalog_, &CatalogModel::itemsAdded, this, &LogDockWidget::retryPending);
    connect(catalog_, &CatalogModel::itemsChanged, this, &LogDockWidget::retryPending);
    connect(catalog_, &CatalogModel::itemsRemoved, this, [this]() { entries_landed_.request(); });
    connect(catalog_, &CatalogModel::cleared, this, [this]() { entries_landed_.request(); });
  }
  if (session_ != nullptr) {
    const auto on_offset_changed = [this]() {
      reload_all_ = true;
      entries_landed_.request();
    };
    connect(session_, qOverload<>(&SessionManager::displayOffsetChanged), this, on_offset_changed);
    connect(session_, qOverload<DatasetId>(&SessionManager::displayOffsetChanged), this, on_offset_changed);
    // dataPublished covers every ingest publication, object-only ones included; a load's last entries land with its
    // end, and derived objects announce themselves.
    const auto on_entries_landed = [this]() { entries_landed_.request(); };
    connect(session_, &SessionManager::dataPublished, this, on_entries_landed);
    connect(session_, &SessionManager::ingestEnded, this, on_entries_landed);
    connect(session_, &SessionManager::derivedEntriesPublished, this, on_entries_landed);
    // A topic names its dataset by source path, which an asynchronous file load registers only at commit and which
    // no catalog change reports.
    connect(session_, &SessionManager::datasetSourcePathChanged, this, &LogDockWidget::retryPending);
  }
}

LogDockWidget::~LogDockWidget() = default;

void LogDockWidget::onTrackerTime(double time) {
  view_->setTrackerTime(time);
}

void LogDockWidget::setTimeLabelFormat(TimeLabelFormat format) {
  view_->setTimeLabelFormat(format);
}

bool LogDockWidget::canAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type) const {
  return object_type == sdk::BuiltinObjectType::kLog &&
         std::ranges::none_of(topics_, [topic_id](const Topic& topic) { return topic.id == topic_id; });
}

bool LogDockWidget::tryAcceptObjectTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& /*title*/) {
  if (!canAcceptObjectTopic(topic_id, object_type) || !addTopic(topic_id)) {
    return false;
  }
  emit workspaceChanged();
  return true;
}

bool LogDockWidget::addTopic(ObjectTopicId topic_id) {
  if (session_ == nullptr || std::ranges::any_of(topics_, [topic_id](const Topic& t) { return t.id == topic_id; })) {
    return false;
  }
  const ObjectStore& objects = session_->objectStore();
  if (!isLogTopic(objects, topic_id)) {
    return false;
  }
  const ObjectTopicDescriptor descriptor = objects.descriptor(topic_id);
  Topic& topic = topics_.emplace_back(
      Topic{.id = topic_id, .dataset = descriptor.dataset_id, .name = QString::fromStdString(descriptor.topic_name)});
  view_->setTopicLabel(topic_id.id, topic.name);
  loadTopic(topic, /*full=*/true);
  emit topicsChanged();
  return true;
}

void LogDockWidget::loadTopic(Topic& topic, bool full) {
  const ObjectStore& objects = session_->objectStore();
  // Read before the drain: a change racing in between shows up as a new identity on the next update.
  const OrderedEntries::ContentIdentity identity = objects.contentIdentity(topic.id);
  if (!full && identity == topic.identity) {
    return;
  }
  // Only an append at or after the newest stamp, or a front eviction, keeps `rewrite`: the cursor follows those.
  full = full || identity.rewrite != topic.identity.rewrite;
  topic.identity = identity;
  if (full) {
    topic.cursor = SequentialUID{};
  }
  std::vector<ResolvedObjectEntry> entries = objects.drainNewSince(topic.id, topic.cursor);
  // Arrival order; the view wants time order.
  std::ranges::stable_sort(entries, {}, &ResolvedObjectEntry::timestamp);
  const DisplayOffset offset = session_->displayOffset(topic.dataset);
  std::vector<LogEntry> rows;
  rows.reserve(entries.size());
  for (const ResolvedObjectEntry& entry : entries) {
    if (entry.fetch_failed) {
      continue;
    }
    const auto object = decodeStoredObject(*session_, topic.id, sdk::BuiltinObjectType::kLog, entry);
    const sdk::Log* log = object ? object->get<sdk::Log>() : nullptr;
    if (log == nullptr) {
      continue;  // an entry its parser rejects shows nothing
    }
    rows.push_back(
        LogEntry{
            .time = toAxisDouble(rawToDisplaySecondsAtOrAfter(entry.timestamp, offset)),
            .stamp_ns = log->timestamp_ns,
            .level = levelOf(log->level),
            .source = log->source,
            .category = log->category,
            .message = log->message,
        });
  }
  if (full) {
    view_->replaceEntries(topic.id.id, std::move(rows));
  } else {
    view_->appendEntries(topic.id.id, std::move(rows));
  }
}

void LogDockWidget::refresh() {
  if (session_ == nullptr) {
    return;
  }
  const bool full = std::exchange(reload_all_, false);
  const ObjectStore& objects = session_->objectStore();
  bool changed = false;
  for (auto it = topics_.begin(); it != topics_.end();) {
    if (objects.isTopic(it->id, it->dataset, it->name.toStdString())) {
      loadTopic(*it, full);
      ++it;
      continue;
    }
    // Removed, reparented by a merge or reissued: wait for it by identity, as a restore does.
    pending_.push_back(
        PendingTopic{
            .dataset_id = static_cast<qint64>(it->dataset),
            .dataset_source =
                catalog_ != nullptr ? catalog_->datasetSourceName(it->dataset).value_or(QString()) : QString(),
            .dataset_path = catalog_ != nullptr ? catalog_->datasetSourcePath(it->dataset) : QString(),
            .topic = it->name,
        });
    view_->removeTopic(it->id.id);
    it = topics_.erase(it);
    changed = true;
  }
  if (changed) {
    emit topicsChanged();
    retryPending();
  }
}

void LogDockWidget::addPendingTopic(DatasetId dataset_id, const QString& topic_name) {
  const PendingTopic pending{
      .dataset_id = static_cast<qint64>(dataset_id),
      .dataset_source = catalog_ != nullptr ? catalog_->datasetSourceName(dataset_id).value_or(QString()) : QString(),
      .dataset_path = catalog_ != nullptr ? catalog_->datasetSourcePath(dataset_id) : QString(),
      .topic = topic_name,
  };
  if (!tryResolve(pending)) {
    pending_.push_back(pending);
    emit topicsChanged();
  }
}

bool LogDockWidget::tryResolve(const PendingTopic& pending) {
  if (session_ == nullptr || pending.topic.isEmpty()) {
    return false;
  }
  const ObjectStore& objects = session_->objectStore();
  const std::string name = pending.topic.toStdString();
  std::optional<ObjectTopicId> topic_id;
  if (pending.dataset_id == 0 && pending.dataset_source.isEmpty() && pending.dataset_path.isEmpty()) {
    // No dataset qualifier: the one dataset holding a topic of that name.
    for (const ObjectTopicId candidate : objects.listTopics()) {
      if (objects.descriptor(candidate).topic_name == name) {
        if (topic_id.has_value()) {
          return false;  // ambiguous
        }
        topic_id = candidate;
      }
    }
  } else {
    const DatasetIdentityResolution dataset = session_->resolveObjectDatasetIdentity(
        static_cast<DatasetId>(pending.dataset_id), pending.dataset_source, pending.dataset_path, pending.topic);
    if (dataset.id) {
      topic_id = objects.findTopic(*dataset.id, name);
    }
  }
  return topic_id.has_value() && addTopic(*topic_id);
}

void LogDockWidget::retryPending() {
  const size_t before = pending_.size();
  std::erase_if(pending_, [this](const PendingTopic& pending) { return tryResolve(pending); });
  if (pending_.size() != before) {
    emit topicsChanged();
  }
}

void LogDockWidget::finishRestore() {
  retryPending();
}

std::vector<std::pair<DatasetId, QString>> LogDockWidget::displayedTopics() const {
  std::vector<std::pair<DatasetId, QString>> out;
  for (const Topic& topic : topics_) {
    out.emplace_back(topic.dataset, topic.name);
  }
  return out;
}

std::vector<std::pair<DatasetId, ObjectTopicId>> LogDockWidget::displayedObjectTopics() const {
  std::vector<std::pair<DatasetId, ObjectTopicId>> out;
  for (const Topic& topic : topics_) {
    out.emplace_back(topic.dataset, topic.id);
  }
  return out;
}

std::vector<LogDockWidget::PendingDemand> LogDockWidget::pendingDemands() const {
  std::vector<PendingDemand> out;
  for (const PendingTopic& pending : pending_) {
    PendingDemand demand{pending.topic, std::nullopt};
    if (catalog_ != nullptr) {
      const auto identity = catalog_->resolveDatasetIdentity(
          static_cast<DatasetId>(pending.dataset_id), pending.dataset_source, pending.dataset_path);
      if (!identity.ambiguous) {
        demand.preferred_dataset = identity.id;
      }
    }
    if (std::ranges::find(out, demand) == out.end()) {
      out.push_back(std::move(demand));
    }
  }
  return out;
}

bool LogDockWidget::hasPendingTopic(const QString& topic_name) const {
  return std::ranges::any_of(
      pending_, [&topic_name](const PendingTopic& pending) { return pending.topic == topic_name; });
}

QDomElement LogDockWidget::xmlSaveState(QDomDocument& doc) const {
  QDomElement root = doc.createElement(kPayloadTag);
  const LogViewState state = view_->state();
  QStringList levels;
  for (size_t i = 0; i < log_view::kToggleLevelCount; ++i) {
    if (state.levels[i]) {
      levels.append(QString::fromLatin1(log_view::kLevelNames[i].key));
    }
  }
  root.setAttribute(u"levels"_s, levels.join(u','));
  root.setAttribute(u"source_filter"_s, state.source);
  root.setAttribute(u"category_filter"_s, state.category);
  root.setAttribute(u"text_filter"_s, state.text);
  root.setAttribute(u"future_rows"_s, state.show_future ? u"grey"_s : u"hide"_s);

  const auto append_topic = [&doc, &root](
                                qint64 dataset_id, const QString& source, const QString& path, const QString& name) {
    QDomElement topic = doc.createElement(kTopicTag);
    topic.setAttribute(u"dataset_id"_s, QString::number(dataset_id));
    topic.setAttribute(u"dataset_source"_s, source);
    topic.setAttribute(u"dataset_path"_s, path);
    topic.setAttribute(u"topic"_s, name);
    root.appendChild(topic);
  };
  for (const Topic& topic : topics_) {
    append_topic(
        static_cast<qint64>(topic.dataset),
        catalog_ != nullptr ? catalog_->datasetSourceName(topic.dataset).value_or(QString()) : QString(),
        catalog_ != nullptr ? catalog_->datasetSourcePath(topic.dataset) : QString(), topic.name);
  }
  // Unresolved topics ride along verbatim: a save without their dataset loaded must not forget them.
  for (const PendingTopic& pending : pending_) {
    append_topic(pending.dataset_id, pending.dataset_source, pending.dataset_path, pending.topic);
  }
  for (const QString& hidden : state.hidden_topics) {
    QDomElement element = doc.createElement(kHiddenTopicTag);
    element.setAttribute(u"topic"_s, hidden);
    root.appendChild(element);
  }
  return root;
}

bool LogDockWidget::xmlLoadState(const QDomElement& element) {
  if (element.tagName() != kPayloadTag) {
    return false;
  }
  LogViewState state;
  if (element.hasAttribute(u"levels"_s)) {
    const QStringList levels = element.attribute(u"levels"_s).split(u',', Qt::SkipEmptyParts);
    for (size_t i = 0; i < log_view::kToggleLevelCount; ++i) {
      state.levels[i] = levels.contains(QString::fromLatin1(log_view::kLevelNames[i].key));
    }
  }
  state.source = element.attribute(u"source_filter"_s);
  state.category = element.attribute(u"category_filter"_s);
  state.text = element.attribute(u"text_filter"_s);
  state.show_future = element.attribute(u"future_rows"_s) != u"hide"_s;
  for (QDomElement hidden = element.firstChildElement(kHiddenTopicTag); !hidden.isNull();
       hidden = hidden.nextSiblingElement(kHiddenTopicTag)) {
    state.hidden_topics.append(hidden.attribute(u"topic"_s));
  }
  view_->applyState(state);

  for (const Topic& topic : topics_) {
    view_->removeTopic(topic.id.id);
  }
  topics_.clear();
  pending_.clear();
  for (QDomElement child = element.firstChildElement(kTopicTag); !child.isNull();
       child = child.nextSiblingElement(kTopicTag)) {
    const PendingTopic saved{
        .dataset_id = child.attribute(u"dataset_id"_s).toLongLong(),
        .dataset_source = child.attribute(u"dataset_source"_s),
        .dataset_path = child.attribute(u"dataset_path"_s),
        .topic = child.attribute(u"topic"_s),
    };
    if (!tryResolve(saved)) {
      pending_.push_back(saved);
    }
  }
  emit topicsChanged();
  return true;
}

}  // namespace PJ
