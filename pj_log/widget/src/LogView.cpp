// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_log/LogView.h"

#include <QAction>
#include <QClipboard>
#include <QEvent>
#include <QGuiApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringList>
#include <QStyle>
#include <QTextDocument>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <tuple>
#include <utility>

#include "pj_widgets/HeaderResizePolicy.h"
#include "pj_widgets/Scrollbar.h"
#include "ui_LogView.h"

namespace PJ {

using log_view::LogRow;
using log_view::LogTableModel;
using log_view::StoreChange;

namespace {

constexpr int kTextDebounceMs = 150;
const char* const kFutureShownIcon = ":/resources/svg/visibility.svg";
const char* const kFutureHiddenIcon = ":/resources/svg/visibility_off.svg";

QString formatStamp(int64_t ns) {
  const int64_t seconds = ns / 1'000'000'000;
  const int64_t fraction = std::llabs(ns % 1'000'000'000);
  const QString sign = (ns < 0 && seconds == 0) ? QStringLiteral("-") : QString();
  return QStringLiteral("%1%2.%3").arg(sign).arg(seconds).arg(fraction, 9, 10, QLatin1Char('0'));
}

/// Equal up to the host's conversion through nanoseconds: a nanosecond, or a few double steps at epoch
/// display times, where one step is ~238 ns. A relative tolerance would swallow real moves there.
bool sameTime(double a, double b) {
  constexpr double kNanosecond = std::chrono::duration<double>(std::chrono::nanoseconds(1)).count();
  const double steps = 4.0 * std::numeric_limits<double>::epsilon() * std::abs(a);
  return std::abs(a - b) <= std::max(kNanosecond, steps);
}

bool isNavigationKey(int key) {
  switch (key) {
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:
      return true;
    default:
      return false;
  }
}

}  // namespace

LogView::LogView(QWidget* parent)
    : QWidget(parent), ui_(std::make_unique<Ui::LogView>()), tracker_(std::numeric_limits<double>::quiet_NaN()) {
  ui_->setupUi(this);
  // Drops of a topic must reach the hosting dock, so nothing in the panel takes them.
  setAcceptDrops(false);
  ui_->logView->viewport()->setAcceptDrops(false);
  ui_->detailText->viewport()->setAcceptDrops(false);
  for (Search* edit : {ui_->sourceEdit, ui_->categoryEdit, ui_->filterEdit}) {
    edit->setVariant(Search::Variant::kStandalone);
    edit->lineEdit()->setAcceptDrops(false);
  }
  ui_->emptyLabel->setForegroundRole(QPalette::PlaceholderText);
  ui_->topicsButton->setMenu(new QMenu(ui_->topicsButton));
  ui_->topicsButton->hide();
  ui_->copyButton->setIconPath(QStringLiteral(":/resources/svg/copy.svg"));
  ui_->copyButton->setEnabled(false);
  ui_->splitter->setStretchFactor(0, 1);
  ui_->splitter->setStretchFactor(1, 0);
  ui_->detailText->setMinimumHeight(ui_->detailText->fontMetrics().lineSpacing() * 3);
  ui_->pages->setCurrentWidget(ui_->emptyPage);

  model_ = new LogTableModel(
      store_, [this](uint32_t wire) { return topics_.contains(wire) ? topics_.at(wire) : QString(); }, ui_->logView,
      this);
  ui_->logView->setModel(model_);
  QHeaderView* header = ui_->logView->header();
  header->setSectionsMovable(false);
  auto* policy = HeaderResizePolicy::install(header, LogTableModel::kMessage);
  policy->setSectionWidth(LogTableModel::kTime, 90);
  policy->setSectionWidth(LogTableModel::kLevel, 60);
  policy->setSectionWidth(LogTableModel::kSource, 140);
  policy->setSectionWidth(LogTableModel::kCategory, 110);
  policy->setSectionWidth(LogTableModel::kTopic, 140);
  ui_->logView->setColumnHidden(LogTableModel::kTopic, true);
  // QTreeView fits a double-clicked divider to the rows it shows: the Time column fits the rows after the playhead too.
  disconnect(header, &QHeaderView::sectionHandleDoubleClicked, ui_->logView, nullptr);
  connect(header, &QHeaderView::sectionHandleDoubleClicked, this, [this](int section) {
    if (section == LogTableModel::kTime) {
      fitTimeColumn(/*over_user=*/true);
    } else {
      ui_->logView->resizeColumnToContents(section);
    }
  });
  refreshOptionalColumns();
  setShowFuture(LogViewState{}.show_future);
  attachPillScrollbars(this);

  ui_->logView->installEventFilter(this);
  ui_->logView->viewport()->installEventFilter(this);
  ui_->logView->verticalScrollBar()->installEventFilter(this);

  for (QToolButton* toggle : levelToggles()) {
    connect(toggle, &QToolButton::toggled, this, &LogView::applyFilters);
    connect(toggle, &QToolButton::clicked, this, &LogView::stateEdited);
  }
  // Re-filtering walks every row: the text filters wait for typing to pause.
  text_timer_.setSingleShot(true);
  text_timer_.setInterval(kTextDebounceMs);
  connect(&text_timer_, &QTimer::timeout, this, &LogView::applyFilters);
  for (Search* edit : {ui_->sourceEdit, ui_->categoryEdit, ui_->filterEdit}) {
    connect(edit, &Search::textChanged, this, [this]() { text_timer_.start(); });
    connect(edit->lineEdit(), &QLineEdit::textEdited, this, &LogView::stateEdited);
  }
  connect(ui_->futureButton, &QToolButton::toggled, this, &LogView::setShowFuture);
  connect(ui_->futureButton, &QToolButton::clicked, this, &LogView::stateEdited);
  // A click only selects (the detail pane shows the entry); a double click seeks to it.
  connect(ui_->logView, &QAbstractItemView::doubleClicked, this, &LogView::onRowDoubleClicked);
  // The detail shows the current row when it is selected: follow both, since Qt moves the current row
  // before it selects it on a click, and a keyboard move can change one without the other.
  connect(ui_->logView->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &LogView::updateDetail);
  connect(ui_->logView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &LogView::onSelectionChanged);
  connect(ui_->logView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &LogView::updateDetail);
  connect(model_, &QAbstractItemModel::modelReset, this, &LogView::updateDetail);
  connect(model_, &LogTableModel::filterApplied, this, &LogView::onFilterApplied);
  connect(ui_->copyButton, &QToolButton::clicked, this, [this]() {
    QGuiApplication::clipboard()->setText(ui_->detailText->toPlainText());
  });
}

LogView::~LogView() {
  // The list reads store_ through the model, and store_ dies before ~QWidget closes a top-level view and lays the
  // list out again: the list goes first.
  delete ui_->logView;
}

void LogView::setTopicLabel(uint32_t topic, const QString& label) {
  if (topics_.contains(topic) && topics_.at(topic) == label) {
    return;
  }
  topics_[topic] = label;
  refreshTopics();
  ui_->logView->viewport()->update();  // the Topic column
}

void LogView::setTopicHidden(uint32_t topic, bool hidden) {
  if (hidden == topicHidden(topic)) {
    return;
  }
  if (hidden) {
    hidden_topics_.insert(topic);
  } else {
    hidden_topics_.erase(topic);
  }
  for (QAction* action : ui_->topicsButton->menu()->actions()) {
    if (action->data().toUInt() == topic) {
      const QSignalBlocker blocker(action);
      action->setChecked(!hidden);
    }
  }
  updateTopicsButton();
  applyFilters();
}

bool LogView::topicHidden(uint32_t topic) const {
  return hidden_topics_.contains(topic);
}

std::array<QToolButton*, log_view::kToggleLevelCount> LogView::levelToggles() const {
  // In log_view::kLevelNames order.
  return {ui_->levelDebug, ui_->levelInfo, ui_->levelWarn, ui_->levelError, ui_->levelFatal};
}

void LogView::changeStore(const std::function<StoreChange()>& mutate) {
  const std::optional<size_t> selected = selectedStoreRow();
  followChange(mutate(), selected);
}

void LogView::appendEntries(uint32_t topic, std::vector<LogEntry> entries) {
  if (entries.empty()) {
    return;
  }
  auto& source = entry_sources_[topic];
  const bool fresh = !source;
  if (fresh) {
    source = std::make_shared<VectorLogRowSource>();
  }
  source->merge(std::move(entries));
  // A new source has its own keys: what the topic showed goes.
  changeStore([&] { return fresh ? store_.replaceWire(topic, source) : store_.update(topic, source); });
}

void LogView::replaceEntries(uint32_t topic, std::vector<LogEntry> entries) {
  auto source = std::make_shared<VectorLogRowSource>();
  source->merge(std::move(entries));
  setTopicRows(topic, source);
  entry_sources_[topic] = std::move(source);
}

void LogView::setTopicRows(uint32_t topic, std::shared_ptr<const LogRowSource> source) {
  entry_sources_.erase(topic);
  // The rows under the viewport are rebuilt (a merge, a reload, a display-offset change): the scroll offset would point
  // at other rows, so the list finds the tracker again.
  shown_rows_ = false;
  changeStore([&] { return store_.replaceWire(topic, std::move(source)); });
}

void LogView::updateTopicRows(uint32_t topic, std::shared_ptr<const LogRowSource> source) {
  entry_sources_.erase(topic);
  changeStore([&] { return store_.update(topic, std::move(source)); });
}

void LogView::removeTopic(uint32_t topic) {
  topics_.erase(topic);
  entry_sources_.erase(topic);
  refreshTopics();
  if (chosen_ && chosen_->wire == topic) {
    chosen_.reset();  // a topic that takes the id later is not the user's choice
  }
  changeStore([&] { return store_.removeWire(topic); });
  setTopicHidden(topic, false);  // the model's filter still hides the id, which a later topic may take
}

void LogView::followChange(const StoreChange& change, std::optional<size_t> selected) {
  // Taken in the model's old rows: a list on the tracker stays on it however many rows land before it.
  const bool on_tracker = !playing_ && trackerRowInView();
  // The detail pane's row is stale from old_tail on, and an eviction shifts every index. Checked before the model
  // follows the store: its signals may refresh the pane, in new coordinates.
  if (detail_row_ && (change.reset || change.dropped_front > 0 || *detail_row_ >= change.old_tail)) {
    detail_valid_ = false;
  }
  {
    const QScopedValueRollback adjusting(adjusting_, true);
    model_->applyStoreChange(change);
    refreshOptionalColumns();
    model_->setTracker(tracker_);
    // A row the change left in place moves by the eviction alone; its rank among equal times may not hold.
    if (const std::optional<size_t> kept = selected ? change.kept(*selected) : std::nullopt) {
      chosen_ = store_.keyOf(*kept);
    }
    showChosen();
  }
  // Before the scroll: a wider column can bring the horizontal scrollbar, which takes height from the viewport.
  autoSizeColumns();
  if (playing_) {
    scrollToTracker(true);
  } else if ((!shown_rows_ || on_tracker) && model_->rowCount() > 0) {
    scrollToTracker(false);
  }
  if (model_->rowCount() > 0) {
    shown_rows_ = true;
  }
  updateDetail();
}

void LogView::setTrackerTime(double time) {
  const bool moved = std::isnan(tracker_) != std::isnan(time) || (!std::isnan(time) && !sameTime(time, tracker_));
  tracker_ = time;
  {
    const QScopedValueRollback adjusting(adjusting_, true);
    model_->setTracker(time);
    showChosen();
  }
  if (playing_) {
    pending_seek_.reset();
    scrollToTracker(true);
  } else if (moved) {
    // The tracker moved because of a double click in this list: that row stays where it is.
    const bool own_seek = pending_seek_.has_value() && sameTime(*pending_seek_, time);
    pending_seek_.reset();
    if (!own_seek) {
      scrollToTracker(false);
    }
  }
}

void LogView::setPlaying(bool playing) {
  if (playing == playing_) {
    return;
  }
  playing_ = playing;
  ui_->logView->verticalScrollBar()->setEnabled(!playing);
  ui_->logView->setAutoScroll(!playing);
  if (playing) {
    scrollToTracker(true);
  }
}

void LogView::setTimeLabelFormat(TimeLabelFormat format, int decimals) {
  // The host repeats its format on every Preferences OK: only a different one fits the column again.
  if (!model_->setTimeLabelFormat(format, decimals)) {
    return;
  }
  // The texts change width: fit the column again, even over a width the user set.
  fitTimeColumn(/*over_user=*/true);
  detail_valid_ = false;
  updateDetail();
}

void LogView::setShowFuture(bool show) {
  {
    const QSignalBlocker blocker(ui_->futureButton);
    ui_->futureButton->setChecked(show);
  }
  ui_->futureButton->setIconPath(QString::fromLatin1(show ? kFutureShownIcon : kFutureHiddenIcon));
  // Names what a click does, like the app's other toggles.
  ui_->futureButton->setToolTip(
      show ? tr("Hide the entries after the playhead") : tr("Show the entries after the playhead, greyed"));
  const QScopedValueRollback adjusting(adjusting_, true);
  model_->setFutureRows(show ? log_view::FutureRows::kGrey : log_view::FutureRows::kHide);
  showChosen();
}

log_view::LogFilter LogView::currentFilter() const {
  log_view::LogFilter filter;
  const LogViewState now = state();
  for (size_t i = 0; i < log_view::kToggleLevelCount; ++i) {
    filter.levels[static_cast<size_t>(log_view::kLevelNames[i].level)] = now.levels[i];
  }
  filter.source = now.source.trimmed().toStdString();
  filter.category = now.category.trimmed().toStdString();
  filter.text = now.text.trimmed().toStdString();
  filter.hidden_wires.assign(hidden_topics_.begin(), hidden_topics_.end());
  return filter;
}

void LogView::applyFilters() {
  text_timer_.stop();
  const QScopedValueRollback adjusting(adjusting_, true);
  model_->setFilter(currentFilter());
}

void LogView::onFilterApplied() {
  showChosen();
  if (playing_) {
    scrollToTracker(true);
  } else if (const QModelIndex current = ui_->logView->currentIndex(); current.isValid()) {
    ui_->logView->scrollTo(current, QAbstractItemView::EnsureVisible);
  } else {
    scrollToTracker(false);
  }
  shown_rows_ = shown_rows_ || model_->rowCount() > 0;
  updateDetail();
}

std::optional<size_t> LogView::selectedStoreRow() const {
  // The selection, not currentIndex(): a mouse drag selects a row before it makes it current.
  const QModelIndexList rows = ui_->logView->selectionModel()->selectedRows();
  if (rows.isEmpty()) {
    return std::nullopt;
  }
  return model_->storeIndex(rows.front().row());
}

bool LogView::trackerRowInView() const {
  // Vertical extent only: a list scrolled sideways still shows the row.
  const QRect rect = ui_->logView->visualRect(model_->index(model_->revealedRows() - 1, 0));
  return rect.isValid() && rect.bottom() >= 0 && rect.top() < ui_->logView->viewport()->height();
}

void LogView::onSelectionChanged() {
  if (adjusting_) {
    return;
  }
  const std::optional<size_t> selected = selectedStoreRow();
  chosen_ = selected ? std::optional(store_.keyOf(*selected)) : std::nullopt;
}

void LogView::showChosen() {
  const QScopedValueRollback adjusting(adjusting_, true);
  const std::optional<size_t> index = chosen_ ? store_.indexOf(*chosen_) : std::nullopt;
  if (const int row = index ? model_->rowOf(*index) : -1; row >= 0) {
    if (selectedStoreRow() != index) {
      selectRow(row);
    }
  } else if (chosen_ || ui_->logView->selectionModel()->hasSelection()) {
    ui_->logView->selectionModel()->clear();  // no neighbour in its place
  }
}

void LogView::selectRow(int row) {
  const bool auto_scroll = ui_->logView->hasAutoScroll();
  ui_->logView->setAutoScroll(false);  // re-selecting must not move the view by itself
  ui_->logView->selectionModel()->setCurrentIndex(
      model_->index(row, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  ui_->logView->setAutoScroll(auto_scroll);
}

void LogView::updateTopicsButton() {
  // A hidden topic keeps the menu that unhides it, the only one left too.
  ui_->topicsButton->setVisible(topics_.size() > 1 || !hidden_topics_.empty());
}

void LogView::refreshTopics() {
  QMenu* menu = ui_->topicsButton->menu();
  menu->clear();
  for (const auto& [id, label] : topics_) {
    QAction* action = menu->addAction(label);
    action->setCheckable(true);
    action->setChecked(!topicHidden(id));
    action->setData(id);
    connect(action, &QAction::toggled, this, [this, id](bool shown) { setTopicHidden(id, !shown); });
    connect(action, &QAction::triggered, this, &LogView::stateEdited);
  }
  const bool several = topics_.size() > 1;
  updateTopicsButton();
  ui_->logView->setColumnHidden(LogTableModel::kTopic, !several);
  ui_->pages->setCurrentWidget(topics_.empty() ? ui_->emptyPage : ui_->contentPage);
}

void LogView::refreshOptionalColumns() {
  // Many logs have no category, some no source: a column shows while a row fills it, its filter field too (or while a
  // restored filter needs the field).
  const std::array<std::tuple<int, Search*, bool>, 2> columns = {{
      {LogTableModel::kSource, ui_->sourceEdit, store_.hasSources()},
      {LogTableModel::kCategory, ui_->categoryEdit, store_.hasCategories()},
  }};
  for (const auto& [column, edit, filled] : columns) {
    edit->setVisible(filled || !edit->text().isEmpty());
    ui_->logView->setColumnHidden(column, !filled);
  }
}

void LogView::updateDetail() {
  const std::optional<size_t> selected = selectedStoreRow();
  if (detail_valid_ && selected == detail_row_) {
    return;
  }
  detail_valid_ = true;
  detail_row_ = selected;
  ui_->copyButton->setEnabled(selected.has_value());
  QString text;
  if (selected && *selected < store_.size()) {
    const LogRow row = store_.row(*selected);
    QStringList lines;
    lines << tr("Time: %1").arg(model_->timeText(row.time));
    lines << tr("Stamp: %1").arg(formatStamp(row.stamp_ns));
    lines << tr("Level: %1").arg(LogTableModel::levelName(row.level));
    lines << tr("Source: %1").arg(QString::fromStdString(store_.text(row.source)));
    lines << tr("Topic: %1").arg(topics_.contains(row.wire) ? topics_.at(row.wire) : QString());
    if (row.category != 0) {
      lines << tr("Category: %1").arg(QString::fromStdString(store_.text(row.category)));
    }
    text = lines.join(QLatin1Char('\n')) + QStringLiteral("\n\n") + QString::fromStdString(row.message);
  }
  if (text != detail_text_) {
    detail_text_ = text;
    ui_->detailText->setPlainText(text);
  }
}

void LogView::scrollToTracker(bool to_bottom) {
  QTreeView* view = ui_->logView;
  const int last = model_->revealedRows() - 1;
  if (last < 0) {
    view->scrollToTop();
    return;
  }
  view->scrollTo(
      model_->index(last, 0), to_bottom ? QAbstractItemView::PositionAtBottom : QAbstractItemView::EnsureVisible);
}

bool LogView::eventFilter(QObject* watched, QEvent* event) {
  if (playing_) {
    if (event->type() == QEvent::Wheel) {
      return true;
    }
    if (event->type() == QEvent::KeyPress && isNavigationKey(static_cast<QKeyEvent*>(event)->key())) {
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

void LogView::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  // Absolute splitter sizes only stick once the splitter has its real geometry.
  if (!splitter_sized_) {
    splitter_sized_ = true;
    // About 8 lines of the pane's font plus its document margins and frame; the list keeps the rest.
    const int detail = ui_->detailText->fontMetrics().lineSpacing() * 8 +
                       static_cast<int>(ui_->detailText->document()->documentMargin() * 2) +
                       ui_->detailText->frameWidth() * 2 + 8;  // + the pane's 4 px top/bottom margins
    ui_->splitter->setSizes({std::max(1, height() - detail), detail});
  }
}

int LogView::fitWidth(const QString& text, int column) const {
  QHeaderView* header = ui_->logView->header();
  // Cell text gets the item margins; the header gets its section padding (and a sort/arrow slack).
  const int cell_pad = ui_->logView->style()->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, ui_->logView) * 2 + 8;
  const int head_pad = header->style()->pixelMetric(QStyle::PM_HeaderMargin, nullptr, header) * 2 + 8;
  const QString title = model_->headerData(column, Qt::Horizontal).toString();
  return std::max(
      ui_->logView->fontMetrics().horizontalAdvance(text) + cell_pad,
      header->fontMetrics().horizontalAdvance(title) + head_pad);
}

void LogView::fitTimeColumn(bool over_user) {
  if (store_.size() == 0) {
    if (over_user) {
      time_width_ = 0;  // the next rows fit it
    }
    return;
  }
  // The store is in time order: its first and last rows bound every time text, the rows not shown yet included.
  const int needed = std::max(
      fitWidth(model_->timeText(store_.ref(0).time), LogTableModel::kTime),
      fitWidth(model_->timeText(store_.ref(store_.size() - 1).time), LogTableModel::kTime));
  QHeaderView* header = ui_->logView->header();
  const int width = header->sectionSize(LogTableModel::kTime);
  // Never over a width the user dragged: columns do not move under the user.
  if (time_width_ != 0 && !over_user && (width != time_width_ || needed == width)) {
    return;
  }
  HeaderResizePolicy::install(header)->setSectionWidth(LogTableModel::kTime, needed);
  time_width_ = header->sectionSize(LogTableModel::kTime);
}

void LogView::autoSizeColumns() {
  fitTimeColumn(/*over_user=*/false);
  if (level_fitted_ || store_.size() == 0) {
    return;
  }
  QString widest_level;
  for (const log_view::LevelName& name : log_view::kLevelNames) {
    const QString level = LogTableModel::levelName(name.level);
    if (ui_->logView->fontMetrics().horizontalAdvance(level) >
        ui_->logView->fontMetrics().horizontalAdvance(widest_level)) {
      widest_level = level;
    }
  }
  HeaderResizePolicy::install(ui_->logView->header())
      ->setSectionWidth(LogTableModel::kLevel, fitWidth(widest_level, LogTableModel::kLevel));
  level_fitted_ = true;
}

void LogView::onRowDoubleClicked(const QModelIndex& index) {
  if (!index.isValid() || playing_) {
    return;
  }
  const std::optional<size_t> store_index = model_->storeIndex(index.row());
  if (!store_index || *store_index >= store_.size()) {
    return;
  }
  const double time = store_.ref(*store_index).time;
  pending_seek_ = time;
  emit seekRequested(time);
}

LogViewState LogView::state() const {
  LogViewState state;
  const auto toggles = levelToggles();
  for (size_t i = 0; i < toggles.size(); ++i) {
    state.levels[i] = toggles[i]->isChecked();
  }
  state.source = ui_->sourceEdit->text();
  state.category = ui_->categoryEdit->text();
  state.text = ui_->filterEdit->text();
  state.show_future = ui_->futureButton->isChecked();
  return state;
}

void LogView::applyState(const LogViewState& state) {
  const auto toggles = levelToggles();
  for (size_t i = 0; i < toggles.size(); ++i) {
    const QSignalBlocker blocker(toggles[i]);
    toggles[i]->setChecked(state.levels[i]);
  }
  for (const auto& [edit, text] :
       {std::pair{ui_->sourceEdit, &state.source}, std::pair{ui_->categoryEdit, &state.category},
        std::pair{ui_->filterEdit, &state.text}}) {
    const QSignalBlocker blocker(edit);
    edit->setText(*text);
  }
  refreshOptionalColumns();  // a restored filter keeps its field visible
  setShowFuture(state.show_future);
  applyFilters();
}

}  // namespace PJ
