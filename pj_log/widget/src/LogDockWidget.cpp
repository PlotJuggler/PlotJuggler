// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_log/LogDockWidget.h"

#include <QStringList>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

#include "pj_base/builtin/log.hpp"
#include "pj_datastore/log_entries.hpp"
#include "pj_log/LogView.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/XmlStateOptions.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

constexpr auto kPayloadTag = "log"_L1;
constexpr auto kTopicTag = "topic"_L1;
constexpr auto kHiddenTopicTag = "hidden_topic"_L1;

bool isLogTopic(const ObjectStore& objects, ObjectTopicId topic_id) {
  return objectTypeFromMetadata(objects.descriptor(topic_id).metadata_json) == sdk::BuiltinObjectType::kLog &&
         objects.logView(topic_id).has_value();
}

LogLevel levelOf(sdk::Log::Level level) {
  return level <= sdk::Log::Level::kFatal ? static_cast<LogLevel>(level) : LogLevel::kUnknown;
}

/// One topic's rows in the ObjectStore, keyed by their UIDs. Each read takes a LogSeriesView (a batch takes one for all
/// its reads), so the panel never holds the series lock across event-loop turns.
class StoreLogSource final : public LogRowSource {
 public:
  StoreLogSource(const ObjectStore& objects, ObjectTopicId id, DisplayOffset offset)
      : objects_(objects), id_(id), offset_(offset) {}

  [[nodiscard]] size_t size() const override {
    size_t size = 0;
    read([&size](const LogEntries& entries) { size = entries.size(); });
    return size;
  }
  [[nodiscard]] uint64_t key(size_t index) const override {
    uint64_t key = 0;
    read([&](const LogEntries& entries) { key = entries.uidAt(index).value; });
    return key;
  }
  [[nodiscard]] std::optional<size_t> find(uint64_t key, size_t hint) const override {
    std::optional<size_t> index;
    read([&](const LogEntries& entries) {
      index = hint < entries.size() && entries.uidAt(hint).value == key ? std::optional(hint)
                                                                        : entries.indexOfUid(SequentialUID{key});
    });
    return index;
  }
  [[nodiscard]] std::vector<size_t> indicesAfter(uint64_t after) const override {
    std::vector<size_t> indices;
    read([&](const LogEntries& entries) { indices = entries.indicesAfterUid(SequentialUID{after}); });
    return indices;
  }
  [[nodiscard]] double time(size_t index) const override {
    double time = 0.0;
    read([&](const LogEntries& entries) {
      time = toAxisDouble(rawToDisplaySecondsAtOrAfter(entries.timestamps()[index], offset_));
    });
    return time;
  }
  [[nodiscard]] LogLevel level(size_t index) const override {
    LogLevel level = LogLevel::kUnknown;
    read([&](const LogEntries& entries) { level = levelOf(entries.level(index)); });
    return level;
  }
  [[nodiscard]] uint32_t sourceId(size_t index) const override {
    uint32_t id = 0;
    read([&](const LogEntries& entries) { id = entries.sourceId(index); });
    return id;
  }
  [[nodiscard]] uint32_t categoryId(size_t index) const override {
    uint32_t id = 0;
    read([&](const LogEntries& entries) { id = entries.categoryId(index); });
    return id;
  }
  [[nodiscard]] int64_t ownStamp(size_t index) const override {
    int64_t stamp = 0;
    read([&](const LogEntries& entries) { stamp = entries.ownStamp(index); });
    return stamp;
  }
  [[nodiscard]] std::string dictionaryText(uint32_t id) const override {
    std::string text;
    read([&](const LogEntries& entries) { text = std::string(entries.dictionaryText(id)); });
    return text;
  }
  void messages(size_t first, size_t count, const MessageSink& sink) const override {
    read([&](const LogEntries& entries) { entries.messages(first, count, sink); });
  }
  void batch(const std::function<void()>& body) const override {
    if (in_batch_) {
      body();
      return;
    }
    auto view = objects_.logView(id_);
    pinned_ = view ? &**view : nullptr;
    in_batch_ = true;
    struct Unpin {
      const StoreLogSource* self;
      ~Unpin() {
        self->pinned_ = nullptr;
        self->in_batch_ = false;
      }
    } unpin{this};
    body();
  }

 private:
  template <typename F>
  void read(F&& f) const {
    if (in_batch_) {
      if (pinned_ != nullptr) {
        f(*pinned_);
      }
      return;
    }
    if (auto view = objects_.logView(id_)) {
      f(**view);
    }
  }

  const ObjectStore& objects_;
  ObjectTopicId id_;
  DisplayOffset offset_;
  mutable const LogEntries* pinned_ = nullptr;
  mutable bool in_batch_ = false;
};

struct ParsedLogState {
  LogViewState view;
  std::vector<SavedSeriesPath> topics;
};

// Tolerated: an empty or unparsable dataset_id reads as 0, an unqualified topic.
SavedSeriesPath readTopic(const QDomElement& child) {
  SavedSeriesPath saved;
  saved.topic = readXmlText(child, u"topic"_s);
  (void)readXmlInt<uint32_t>(
      child, u"dataset_id"_s, 0, 0, std::numeric_limits<uint32_t>::max(), saved.dataset_id, XmlBadValue::kTolerate);
  saved.dataset_source = readXmlText(child, u"dataset_source"_s);
  saved.dataset_path = readXmlText(child, u"dataset_path"_s);
  return saved;
}

const QStringList& levelKeys() {
  static const QStringList keys = [] {
    QStringList list;
    for (size_t i = 0; i < log_view::kToggleLevelCount; ++i) {
      list.append(QString::fromLatin1(log_view::kLevelNames[i].key));
    }
    return list;
  }();
  return keys;
}

// The one reader of a <log> payload, for xmlLoadState and xmlValidateState. Every attribute is read through a
// reporting reader, and under a recorder the children's too when the payload has none of that tag, so a state without
// topics still lists what a topic accepts. Tolerated, and noted as rejections: an unknown level key (ignored), a
// future_rows other than "grey" (it hides) and an unparsable dataset_id (0).
std::optional<ParsedLogState> parseLogState(const QDomElement& element) {
  if (element.tagName() != kPayloadTag) {
    XmlOptionsRecorder::noteRejection(element, {}, u"expected a <log> element"_s);
    return std::nullopt;
  }
  ParsedLogState parsed;
  LogViewState& state = parsed.view;

  const QStringList& level_keys = levelKeys();
  if (XmlOptionsRecorder::active()) {
    XmlOptionsRecorder::noteOption(
        element,
        {.attribute = u"levels"_s, .type = u"text"_s, .values = level_keys, .fallback = level_keys.join(u',')});
  }
  if (element.hasAttribute(u"levels"_s)) {
    const QStringList levels = element.attribute(u"levels"_s).split(u',', Qt::SkipEmptyParts);
    for (size_t i = 0; i < log_view::kToggleLevelCount; ++i) {
      state.levels[i] = levels.contains(level_keys[static_cast<qsizetype>(i)]);
    }
    const bool all_known =
        std::ranges::all_of(levels, [&level_keys](const QString& key) { return level_keys.contains(key); });
    if (!all_known) {
      XmlOptionsRecorder::noteRejection(
          element, u"levels"_s, u"expected a comma-separated list of "_s + level_keys.join(u", "_s));
    }
  }
  state.source = readXmlText(element, u"source_filter"_s);
  state.category = readXmlText(element, u"category_filter"_s);
  state.text = readXmlText(element, u"text_filter"_s);
  QString future_rows;
  (void)readXmlEnum(element, u"future_rows"_s, {u"grey"_s, u"hide"_s}, u"hide"_s, future_rows, XmlBadValue::kTolerate);
  state.show_future = future_rows == u"grey"_s;

  for (QDomElement hidden = element.firstChildElement(kHiddenTopicTag); !hidden.isNull();
       hidden = hidden.nextSiblingElement(kHiddenTopicTag)) {
    state.hidden_topics.append(readXmlText(hidden, u"topic"_s));
  }
  for (QDomElement child = element.firstChildElement(kTopicTag); !child.isNull();
       child = child.nextSiblingElement(kTopicTag)) {
    parsed.topics.push_back(readTopic(child));
  }

  if (XmlOptionsRecorder::active()) {
    QDomDocument schema;
    readXmlText(schema.createElement(kHiddenTopicTag), u"topic"_s);
    (void)readTopic(schema.createElement(kTopicTag));
  }
  return parsed;
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

  view_->setPlaying(playback_->isPlaying());
  connect(playback_, &PlaybackEngine::playingChanged, view_, &LogView::setPlaying);
  connect(view_, &LogView::seekRequested, playback_, [this](double time) {
    playback_->setCurrentTime(displaySeconds(time));
  });
  // A topic saved against a dataset that had not loaded yet binds the moment its items surface.
  connect(catalog_, &CatalogModel::itemsAdded, this, &LogDockWidget::retryPending);
  connect(catalog_, &CatalogModel::itemsChanged, this, &LogDockWidget::retryPending);
  connect(catalog_, &CatalogModel::itemsRemoved, this, [this]() { entries_landed_.request(); });
  connect(catalog_, &CatalogModel::cleared, this, [this]() { entries_landed_.request(); });
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
  connect(session_, &SessionManager::datasetSourcePathChanged, this, [this](DatasetId dataset_id) {
    for (Topic& topic : topics_) {
      if (topic.saved.dataset_id == dataset_id) {
        topic.saved = savedIdentity(dataset_id, topic.saved.topic);
      }
    }
    retryPending();
  });
}

LogDockWidget::~LogDockWidget() = default;

void LogDockWidget::onTrackerTime(double time) {
  view_->setTrackerTime(time);
}

void LogDockWidget::setTimeLabelFormat(TimeLabelFormat format) {
  view_->setTimeLabelFormat(format);
}

bool LogDockWidget::shows(ObjectTopicId topic_id) const {
  return std::ranges::any_of(topics_, [topic_id](const Topic& topic) { return topic.id == topic_id; });
}

bool LogDockWidget::canAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type) const {
  return object_type == sdk::BuiltinObjectType::kLog && !shows(topic_id);
}

bool LogDockWidget::tryAcceptObjectTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& /*title*/) {
  if (!canAcceptObjectTopic(topic_id, object_type) || !addTopic(topic_id)) {
    return false;
  }
  emit topicsChanged();
  emit workspaceChanged();
  return true;
}

SavedSeriesPath LogDockWidget::savedIdentity(DatasetId dataset_id, const QString& topic_name) const {
  return SavedSeriesPath{
      .topic = topic_name,
      .field = {},
      .dataset_id = dataset_id,
      .dataset_source = catalog_->datasetSourceName(dataset_id).value_or(QString()),
      .dataset_path = catalog_->datasetSourcePath(dataset_id),
  };
}

bool LogDockWidget::addTopic(ObjectTopicId topic_id) {
  const ObjectStore& objects = session_->objectStore();
  if (shows(topic_id) || !isLogTopic(objects, topic_id)) {
    return false;
  }
  const ObjectTopicDescriptor descriptor = objects.descriptor(topic_id);
  Topic& topic = topics_.emplace_back(
      Topic{
          .id = topic_id,
          .saved = savedIdentity(descriptor.dataset_id, QString::fromStdString(descriptor.topic_name)),
          .identity = {}});
  view_->setTopicLabel(topic_id.id, topic.saved.topic);
  loadTopic(topic, /*full=*/true);
  return true;
}

bool LogDockWidget::loadTopic(Topic& topic, bool full) {
  const ObjectStore& objects = session_->objectStore();
  OrderedEntries::ContentIdentity identity;
  if (auto view = objects.logView(topic.id)) {
    identity = (**view).contentIdentity();
  } else {
    return false;  // no longer stored as a log
  }
  if (!full && identity == topic.identity) {
    return true;
  }
  // A removal, a shift or a re-UID (a merge, a reload) moves rows the update cannot follow: index the topic anew.
  full = full || identity.removal != topic.identity.removal;
  auto source = std::make_shared<StoreLogSource>(objects, topic.id, session_->displayOffset(topic.saved.dataset_id));
  if (full) {
    view_->setTopicRows(topic.id.id, std::move(source));
  } else {
    view_->updateTopicRows(topic.id.id, std::move(source));
  }
  // Taken before the rows were read: a change in between shows as a new identity next update.
  topic.identity = identity;
  return true;
}

void LogDockWidget::refresh() {
  const bool full = std::exchange(reload_all_, false);
  const ObjectStore& objects = session_->objectStore();
  bool changed = false;
  for (auto it = topics_.begin(); it != topics_.end();) {
    if (objects.isTopic(it->id, it->saved.dataset_id, it->saved.topic.toStdString()) && loadTopic(*it, full)) {
      ++it;
      continue;
    }
    // Removed, reparented by a merge or reissued: wait for it by the identity it was shown with, as a restore does;
    // its dataset may be gone already.
    if (std::ranges::find(pending_, it->saved) == pending_.end()) {
      pending_.push_back(it->saved);
    }
    view_->removeTopic(it->id.id);
    it = topics_.erase(it);
    changed = true;
  }
  if (changed) {
    retryPending();
    emit topicsChanged();
  }
}

void LogDockWidget::addPendingTopic(DatasetId dataset_id, const QString& topic_name) {
  SavedSeriesPath pending = savedIdentity(dataset_id, topic_name);
  if (tryResolve(pending)) {
    emit topicsChanged();
  } else if (std::ranges::find(pending_, pending) == pending_.end()) {
    pending_.push_back(std::move(pending));
    emit topicsChanged();
  }
}

void LogDockWidget::discardTopic(DatasetId dataset_id, const QString& topic_name) {
  discardIf([dataset_id, &topic_name](const SavedSeriesPath& saved) {
    return saved.dataset_id == dataset_id && saved.topic == topic_name;
  });
}

void LogDockWidget::discardDataset(DatasetId dataset_id) {
  discardIf([dataset_id](const SavedSeriesPath& saved) { return saved.dataset_id == dataset_id; });
}

void LogDockWidget::followMerge(DatasetId anchor, const QList<DatasetId>& consumed) {
  for (Topic& topic : topics_) {
    if (consumed.contains(topic.saved.dataset_id)) {
      topic.saved = savedIdentity(anchor, topic.saved.topic);
    }
  }
  for (SavedSeriesPath& pending : pending_) {
    if (consumed.contains(pending.dataset_id)) {
      pending = savedIdentity(anchor, pending.topic);
    }
  }
  // A moved topic is shown again; one folded into the anchor's topic of its name goes pending and binds to that.
  refresh();
  retryPending();
}

void LogDockWidget::discardIf(const std::function<bool(const SavedSeriesPath&)>& matches) {
  bool changed = std::erase_if(pending_, matches) > 0;
  for (auto it = topics_.begin(); it != topics_.end();) {
    if (!matches(it->saved)) {
      ++it;
      continue;
    }
    view_->removeTopic(it->id.id);
    it = topics_.erase(it);
    changed = true;
  }
  if (changed) {
    emit topicsChanged();
  }
}

std::optional<DatasetId> LogDockWidget::resolveDataset(const SavedSeriesPath& saved) const {
  return session_->resolveObjectDatasetIdentity(saved.dataset_id, saved.dataset_source, saved.dataset_path, saved.topic)
      .id;
}

bool LogDockWidget::tryResolve(const SavedSeriesPath& pending) {
  if (pending.topic.isEmpty()) {
    return false;
  }
  std::optional<ObjectTopicId> topic_id;
  if (pending.dataset_id == 0 && pending.dataset_source.isEmpty() && pending.dataset_path.isEmpty()) {
    // No dataset qualifier: the one dataset holding a topic of that name.
    const std::vector<CatalogItem> named = catalog_->objectTopicsNamed(pending.topic, {});
    if (named.size() == 1) {
      topic_id = asObjectTopic(named.front())->object_topic_id;
    }
  } else if (const std::optional<DatasetId> dataset = resolveDataset(pending)) {
    topic_id = session_->objectStore().findTopic(*dataset, pending.topic.toStdString());
  }
  // A topic shown already is resolved: waiting for it would hold its demand and save it twice.
  return topic_id.has_value() && (shows(*topic_id) || addTopic(*topic_id));
}

void LogDockWidget::retryPending() {
  // No signal while pending_ is being erased from: a slot reading it would see it half done.
  if (std::erase_if(pending_, [this](const SavedSeriesPath& pending) { return tryResolve(pending); }) > 0) {
    emit topicsChanged();
  }
}

void LogDockWidget::finishRestore() {
  retryPending();
}

std::vector<std::pair<DatasetId, QString>> LogDockWidget::displayedTopics() const {
  std::vector<std::pair<DatasetId, QString>> out;
  for (const Topic& topic : topics_) {
    out.emplace_back(topic.saved.dataset_id, topic.saved.topic);
  }
  return out;
}

std::vector<std::pair<DatasetId, ObjectTopicId>> LogDockWidget::displayedObjectTopics() const {
  std::vector<std::pair<DatasetId, ObjectTopicId>> out;
  for (const Topic& topic : topics_) {
    out.emplace_back(topic.saved.dataset_id, topic.id);
  }
  return out;
}

std::vector<LogDockWidget::PendingDemand> LogDockWidget::pendingDemands() const {
  std::vector<PendingDemand> out;
  for (const SavedSeriesPath& pending : pending_) {
    // The dataset tryResolve() would bind to, so the demand starts the source the topic will come from.
    PendingDemand demand{pending.topic, resolveDataset(pending)};
    if (std::ranges::find(out, demand) == out.end()) {
      out.push_back(std::move(demand));
    }
  }
  return out;
}

bool LogDockWidget::hasPendingTopic(const QString& topic_name) const {
  return std::ranges::any_of(
      pending_, [&topic_name](const SavedSeriesPath& pending) { return pending.topic == topic_name; });
}

QStringList LogDockWidget::unresolvedRestores() const {
  QStringList topics;
  for (const SavedSeriesPath& pending : pending_) {
    topics.append(pending.topic);
  }
  return topics;
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

  const auto append_topic = [&doc, &root](const SavedSeriesPath& saved) {
    QDomElement topic = doc.createElement(kTopicTag);
    topic.setAttribute(u"dataset_id"_s, QString::number(saved.dataset_id));
    topic.setAttribute(u"dataset_source"_s, saved.dataset_source);
    topic.setAttribute(u"dataset_path"_s, saved.dataset_path);
    topic.setAttribute(u"topic"_s, saved.topic);
    root.appendChild(topic);
  };
  for (const Topic& topic : topics_) {
    append_topic(topic.saved);
  }
  // Unresolved topics ride along verbatim: a save without their dataset loaded must not forget them.
  for (const SavedSeriesPath& pending : pending_) {
    append_topic(pending);
  }
  for (const QString& hidden : state.hidden_topics) {
    QDomElement element = doc.createElement(kHiddenTopicTag);
    element.setAttribute(u"topic"_s, hidden);
    root.appendChild(element);
  }
  return root;
}

bool LogDockWidget::xmlValidateState(const QDomElement& element) const {
  return parseLogState(element).has_value();
}

bool LogDockWidget::xmlLoadState(const QDomElement& element) {
  const std::optional<ParsedLogState> parsed = parseLogState(element);
  if (!parsed.has_value()) {
    return false;
  }
  view_->applyState(parsed->view);

  for (const Topic& topic : topics_) {
    view_->removeTopic(topic.id.id);
  }
  topics_.clear();
  pending_.clear();
  for (const SavedSeriesPath& saved : parsed->topics) {
    if (!tryResolve(saved) && std::ranges::find(pending_, saved) == pending_.end()) {
      pending_.push_back(saved);
    }
  }
  emit topicsChanged();
  return true;
}

}  // namespace PJ
