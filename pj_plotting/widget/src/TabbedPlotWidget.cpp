// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/TabbedPlotWidget.h"

#include <DockManager.h>

#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <utility>

#include "pj_plotting/PlotDocker.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgUtil.h"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {
// Compact tab-strip height. We aim for the tab column's *total* chrome
// (the strip itself plus the 1-px tabs_separator drawn underneath) to
// match the Sources/Datasets header bars, which are 24 px and have no
// separator below. So the strip itself is 23 px and the separator
// adds the final 1 px → 24 total — keeping the plot content's top
// edge pixel-aligned with the curve-list content on the left column.
constexpr int kTabBarHeight = 23;
// Tab/button footprint inside the bar — flush with the bar height
// (no breathing room) because tabs have their own coloured backgrounds;
// any gap would show a strip of bar-background above and below the tab.
// The [+] and panel-toggle buttons match so the whole row is uniform.
constexpr int kTabBarButtonSize = 23;
// Icon size — matches the title bar's 20-px glyph so the tab-strip
// buttons read at the same visual weight as the rest of the chrome.
constexpr int kTabBarIconSize = 20;

// QScrollArea variant that redirects vertical wheel input to the
// horizontal scrollbar. Lets the user wheel-scroll an overflowing
// tab strip the same way Chrome / VSCode do, without us needing to
// show a (visually heavy) horizontal scrollbar.
class HorizontalScrollArea : public QScrollArea {
 public:
  using QScrollArea::QScrollArea;

 protected:
  void wheelEvent(QWheelEvent* event) override {
    QScrollBar* hbar = horizontalScrollBar();
    const int delta = event->angleDelta().y();
    if (hbar != nullptr && delta != 0) {
      hbar->setValue(hbar->value() - delta);
      event->accept();
      return;
    }
    QScrollArea::wheelEvent(event);
  }
};

// ADS config flags are process-global and must be set before the first
// CDockManager is instantiated. Call once, lazily.
void applyAdsConfigOnce() {
  static bool done = false;
  if (done) {
    return;
  }
  ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasTabsMenuButton, false);
  ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasUndockButton, false);
  ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasCloseButton, false);
  ads::CDockManager::setConfigFlag(ads::CDockManager::EqualSplitOnInsertion, true);
  ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
  ads::CDockManager::setConfigFlag(ads::CDockManager::DragPreviewIsDynamic, true);
  ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
  done = true;
}
}  // namespace

// Small private widget for one tab in the strip. Holds a name label
// plus a close button. Double-clicking flips the label out for an
// in-place QLineEdit; pressing Enter commits the rename, Escape or
// loss of focus reverts to the previous name.
class PlotTabFrame : public QFrame {
  Q_OBJECT
 public:
  explicit PlotTabFrame(const QString& tab_name, QWidget* parent = nullptr) : QFrame(parent) {
    setObjectName("plotTabFrame");
    setProperty("selected", false);
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);
    // Fill the bar height exactly — tabs have a colored background, so
    // any breathing room above or below would render as a visible
    // 1-px stripe of bar-background flanking the tab.
    setFixedHeight(kTabBarButtonSize);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(
        PJ::theme::space(theme::Space::Comfortable), PJ::theme::space(theme::Space::None),
        PJ::theme::space(theme::Space::Snug), PJ::theme::space(theme::Space::None));
    layout->setSpacing(PJ::theme::space(theme::Space::Comfortable));

    label_ = new QLabel(tab_name, this);
    label_->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(label_);

    edit_ = new QLineEdit(this);
    edit_->setObjectName("plotTabRenameEdit");
    edit_->hide();
    edit_->installEventFilter(this);
    connect(edit_, &QLineEdit::returnPressed, this, &PlotTabFrame::commitEdit);
    layout->addWidget(edit_);

    close_button_ = new QPushButton(this);
    close_button_->setFlat(true);
    close_button_->setFixedSize(QSize(16, 16));
    close_button_->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(close_button_);
    connect(close_button_, &QPushButton::clicked, this, &PlotTabFrame::closeRequested);
  }

  void setName(const QString& tab_name) {
    label_->setText(tab_name);
  }

  [[nodiscard]] QString name() const {
    return label_->text();
  }

  void setSelected(bool selected) {
    if (property("selected").toBool() == selected) {
      return;
    }
    setProperty("selected", selected);
    style()->unpolish(this);
    style()->polish(this);
    update();
  }

  [[nodiscard]] QPushButton* closeButton() const {
    return close_button_;
  }

  void enterEditMode() {
    if (edit_active_) {
      return;
    }
    edit_active_ = true;
    edit_->setText(label_->text());
    label_->hide();
    edit_->show();
    edit_->setFocus(Qt::OtherFocusReason);
    edit_->setCursorPosition(edit_->text().length());
  }

 signals:
  void clicked();
  void renameCommitted(const QString& new_name);
  void closeRequested();

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton) {
      emit clicked();
    }
    QFrame::mousePressEvent(event);
  }

  void mouseDoubleClickEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton) {
      enterEditMode();
    }
    QFrame::mouseDoubleClickEvent(event);
  }

  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == edit_ && edit_active_) {
      if (event->type() == QEvent::FocusOut) {
        // Reverting on focus loss is the user-requested behaviour;
        // explicit commit only happens on Enter.
        cancelEdit();
      } else if (event->type() == QEvent::KeyPress) {
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
          cancelEdit();
          return true;
        }
      }
    }
    return QFrame::eventFilter(watched, event);
  }

 private slots:
  void commitEdit() {
    if (!edit_active_) {
      return;
    }
    const QString new_text = edit_->text();
    // An empty name is never useful — and for a widget tab it would silently
    // fall back to the default label on layout restore (the empty rename is
    // not serialized). Treat it as a cancelled rename.
    if (new_text.trimmed().isEmpty()) {
      cancelEdit();
      return;
    }
    edit_active_ = false;
    label_->setText(new_text);
    edit_->hide();
    label_->show();
    setFocus(Qt::OtherFocusReason);
    emit renameCommitted(new_text);
  }

  void cancelEdit() {
    if (!edit_active_) {
      return;
    }
    edit_active_ = false;
    edit_->hide();
    label_->show();
  }

 private:
  QLabel* label_;
  QLineEdit* edit_;
  QPushButton* close_button_;
  bool edit_active_ = false;
};

TabbedPlotWidget::TabbedPlotWidget(QWidget* parent) : TabbedPlotWidget(u"main"_s, parent) {}

TabbedPlotWidget::TabbedPlotWidget(QString name, QWidget* parent) : QWidget(parent), name_(std::move(name)) {
  applyAdsConfigOnce();
  setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));

  auto* root_layout = new QVBoxLayout(this);
  root_layout->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  root_layout->setSpacing(PJ::theme::space(theme::Space::None));

  // Tab strip — outer hbox with two regions:
  //   * a horizontally scrollable area containing the [+] add button
  //     and one PlotTabFrame per tab (left-aligned via a trailing
  //     stretch). When the tabs together exceed the viewport, the
  //     scroll area shows a horizontal scrollbar instead of squeezing
  //     the panel buttons off-screen.
  //   * the three panel-toggle buttons, pinned at the far right.
  auto* tabs_bar_widget = new QWidget(this);
  tabs_bar_widget->setObjectName("plotTabsBar");
  tabs_bar_widget->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  auto* outer_layout = new QHBoxLayout(tabs_bar_widget);
  outer_layout->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  outer_layout->setSpacing(PJ::theme::space(theme::Space::None));

  // QScrollArea-based wrapper removed — the scroll area's viewport was
  // reserving a couple of pixels of vertical chrome that pushed the
  // tabs and [+] button down inside the 24-px bar, even with both
  // scrollbar policies set to AlwaysOff. Without it, overflowing tabs
  // simply squish via QHBoxLayout's default behavior.
  //
  //   auto* tabs_scroll = new HorizontalScrollArea(tabs_bar_widget);
  //   tabs_scroll->setObjectName("plotTabsScrollArea");
  //   tabs_scroll->setFrameShape(QFrame::NoFrame);
  //   tabs_scroll->setWidgetResizable(true);
  //   tabs_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  //   tabs_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  //   tabs_scroll->setFixedHeight(kTabBarHeight);
  //   tabs_scroll->setWidget(tabs_inner);
  //   outer_layout->addWidget(tabs_scroll, 1);
  tabs_inner_ = new QWidget(tabs_bar_widget);
  tabs_inner_->setObjectName("plotTabsInner");
  tabs_inner_->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  tabs_inner_->setFixedHeight(kTabBarHeight);
  tabs_bar_layout_ = new QHBoxLayout(tabs_inner_);
  tabs_bar_layout_->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  tabs_bar_layout_->setSpacing(PJ::theme::space(theme::Space::None));

  button_add_tab_ = new QPushButton(this);
  button_add_tab_->setFlat(true);
  button_add_tab_->setFixedSize(QSize(kTabBarButtonSize, kTabBarButtonSize));
  button_add_tab_->setIconSize(QSize(kTabBarIconSize, kTabBarIconSize));
  button_add_tab_->setFocusPolicy(Qt::NoFocus);
  connect(button_add_tab_, &QPushButton::clicked, this, &TabbedPlotWidget::onAddTabButtonPressed);
  tabs_bar_layout_->addWidget(button_add_tab_, 0, Qt::AlignVCenter);
  tabs_bar_layout_->addStretch(1);

  outer_layout->addWidget(tabs_inner_, 1);

  // Panel-toggle buttons control the surrounding shell panels (left
  // column, timeline, right toolbar). Created here so the accessors and
  // the MainWindow wiring keep working, but NOT mounted in the tab
  // strip: the shell reparents them into the title bar's right cluster
  // (TitleBar::addRightClusterWidget) after construction.
  auto make_panel_button = [this](const char* tip) {
    auto* button = new QPushButton(this);
    button->setObjectName(u"plotTabsPanelButton"_s);
    button->setFlat(true);
    // Not checkable — MainWindow swaps the glyph between filled (panel
    // visible) and outlined (panel hidden) variants on click.
    button->setFixedSize(QSize(kTabBarButtonSize, kTabBarButtonSize));
    button->setIconSize(QSize(kTabBarIconSize, kTabBarIconSize));
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(QObject::tr(tip));
    return button;
  };
  button_left_panel_ = make_panel_button(QT_TR_NOOP("Toggle left panel"));
  button_bottom_panel_ = make_panel_button(QT_TR_NOOP("Toggle bottom panel"));
  button_right_panel_ = make_panel_button(QT_TR_NOOP("Toggle right panel"));

  root_layout->addWidget(tabs_bar_widget);

  // 1px separator — same paint pattern as the title-bar / timeline /
  // right-toolbar dividers so all chrome lines look identical.
  auto* tabs_separator = new QFrame(this);
  tabs_separator->setObjectName("plotTabsSeparator");
  tabs_separator->setFrameShape(QFrame::NoFrame);
  tabs_separator->setAutoFillBackground(true);
  tabs_separator->setFixedHeight(1);
  root_layout->addWidget(tabs_separator);

  stack_ = new QStackedWidget(this);
  connect(stack_, &QStackedWidget::currentChanged, this, [this](int /*index*/) { adaptWidgetTabPagePolicies(); });
  stack_->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  root_layout->addWidget(stack_, 1);

  onStylesheetChanged(currentTheme());
  addTab({});
}

TabbedPlotWidget::~TabbedPlotWidget() = default;

PlotDocker* TabbedPlotWidget::currentTab() {
  return qobject_cast<PlotDocker*>(stack_->currentWidget());
}

int TabbedPlotWidget::dockerCount() const {
  int count = 0;
  for (const TabEntry& entry : tabs_) {
    if (entry.docker != nullptr) {
      ++count;
    }
  }
  return count;
}

int TabbedPlotWidget::historySerializableDockerCount() const {
  int count = 0;
  for (const TabEntry& entry : tabs_) {
    if (entry.docker != nullptr && entry.docker->isSerializable(SnapshotScope::kHistory)) {
      ++count;
    }
  }
  return count;
}

PlotDocker* TabbedPlotWidget::dockerAt(int index) {
  if (index < 0) {
    return nullptr;
  }
  for (TabEntry& entry : tabs_) {
    if (entry.docker != nullptr && index-- == 0) {
      return entry.docker;
    }
  }
  return nullptr;
}

int TabbedPlotWidget::tabCount() const {
  return static_cast<int>(tabs_.size());
}

void TabbedPlotWidget::forEachDocker(const std::function<void(PlotDocker*)>& operation) const {
  for (const TabEntry& entry : tabs_) {
    if (entry.docker != nullptr) {
      operation(entry.docker);
    }
  }
}

PlotDocker* TabbedPlotWidget::findDocker(const std::function<bool(PlotDocker*)>& predicate) const {
  for (const TabEntry& entry : tabs_) {
    if (entry.docker != nullptr && predicate(entry.docker)) {
      return entry.docker;
    }
  }
  return nullptr;
}

PlotDocker* TabbedPlotWidget::addTab(QString tab_name) {
  if (tab_name.isEmpty()) {
    tab_name = QString("tab%1").arg(++tab_suffix_count_);
  }
  PlotDocker* docker = createDocker(tab_name);
  PlotTabFrame* frame = createTabFrame(tab_name);

  stack_->addWidget(docker);
  // Insert frame just before the add-button (which is at index 0 after
  // the stretch we appended) so tabs grow leftward of the [+] button.
  // Explicit AlignVCenter — same rationale as the [+] button: matches
  // the 1-px-breathing-room rhythm of the rest of the chrome bars.
  const int insert_index = static_cast<int>(tabs_.size());
  tabs_bar_layout_->insertWidget(insert_index, frame, 0, Qt::AlignVCenter);
  tabs_.push_back({.frame = frame, .docker = docker});

  emit tabAdded(docker);

  stack_->setCurrentWidget(docker);
  updateSelectionStyle();
  emit currentTabChanged(docker);
  return docker;
}

void TabbedPlotWidget::addWidgetTab(const QString& tab_name, QWidget* content, std::function<void()> on_close) {
  if (content == nullptr || findEntry(content) != nullptr) {
    return;
  }
  PlotTabFrame* frame = createTabFrame(tab_name);

  stack_->addWidget(content);
  const int insert_index = static_cast<int>(tabs_.size());
  tabs_bar_layout_->insertWidget(insert_index, frame, 0, Qt::AlignVCenter);
  tabs_.push_back(
      {.frame = frame, .widget = content, .on_close = std::move(on_close), .original_policy = content->sizePolicy()});

  stack_->setCurrentWidget(content);
  updateSelectionStyle();
  emit currentTabChanged(nullptr);
}

void TabbedPlotWidget::focusWidgetTab(QWidget* content) {
  if (const TabEntry* entry = findWidgetEntry(content)) {
    selectEntry(*entry);
  }
}

void TabbedPlotWidget::closeWidgetTab(QWidget* content) {
  if (const TabEntry* entry = findWidgetEntry(content)) {
    closeTab(entry->frame, /*honor_veto=*/true);
  }
}

QWidget* TabbedPlotWidget::takeWidgetTab(QWidget* content) {
  const TabEntry* entry = findWidgetEntry(content);
  if (entry == nullptr) {
    return nullptr;
  }
  PlotTabFrame* frame = entry->frame;
  const QSizePolicy original_policy = entry->original_policy;
  const auto it =
      std::find_if(tabs_.begin(), tabs_.end(), [content](const TabEntry& tab) { return tab.widget == content; });
  tabs_.erase(it);
  stack_->removeWidget(content);
  tabs_bar_layout_->removeWidget(frame);
  frame->deleteLater();
  content->hide();
  content->setParent(nullptr);
  content->setSizePolicy(original_policy);
  updateSelectionStyle();
  emit currentTabChanged(qobject_cast<PlotDocker*>(stack_->currentWidget()));
  return content;
}

void TabbedPlotWidget::closeTab(PlotDocker* docker) {
  if (docker == nullptr) {
    return;
  }
  if (const TabEntry* entry = findEntry(static_cast<QWidget*>(docker))) {
    closeTab(entry->frame, /*honor_veto=*/true);
  }
}

void TabbedPlotWidget::closeWidgetTabForced(QWidget* content) {
  if (const TabEntry* entry = findWidgetEntry(content)) {
    closeTab(entry->frame, /*honor_veto=*/false);
  }
}

void TabbedPlotWidget::setWidgetTabPreClose(QWidget* content, std::function<bool()> pre_close) {
  if (TabEntry* entry = findWidgetEntry(content)) {
    entry->pre_close = std::move(pre_close);
  }
}

QString TabbedPlotWidget::widgetTabName(QWidget* content) const {
  const TabEntry* entry = findWidgetEntry(content);
  return entry != nullptr ? entry->frame->name() : QString{};
}

void TabbedPlotWidget::setWidgetTabName(QWidget* content, const QString& name) {
  if (const TabEntry* entry = findWidgetEntry(content)) {
    entry->frame->setName(name);
  }
}

void TabbedPlotWidget::setDataServices(SessionManager* session, CatalogModel* catalog) {
  session_ = session;
  catalog_ = catalog;
  for (const TabEntry& entry : tabs_) {
    if (entry.docker != nullptr) {
      entry.docker->setDataServices(session_, catalog_);
    }
  }
}

void TabbedPlotWidget::setObjectWidgetFactory(ObjectWidgetFactory factory) {
  object_widget_factory_ = std::move(factory);
  for (const TabEntry& entry : tabs_) {
    if (entry.docker != nullptr) {
      entry.docker->setObjectWidgetFactory(object_widget_factory_);
    }
  }
}

void TabbedPlotWidget::onAddTabButtonPressed() {
  addTab({});
  emit undoableChange();
}

void TabbedPlotWidget::onTabFrameClicked(PlotTabFrame* frame) {
  if (TabEntry* entry = findEntry(frame)) {
    selectEntry(*entry);
  }
}

void TabbedPlotWidget::onTabRenameRequested(PlotTabFrame* frame, const QString& new_name) {
  TabEntry* entry = findEntry(frame);
  if (entry == nullptr) {
    return;
  }
  if (entry->docker == nullptr) {
    // Widget tab: the frame label (already updated by the in-place editor)
    // is the sole name store, and widget tabs live outside the undo
    // snapshot — so nothing to propagate and no undo state to push.
    return;
  }
  entry->docker->setName(new_name);
  emit undoableChange();
}

void TabbedPlotWidget::onTabCloseRequested(PlotTabFrame* frame) {
  closeTab(frame, /*honor_veto=*/true);
}

void TabbedPlotWidget::closeTab(PlotTabFrame* frame, bool honor_veto) {
  TabEntry* entry = findEntry(frame);
  if (entry == nullptr) {
    return;
  }
  // The veto is consulted before ANY side effect — including the last-plot-tab
  // spawn below — so a declined close leaves the tab set exactly as it was.
  if (honor_veto && entry->pre_close) {
    const std::function<bool()> pre_close = entry->pre_close;
    const QPointer<PlotTabFrame> frame_guard(frame);
    if (!pre_close()) {
      return;
    }
    // pre_close may have run a modal event loop: tabs_ can have been
    // reallocated, and this very tab closed (deleting its frame) meanwhile.
    if (frame_guard.isNull()) {
      return;
    }
    entry = findEntry(frame);
    if (entry == nullptr) {
      return;
    }
  }
  // Keep one ordinary plot tab available for history snapshots. Widget and
  // history-exempt pages cannot satisfy that invariant.
  if (entry->docker != nullptr && entry->docker->isSerializable(SnapshotScope::kHistory) &&
      historySerializableDockerCount() == 1) {
    onAddTabButtonPressed();
    entry = findEntry(frame);  // vector reallocated above
    if (entry == nullptr) {
      return;
    }
  }

  const bool is_widget_tab = entry->docker == nullptr;
  QWidget* content = contentOf(*entry);
  PlotTabFrame* frame_widget = entry->frame;
  // Owner teardown runs before the widget dies. Move the callback out first
  // so a reentrant close of the same tab (owner teardown looping back
  // through closeWidgetTab) finds it empty instead of running it twice.
  const std::function<void()> on_close = std::move(entry->on_close);
  entry->on_close = nullptr;
  // Past the veto the close is committed, so a reentrant close of this tab must
  // not put the question a second time.
  entry->pre_close = nullptr;
  if (on_close) {
    on_close();
  }
  entry = nullptr;  // on_close may reenter and shift tabs_; re-derive below.

  const auto it = std::find_if(
      tabs_.begin(), tabs_.end(), [frame_widget](const TabEntry& tab) { return tab.frame == frame_widget; });
  if (it == tabs_.end()) {
    return;  // a reentrant close already removed it
  }
  tabs_.erase(it);

  stack_->removeWidget(content);
  tabs_bar_layout_->removeWidget(frame_widget);
  frame_widget->deleteLater();
  content->deleteLater();

  if (stack_->currentWidget() == nullptr && !tabs_.empty()) {
    stack_->setCurrentWidget(contentOf(tabs_.front()));
  }
  updateSelectionStyle();
  emit currentTabChanged(qobject_cast<PlotDocker*>(stack_->currentWidget()));
  // Closing a pinned widget tab is not part of the plot workspace, so it
  // must not push an undo state (undo can never resurrect its content).
  if (!is_widget_tab) {
    emit undoableChange();
  }
}

TabbedPlotWidget::TabEntry* TabbedPlotWidget::findEntry(PlotTabFrame* frame) {
  for (TabEntry& entry : tabs_) {
    if (entry.frame == frame) {
      return &entry;
    }
  }
  return nullptr;
}

TabbedPlotWidget::TabEntry* TabbedPlotWidget::findEntry(QWidget* content) {
  for (TabEntry& entry : tabs_) {
    if (contentOf(entry) == content) {
      return &entry;
    }
  }
  return nullptr;
}

TabbedPlotWidget::TabEntry* TabbedPlotWidget::findWidgetEntry(QWidget* content) {
  if (content == nullptr) {
    return nullptr;
  }
  for (TabEntry& entry : tabs_) {
    if (entry.widget == content) {
      return &entry;
    }
  }
  return nullptr;
}

const TabbedPlotWidget::TabEntry* TabbedPlotWidget::findWidgetEntry(QWidget* content) const {
  return const_cast<TabbedPlotWidget*>(this)->findWidgetEntry(content);
}

QWidget* TabbedPlotWidget::contentOf(const TabEntry& entry) {
  return entry.docker != nullptr ? static_cast<QWidget*>(entry.docker) : entry.widget;
}

void TabbedPlotWidget::updateSelectionStyle() {
  QWidget* current = stack_->currentWidget();
  for (const TabEntry& entry : tabs_) {
    entry.frame->setSelected(contentOf(entry) == current);
  }
}

void TabbedPlotWidget::adaptWidgetTabPagePolicies() {
  // QStackedWidget sizes from the union of every page, so a hidden toolbox
  // page with a large hint would constrain the plot area even while
  // invisible: only the current page keeps its real policy (same pattern as
  // LeftPanel's input stack). Driven by the stack's own currentChanged so
  // every page switch enforces it, not just tab-strip clicks. The equality
  // check matters: setSizePolicy posts a LayoutRequest even for an
  // unchanged value, and this runs for every widget tab per switch.
  QWidget* current = stack_->currentWidget();
  for (const TabEntry& entry : tabs_) {
    if (entry.widget == nullptr) {
      continue;
    }
    QSizePolicy desired = entry.original_policy;
    if (entry.widget != current) {
      desired.setHorizontalPolicy(QSizePolicy::Ignored);
      desired.setVerticalPolicy(QSizePolicy::Ignored);
    }
    if (entry.widget->sizePolicy() != desired) {
      entry.widget->setSizePolicy(desired);
    }
  }
}

void TabbedPlotWidget::selectEntry(const TabEntry& entry) {
  stack_->setCurrentWidget(contentOf(entry));
  updateSelectionStyle();
  emit currentTabChanged(entry.docker);
}

PlotDocker* TabbedPlotWidget::createDocker(const QString& tab_name) {
  auto* docker = new PlotDocker(tab_name, session_, catalog_, this);
  docker->setObjectWidgetFactory(object_widget_factory_);
  connect(docker, &PlotDocker::undoableChange, this, &TabbedPlotWidget::undoableChange);
  return docker;
}

PlotTabFrame* TabbedPlotWidget::createTabFrame(const QString& tab_name) {
  auto* frame = new PlotTabFrame(tab_name, this);
  // Tab frames default to kTabBarButtonSize; rebind to the live chrome
  // extent so a tab added after the user customised icon metrics still
  // fills the bar.
  const int chrome_extent = std::max(
      1, (chrome_metrics_.icon_size + chrome_metrics_.icon_padding) - 1 + (2 * chrome_metrics_.layout_padding));
  frame->setFixedHeight(chrome_extent);
  frame->closeButton()->setIcon(loadSvg(":/resources/svg/close-button.svg", currentTheme()));
  connect(frame, &PlotTabFrame::clicked, this, [this, frame]() { onTabFrameClicked(frame); });
  connect(frame, &PlotTabFrame::renameCommitted, this, [this, frame](const QString& new_name) {
    onTabRenameRequested(frame, new_name);
  });
  connect(frame, &PlotTabFrame::closeRequested, this, [this, frame]() { onTabCloseRequested(frame); });
  return frame;
}

void TabbedPlotWidget::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  chrome_metrics_ = metrics;
  // The tab strip historically uses (icon + icon_padding − 1) for the
  // strip height and chrome buttons, plus a 1-px separator below for
  // alignment with full-extent left-column bands. Layout padding adds
  // on top, growing the strip uniformly.
  const int chrome_extent = std::max(1, (metrics.icon_size + metrics.icon_padding) - 1 + (2 * metrics.layout_padding));
  if (tabs_inner_ != nullptr) {
    tabs_inner_->setFixedHeight(chrome_extent);
  }
  // Only the add-tab button lives in the strip; the panel-toggle buttons are
  // reparented into the TitleBar right cluster, which sizes them to its own
  // chrome extent — sizing them here would overflow the title-bar row.
  if (button_add_tab_ != nullptr) {
    button_add_tab_->setFixedSize(QSize(chrome_extent, chrome_extent));
    button_add_tab_->setIconSize(QSize(metrics.icon_size, metrics.icon_size));
  }
  for (const TabEntry& entry : tabs_) {
    if (entry.frame != nullptr) {
      entry.frame->setFixedHeight(chrome_extent);
    }
  }
}

void TabbedPlotWidget::onStylesheetChanged(QString theme) {
  if (button_add_tab_ != nullptr) {
    button_add_tab_->setIcon(loadSvg(":/resources/svg/add.svg", theme));
  }
  // Panel-toggle button icons are owned by MainWindow because their
  // open/close glyph depends on the live visibility of the target
  // panel — knowledge this widget intentionally doesn't have.
  const QIcon close_icon = loadSvg(":/resources/svg/close-button.svg", theme);
  for (const TabEntry& entry : tabs_) {
    if (auto* close_btn = entry.frame->closeButton()) {
      close_btn->setIcon(close_icon);
    }
    if (entry.docker != nullptr) {
      entry.docker->onStylesheetChanged(theme);
    }
  }
}

QDomElement TabbedPlotWidget::xmlSaveState(QDomDocument& doc, SnapshotScope scope) const {
  QDomElement tabbed_area = doc.createElement(u"tabbed_widget"_s);
  tabbed_area.setAttribute(u"id"_s, state_id_);
  tabbed_area.setAttribute(u"name"_s, name_);
  tabbed_area.setAttribute(u"parent"_s, u"main_window"_s);

  // currentTabIndex counts only pages written at this scope. A current page
  // outside the scope restores selection to the first serialized plot tab.
  PlotDocker* current = qobject_cast<PlotDocker*>(stack_->currentWidget());
  int current_index = 0;
  int docker_index = 0;
  for (const TabEntry& entry : tabs_) {
    if (entry.docker == nullptr || !entry.docker->isSerializable(scope)) {
      continue;
    }
    QDomElement tab_element = entry.docker->xmlSaveState(doc);
    tab_element.setAttribute(u"tab_name"_s, entry.docker->name());
    tabbed_area.appendChild(tab_element);
    if (entry.docker == current) {
      current_index = docker_index;
    }
    ++docker_index;
  }
  QDomElement current_tab = doc.createElement(u"currentTabIndex"_s);
  current_tab.setAttribute(u"index"_s, current_index);
  tabbed_area.appendChild(current_tab);
  return tabbed_area;
}

bool TabbedPlotWidget::xmlLoadState(const QDomElement& tabbed_area, RestoreIntent intent) {
  if (tabbed_area.isNull() || tabbed_area.tagName() != "tabbed_widget"_L1) {
    return false;
  }

  setStateId(tabbed_area.attribute(u"id"_s));
  if (tabbed_area.hasAttribute(u"name"_s)) {
    name_ = tabbed_area.attribute(u"name"_s);
  }

  const SnapshotScope scope = intent == RestoreIntent::kHistory ? SnapshotScope::kHistory : SnapshotScope::kFull;

  QVector<QDomElement> target_tabs;
  for (QDomElement tab = tabbed_area.firstChildElement(u"Tab"_s); !tab.isNull();
       tab = tab.nextSiblingElement(u"Tab"_s)) {
    if (intent == RestoreIntent::kHistory && tab.attribute(u"history_exempt"_s) == u"1"_s) {
      continue;
    }
    target_tabs.push_back(tab);
  }
  if (target_tabs.isEmpty()) {
    return false;
  }

  restoring_state_ = true;

  // Lift pages outside this restore's authority out of the tab strip, rebuild
  // the target plot pages, then reattach the preserved live objects.
  std::vector<TabEntry> preserved_tabs;
  for (TabEntry& entry : tabs_) {
    if (entry.docker == nullptr || !entry.docker->isSerializable(scope)) {
      tabs_bar_layout_->removeWidget(entry.frame);
      preserved_tabs.push_back(std::move(entry));
      continue;
    }
    stack_->removeWidget(entry.docker);
    entry.docker->deleteLater();
    tabs_bar_layout_->removeWidget(entry.frame);
    entry.frame->deleteLater();
  }
  tabs_.clear();
  tab_suffix_count_ = 0;

  // Runs on EVERY exit below (success or failed docker load) so the
  // preserved tabs are never orphaned outside tabs_ (their stack pages were
  // never removed).
  const auto reattach_preserved_tabs = [this, &preserved_tabs]() {
    for (TabEntry& entry : preserved_tabs) {
      tabs_bar_layout_->insertWidget(static_cast<int>(tabs_.size()), entry.frame, 0, Qt::AlignVCenter);
      tabs_.push_back(std::move(entry));
    }
  };

  for (qsizetype target_index = 0; target_index < target_tabs.size(); ++target_index) {
    const QDomElement tab_element = target_tabs.at(target_index);
    const QString tab_name = tab_element.attribute(u"tab_name"_s, u"tab%1"_s.arg(target_index + 1));
    PlotDocker* docker = addTab(tab_name);
    docker->setStateId(tab_element.attribute(u"id"_s));
    docker->setName(tab_name);
    docker->setHistoryExempt(tab_element.attribute(u"history_exempt"_s) == u"1"_s);
    docker->setOwnerMetadata(tab_element.attribute(u"owner_plugin"_s), tab_element.attribute(u"tab_id"_s));
    if (!docker->xmlLoadState(tab_element)) {
      reattach_preserved_tabs();
      restoring_state_ = false;
      return false;
    }
  }
  reattach_preserved_tabs();

  const int requested_index = tabbed_area.firstChildElement(u"currentTabIndex"_s).attribute(u"index"_s, u"0"_s).toInt();
  // The saved index counts rebuilt (serialized) docker tabs only; after the
  // rebuild those occupy the leading slots, so clamp against target_tabs'
  // count (not dockerCount(), which also counts the re-appended widget and
  // history-exempt tabs).
  const int max_index = static_cast<int>(target_tabs.size()) - 1;
  const int current_index = std::clamp(requested_index, 0, std::max(0, max_index));
  if (PlotDocker* docker = dockerAt(current_index)) {
    stack_->setCurrentWidget(docker);
    updateSelectionStyle();
    emit currentTabChanged(docker);
  }
  restoring_state_ = false;
  return true;
}

}  // namespace PJ

#include "TabbedPlotWidget.moc"
