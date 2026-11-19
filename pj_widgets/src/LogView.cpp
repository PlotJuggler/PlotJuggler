// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/LogView.h"

#include <QAction>
#include <QClipboard>
#include <QEvent>
#include <QGuiApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QTextDocument>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

#include "pj_widgets/HeaderResizePolicy.h"
#include "pj_widgets/Scrollbar.h"
#include "ui_LogView.h"

namespace PJ {

using log_view::LogRow;
using log_view::LogStore;
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
  ui_->categoryEdit->hide();  // until an entry has a category
  ui_->futureButton->setIconPath(QString::fromLatin1(kFutureShownIcon));
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
  ui_->logView->setColumnHidden(LogTableModel::kCategory, true);
  ui_->logView->setColumnHidden(LogTableModel::kTopic, true);
  attachPillScrollbars(this);

  ui_->logView->installEventFilter(this);
  ui_->logView->viewport()->installEventFilter(this);
  ui_->logView->verticalScrollBar()->installEventFilter(this);

  for (QToolButton* toggle : {ui_->levelDebug, ui_->levelInfo, ui_->levelWarn, ui_->levelError, ui_->levelFatal}) {
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
  connect(ui_->logView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &LogView::updateDetail);
  connect(model_, &QAbstractItemModel::modelReset, this, &LogView::updateDetail);
  connect(ui_->copyButton, &QToolButton::clicked, this, [this]() {
    QGuiApplication::clipboard()->setText(ui_->detailText->toPlainText());
  });
}

LogView::~LogView() = default;

void LogView::setTopicLabel(uint32_t topic, const QString& label) {
  if (topics_.contains(topic) && topics_.at(topic) == label) {
    return;
  }
  topics_[topic] = label;
  refreshTopics();
  if (currentFilter().hidden_wires != applied_hidden_wires_) {
    applyFilters();  // a returning topic the user hid: its rows stay hidden
  }
}

std::vector<LogRow> LogView::toRows(uint32_t topic, std::vector<LogEntry> entries) {
  std::vector<LogRow> rows;
  rows.reserve(entries.size());
  uint64_t& seq = next_seq_[topic];
  for (LogEntry& entry : entries) {
    if (!std::isfinite(entry.time)) {
      continue;
    }
    LogRow row;
    row.time = entry.time;
    row.stamp_ns = entry.stamp_ns;
    row.seq = seq++;
    row.level = entry.level;
    row.source = store_.intern(entry.source);
    row.category = store_.internCategory(entry.category);
    row.message = std::move(entry.message);
    log_view::stripTerminalEscapes(row.message);
    rows.push_back(std::move(row));
  }
  return rows;
}

void LogView::appendEntries(uint32_t topic, std::vector<LogEntry> entries) {
  if (entries.empty()) {
    return;
  }
  const std::optional<size_t> selected = selectedStoreRow();
  const auto key = selected ? std::optional(store_.keyOf(*selected)) : std::nullopt;
  const StoreChange change = store_.insert(topic, toRows(topic, std::move(entries)));
  followChange(change, selected, key);
}

void LogView::replaceEntries(uint32_t topic, std::vector<LogEntry> entries) {
  const std::optional<size_t> selected = selectedStoreRow();
  const auto key = selected ? std::optional(store_.keyOf(*selected)) : std::nullopt;
  next_seq_[topic] = 0;  // arrival numbers restart with a reload; RowKey re-finds the selection
  const StoreChange change = store_.replaceWire(topic, toRows(topic, std::move(entries)));
  followChange(change, selected, key);
}

void LogView::removeTopic(uint32_t topic) {
  const std::optional<size_t> selected = selectedStoreRow();
  const auto key = selected ? std::optional(store_.keyOf(*selected)) : std::nullopt;
  const StoreChange change = store_.removeWire(topic);
  topics_.erase(topic);
  next_seq_.erase(topic);
  refreshTopics();
  followChange(change, selected, key);
}

void LogView::followChange(
    const StoreChange& change, std::optional<size_t> selected, std::optional<LogStore::RowKey> selected_key) {
  // The detail pane's row is stale from old_tail on, and an eviction shifts every index. Checked before the model
  // follows the store: its signals may refresh the pane, in new coordinates.
  if (detail_row_ && (change.reset || change.dropped_front > 0 || *detail_row_ >= change.old_tail)) {
    detail_valid_ = false;
  }
  model_->applyStoreChange(change);
  if (store_.categories().size() != categories_listed_) {
    refreshCategories();
  }
  model_->setTracker(tracker_);
  if (selected) {
    // A row the change left in place moves by the eviction alone; its rank among equal times may not hold.
    std::optional<size_t> now = change.kept(*selected);
    if (!now && (change.reset || *selected >= change.old_tail)) {
      now = store_.indexOf(*selected_key);
    }
    if (now && now != selectedStoreRow()) {
      selectStoreRow(now);
    }
  }
  if (playing_) {
    scrollToTracker(true);
  } else if (!shown_rows_ && model_->rowCount() > 0) {
    scrollToTracker(false);
  }
  if (model_->rowCount() > 0) {
    shown_rows_ = true;
    autoSizeColumns();
  }
  updateDetail();
}

void LogView::setTrackerTime(double time) {
  const bool moved = std::isnan(tracker_) != std::isnan(time) || (!std::isnan(time) && !sameTime(time, tracker_));
  tracker_ = time;
  model_->setTracker(time);
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
  model_->setTimeLabelFormat(format, decimals);
  // The texts change width: fit the column again, even over a width the user set.
  fitTimeColumn();
  detail_valid_ = false;
  updateDetail();
}

void LogView::setShowFuture(bool show) {
  {
    const QSignalBlocker blocker(ui_->futureButton);
    ui_->futureButton->setChecked(show);
  }
  ui_->futureButton->setIconPath(QString::fromLatin1(show ? kFutureShownIcon : kFutureHiddenIcon));
  ui_->futureButton->setToolTip(
      show ? tr("Show the entries after the playhead, greyed") : tr("Hide the entries after the playhead"));
  const std::optional<size_t> selected = selectedStoreRow();
  model_->setFutureRows(show ? log_view::FutureRows::kGrey : log_view::FutureRows::kHide);
  selectStoreRow(selected);
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
  for (const auto& [id, label] : topics_) {
    if (hidden_topics_.contains(label)) {
      filter.hidden_wires.push_back(id);
    }
  }
  return filter;
}

void LogView::applyFilters() {
  text_timer_.stop();
  const std::optional<size_t> selected = selectedStoreRow();
  const log_view::LogFilter filter = currentFilter();
  applied_hidden_wires_ = filter.hidden_wires;
  model_->setFilter(filter);
  selectStoreRow(selected);
  if (playing_) {
    scrollToTracker(true);
  } else if (const QModelIndex current = ui_->logView->currentIndex(); current.isValid()) {
    ui_->logView->scrollTo(current, QAbstractItemView::EnsureVisible);
  } else {
    scrollToTracker(false);
  }
  updateDetail();
}

std::optional<size_t> LogView::selectedStoreRow() const {
  const QModelIndex current = ui_->logView->currentIndex();
  if (!current.isValid() || !ui_->logView->selectionModel()->isRowSelected(current.row(), QModelIndex())) {
    return std::nullopt;
  }
  return model_->storeIndex(current.row());
}

void LogView::selectStoreRow(std::optional<size_t> store_index) {
  if (!store_index) {
    return;
  }
  const int row = model_->rowOf(*store_index);
  if (row < 0) {
    return;
  }
  const bool auto_scroll = ui_->logView->hasAutoScroll();
  ui_->logView->setAutoScroll(false);  // re-selecting must not move the view by itself
  ui_->logView->selectionModel()->setCurrentIndex(
      model_->index(row, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  ui_->logView->setAutoScroll(auto_scroll);
}

void LogView::refreshTopics() {
  QMenu* menu = ui_->topicsButton->menu();
  menu->clear();
  for (const auto& [id, label] : topics_) {
    QAction* action = menu->addAction(label);
    action->setCheckable(true);
    action->setChecked(!hidden_topics_.contains(label));
    connect(action, &QAction::toggled, this, [this, label](bool shown) {
      if (shown) {
        hidden_topics_.removeAll(label);
      } else if (!hidden_topics_.contains(label)) {
        hidden_topics_.append(label);
      }
      applyFilters();
    });
    connect(action, &QAction::triggered, this, &LogView::stateEdited);
  }
  const bool several = topics_.size() > 1;
  ui_->topicsButton->setVisible(several);
  ui_->logView->setColumnHidden(LogTableModel::kTopic, !several);
  ui_->pages->setCurrentWidget(topics_.empty() ? ui_->emptyPage : ui_->contentPage);
}

void LogView::refreshCategories() {
  categories_listed_ = store_.categories().size();
  // Many logs have no category: the column and the filter appear once an entry has one (or a restored filter needs
  // its field).
  ui_->categoryEdit->setVisible(!store_.categories().empty() || !ui_->categoryEdit->text().isEmpty());
  ui_->logView->setColumnHidden(LogTableModel::kCategory, store_.categories().empty());
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
    const LogRow& row = store_.row(*selected);
    QStringList lines;
    lines << tr("Time: %1").arg(model_->timeText(row));
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

bool LogView::fitTimeColumn() {
  if (model_->rowCount() == 0) {
    return false;
  }
  const std::optional<size_t> last = model_->storeIndex(model_->rowCount() - 1);
  if (!last || *last >= store_.size()) {
    return false;
  }
  HeaderResizePolicy::install(ui_->logView->header())
      ->setSectionWidth(LogTableModel::kTime, fitWidth(model_->timeText(store_.row(*last)), LogTableModel::kTime));
  return true;
}

void LogView::autoSizeColumns() {
  // Once: later growth of the times does not move the columns under the user.
  if (columns_fitted_ || !fitTimeColumn()) {
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
  columns_fitted_ = true;
}

void LogView::onRowDoubleClicked(const QModelIndex& index) {
  if (!index.isValid() || playing_) {
    return;
  }
  const std::optional<size_t> store_index = model_->storeIndex(index.row());
  if (!store_index || *store_index >= store_.size()) {
    return;
  }
  const double time = store_.row(*store_index).time;
  pending_seek_ = time;
  emit seekRequested(time);
}

LogViewState LogView::state() const {
  LogViewState state;
  const std::array<QToolButton*, log_view::kToggleLevelCount> toggles = {
      ui_->levelDebug, ui_->levelInfo, ui_->levelWarn, ui_->levelError, ui_->levelFatal};
  for (size_t i = 0; i < toggles.size(); ++i) {
    state.levels[i] = toggles[i]->isChecked();
  }
  state.source = ui_->sourceEdit->text();
  state.category = ui_->categoryEdit->text();
  state.text = ui_->filterEdit->text();
  state.hidden_topics = hidden_topics_;
  state.show_future = ui_->futureButton->isChecked();
  return state;
}

void LogView::applyState(const LogViewState& state) {
  const std::array<QToolButton*, log_view::kToggleLevelCount> toggles = {
      ui_->levelDebug, ui_->levelInfo, ui_->levelWarn, ui_->levelError, ui_->levelFatal};
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
  refreshCategories();  // a restored category filter keeps its field visible
  hidden_topics_ = state.hidden_topics;
  refreshTopics();
  setShowFuture(state.show_future);
  applyFilters();
}

}  // namespace PJ
